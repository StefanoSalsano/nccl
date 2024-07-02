/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "comm.h"
#include "info.h"
#include "bootstrap.h"
#define ENABLE_TIMER 0
#include "timer.h"
#include "transport.h"

struct ncclTransport* ncclTransports[NTRANSPORTS] = {
  &p2pTransport,
  &shmTransport,
  &netTransport,
  &collNetTransport
};

template <int type>
static ncclResult_t selectTransport(struct ncclComm* comm, struct ncclTopoGraph* graph, struct ncclConnect* connect, int channelId, int peer, int connIndex, int* transportType) {
  struct ncclPeerInfo* myInfo = comm->peerInfo+comm->rank;
  struct ncclPeerInfo* peerInfo = comm->peerInfo+peer;
  struct ncclConnector* connector = (type == 1) ? comm->channels[channelId].peers[peer]->send + connIndex :
                                                  comm->channels[channelId].peers[peer]->recv + connIndex;
  for (int t=0; t<NTRANSPORTS; t++) {
    struct ncclTransport *transport = ncclTransports[t];
    struct ncclTransportComm* transportComm = type == 1 ? &transport->send : &transport->recv;
    int ret = 0;
    NCCLCHECK(transport->canConnect(&ret, comm->topo, graph, myInfo, peerInfo));
    if (ret) {
      connector->transportComm = transportComm;
      // in our case sendSetup or recvSetup in transport/net.cc
      NCCLCHECK(transportComm->setup(comm, graph, myInfo, peerInfo, connect, connector, channelId, connIndex));
      if (transportType) *transportType = t;
      return ncclSuccess;
    }
  }
  WARN("No transport found for rank %d[%lx] -> rank %d[%lx]", myInfo->rank, myInfo->busId, peerInfo->rank, peerInfo->busId);
  return ncclSystemError;
}

// connect with previous and next node in RING or TREE
// only sets the intent to connect, then the connections are created in ncclTransportP2pSetup
ncclResult_t ncclTransportP2pConnect(struct ncclComm* comm, int channelId, int nrecv, int* peerRecv, int nsend, int* peerSend, int connIndex) {
  TRACE(NCCL_INIT, "nsend %d nrecv %d", nsend, nrecv);
  INFO(NCCL_ALL,"ncclTransportP2pConnect : nsend %d nrecv %d channelId %d peerRecv[0] %d peerSend[0] %d", 
                                            nsend, nrecv, channelId, peerRecv[0], peerSend[0]);
  struct ncclChannel* channel = &comm->channels[channelId];
  uint64_t mask = 1UL << channel->id;
  for (int i=0; i<nrecv; i++) {
    int peer = peerRecv[i];
    if (peer == -1 || peer >= comm->nRanks || peer == comm->rank || channel->peers[peer]->recv[connIndex].connected) continue;
    comm->connectRecv[peer] |= mask;
  }
  for (int i=0; i<nsend; i++) {
    int peer = peerSend[i];
    if (peer == -1 || peer >= comm->nRanks || peer == comm->rank || channel->peers[peer]->send[connIndex].connected) continue;
    comm->connectSend[peer] |= mask;
  }
  return ncclSuccess;
}

void dumpData(struct ncclConnect* data, int ndata) {
  for (int n=0; n<ndata; n++) {
    printf("[%d] ", n);
    uint8_t* d = (uint8_t*)data;
    for (int i=0; i<sizeof(struct ncclConnect); i++) printf("%02x", d[i]);
    printf("\n");
  }
}

NCCL_PARAM(ConnectRoundMaxPeers, "CONNECT_ROUND_MAX_PEERS", 128);
NCCL_PARAM(ReportConnectProgress, "REPORT_CONNECT_PROGRESS", 1);
#include <sys/time.h>

// opens the connections, waits and connects
ncclResult_t ncclTransportP2pSetup(struct ncclComm* comm, struct ncclTopoGraph* graph, int connIndex, int* highestTransportType/*=NULL*/) {
  INFO(NCCL_ALL,"ncclTransportP2pSetup ----------------START------------------------>");
  
  // Stream used during transport setup; need for P2P pre-connect + CUDA Graph
  ncclResult_t ret = ncclSuccess;
  int highestType = TRANSPORT_UNDEFINED;  // track highest transport type
  struct ncclConnect** data; // Store intermediate send/recvData structs for connect
  struct ncclConnect** recvData; // Points to entries inside data for given recv connection within a channel
  struct ncclConnect** sendData; // Points to entries inside data for given send connection within a channel
  int done = 0;

  int maxPeers = ncclParamConnectRoundMaxPeers();
  NCCLCHECK(ncclCalloc(&data, maxPeers));
  NCCLCHECK(ncclCalloc(&recvData, maxPeers));
  NCCLCHECK(ncclCalloc(&sendData, maxPeers));

  struct timeval timeStart, timeLast;
  gettimeofday(&timeStart, NULL);
  timeLast = timeStart; // struct copy
  bool timeReported = false;

  NCCLCHECKGOTO(ncclStrongStreamAcquireUncaptured(&comm->sharedRes->hostStream), ret, fail);
  // First time initialization
  for (int i=1; i<comm->nRanks; i++) {
    int bootstrapTag = (i<<8) + (graph ? graph->id+1 : 0);
    int recvPeer = (comm->rank - i + comm->nRanks) % comm->nRanks;
    int sendPeer = (comm->rank + i) % comm->nRanks;
    uint64_t recvMask = comm->connectRecv[recvPeer];
    uint64_t sendMask = comm->connectSend[sendPeer];

    // Data[i] contains all ncclConnect information for all send and receive connections with a given send and recv peer
    // This data is packed in the array based on the number of sendChannels and recvChannels connected with these peers
    // The first N entries contain recvData, connection information for recv connections
    // The next M entries contain sendData, connection information for send connections
    // It's not guaranteed that each entry of data has the same number of total or send/recv specific connections
    int p = i-(done+1);
    if (recvMask || sendMask) NCCLCHECK(ncclCalloc(data+p, 2*MAXCHANNELS));
    recvData[p] = data[p];
    int sendChannels = 0, recvChannels = 0;
    int type;
    TIME_START(0);
    for (int c=0; c<MAXCHANNELS; c++) {
      if (recvMask & (1UL<<c)) {
        INFO(NCCL_ALL,"ncclTransportP2pSetup BEFORE selectTransport (recv) channel : %d",c);
        NCCLCHECKGOTO(selectTransport<0>(comm, graph, recvData[p]+recvChannels++, c, recvPeer, connIndex, &type), ret, fail);
        INFO(NCCL_ALL,"ncclTransportP2pSetup AFTER selectTransport (recv) channel : %d",c);
        if (type > highestType) highestType = type;
      }
    }
    TIME_STOP(0);
    TIME_START(1);
    sendData[p] = recvData[p]+recvChannels;
    for (int c=0; c<MAXCHANNELS; c++) {
      if (sendMask & (1UL<<c)) {
        INFO(NCCL_ALL,"ncclTransportP2pSetup BEFORE selectTransport (send) channel : %d",c);
        NCCLCHECKGOTO(selectTransport<1>(comm, graph, sendData[p]+sendChannels++, c, sendPeer, connIndex, &type), ret, fail);
        INFO(NCCL_ALL,"ncclTransportP2pSetup AFTER selectTransport (send) channel : %d",c);
        if (type > highestType) highestType = type;
      }
    }
    TIME_STOP(1);

    TIME_START(2);
    if (sendPeer == recvPeer) {
      if (recvChannels+sendChannels) {
        INFO(NCCL_ALL,"ncclTransportP2pSetup :::::: sendPeer == recvPeer : %d",sendPeer);
        NCCLCHECKGOTO(bootstrapSend(comm->bootstrap, recvPeer, bootstrapTag, data[p], sizeof(struct ncclConnect)*(recvChannels+sendChannels)), ret, fail);
        NCCLCHECKGOTO(bootstrapRecv(comm->bootstrap, recvPeer, bootstrapTag, data[p], sizeof(struct ncclConnect)*(recvChannels+sendChannels)), ret, fail);
        sendData[p] = data[p];
        recvData[p] = data[p]+sendChannels;
      }
    } else {
      INFO(NCCL_ALL,"ncclTransportP2pSetup :::::: sendPeer : % d recvPeer : %d",sendPeer, recvPeer);
      if (recvChannels) NCCLCHECKGOTO(bootstrapSend(comm->bootstrap, recvPeer, bootstrapTag, recvData[p], sizeof(struct ncclConnect)*recvChannels), ret, fail);
      if (sendChannels) NCCLCHECKGOTO(bootstrapSend(comm->bootstrap, sendPeer, bootstrapTag, sendData[p], sizeof(struct ncclConnect)*sendChannels), ret, fail);
      if (sendChannels) NCCLCHECKGOTO(bootstrapRecv(comm->bootstrap, sendPeer, bootstrapTag, sendData[p], sizeof(struct ncclConnect)*sendChannels), ret, fail);
      if (recvChannels) NCCLCHECKGOTO(bootstrapRecv(comm->bootstrap, recvPeer, bootstrapTag, recvData[p], sizeof(struct ncclConnect)*recvChannels), ret, fail);
    }
    TIME_STOP(2);

    if (i-done == maxPeers || i == comm->nRanks-1) {
      // Loop until all channels with all ranks have been connected
      bool allChannelsConnected;
      allChannelsConnected = false;
      while (!allChannelsConnected) {
        allChannelsConnected = true;
        for (int j=done+1; j<=i; j++) {
          int recvPeer = (comm->rank - j + comm->nRanks) % comm->nRanks;
          int sendPeer = (comm->rank + j) % comm->nRanks;
          uint64_t recvMask = comm->connectRecv[recvPeer];
          uint64_t sendMask = comm->connectSend[sendPeer];

          int p = j-(done+1);
          int sendDataOffset = 0;
          int recvDataOffset = 0;
          for (int c=0; c<MAXCHANNELS; c++) {
            TIME_START(3);
            if (sendMask & (1UL<<c)) {
              struct ncclConnector* conn = comm->channels[c].peers[sendPeer]->send + connIndex;
              strcpy(conn->conn.hostname,comm->hostname); //STEFANO
              INFO(NCCL_ALL,"conn->conn.hostname %s",conn->conn.hostname);
              // This connector hasn't completed connection yet
              if (conn->connected == 0) {
                NCCLCHECKGOTO(conn->transportComm->connect(comm, sendData[p] + sendDataOffset++, 1, comm->rank, conn), ret, fail);
                if (ret == ncclSuccess) {
                  conn->connected = 1;
                  /* comm->channels[c].devPeers[sendPeer]->send[connIndex] is a device memory access. */
                  CUDACHECKGOTO(cudaMemcpyAsync(&comm->channels[c].devPeersHostPtr[sendPeer]->send[connIndex], &conn->conn, sizeof(struct ncclConnInfo), cudaMemcpyHostToDevice, comm->sharedRes->hostStream.cudaStream), ret, fail);
                } else if (ret == ncclInProgress) {
                  allChannelsConnected = false;
                }
              }
            }
            TIME_STOP(3);

            // Start with recv channels
            TIME_START(4);
            if (recvMask & (1UL<<c)) {
              struct ncclConnector* conn = comm->channels[c].peers[recvPeer]->recv + connIndex;
              strcpy(conn->conn.hostname,comm->hostname); //STEFANO
              // This connector hasn't completed connection yet
              if (conn->connected == 0) {
                NCCLCHECKGOTO(conn->transportComm->connect(comm, recvData[p] + recvDataOffset++, 1, comm->rank, conn), ret, fail);
                if (ret == ncclSuccess) {
                  conn->connected = 1;
                  /* comm->channels[c].devPeers[recvPeer]->recv[connIndex] is a device memory access. */
                  CUDACHECKGOTO(cudaMemcpyAsync(&comm->channels[c].devPeersHostPtr[recvPeer]->recv[connIndex], &conn->conn, 
                                        sizeof(struct ncclConnInfo), cudaMemcpyHostToDevice, comm->sharedRes->hostStream.cudaStream),
                                        ret, fail);
                } else if (ret == ncclInProgress) {
                  allChannelsConnected = false;
                }
              }
            }
            TIME_STOP(4);
          }
          if (sendMask || recvMask) {
            free(data[p]);
            data[p] = NULL;
          }
        }
	if (ncclParamReportConnectProgress() && comm->rank == 0) {
          struct timeval now;
          gettimeofday(&now, NULL);
          if (((now.tv_sec - timeLast.tv_sec)*1.0 + (now.tv_usec-timeLast.tv_usec)*1e-6) > 1) {
            float elapsed = (now.tv_sec - timeStart.tv_sec)*1.0 + (now.tv_usec-timeStart.tv_usec)*1e-6;
	          float remaining = elapsed*(comm->nRanks-done)/done;
            printf("%sP2p connect: %g%% Elapsed %d:%02d Remaining %d:%02d                                       ",
                timeReported ? "\r" : "", done*100.0/comm->nRanks, ((int)elapsed)/60, ((int)elapsed)%60, ((int)remaining)/60, ((int)remaining)%60);
            fflush(stdout);
            timeReported = true;
	    timeLast = now; // struct copy;
          }
        }
      }
      done = i;
    }
  }

  {
    struct timeval now;
    gettimeofday(&now, NULL);
    float elapsed = (now.tv_sec - timeStart.tv_sec)*1.0 + (now.tv_usec-timeStart.tv_usec)*1e-6;
    if (elapsed > 1.0) INFO(NCCL_PROFILE, "timings: rank %d nranks %d P2p connect done in %.2f", comm->rank, comm->nRanks, elapsed);
    if (1 || timeReported) {
      printf("\rP2p connect done in %d:%02d.%06d          \n",
         ((int)elapsed)/60, ((int)elapsed)%60, (int)((elapsed-((int)elapsed))*(float)1000000));
      fflush(stdout);
    }
  }

  /* We need to sync ranks here since some ranks might run too fast after connection setup
   * and start to destroy the connection after returning from this function; however, the
   * others might still be trying to connect and import the buffer. No sync can lead to invalid
   * shmem/cuda buffer. In addition, we also clear all connect masks and free each connectInfo array */
  for (int i = 1; i < comm->nRanks; i++) {
    int bootstrapTag = (i << 8) + (1 << 7) + (graph ? graph->id + 1 : 0);
    int recvPeer = (comm->rank - i + comm->nRanks) % comm->nRanks;
    int sendPeer = (comm->rank + i) % comm->nRanks;
    int flag = 0;

    if (recvPeer != sendPeer) {
      if (comm->connectSend[sendPeer] != 0UL)
        NCCLCHECKGOTO(bootstrapSend(comm->bootstrap, sendPeer, bootstrapTag, &flag, sizeof(int)), ret, fail);
      if (comm->connectRecv[recvPeer] != 0UL)
        NCCLCHECKGOTO(bootstrapSend(comm->bootstrap, recvPeer, bootstrapTag, &flag, sizeof(int)), ret, fail);

      if (comm->connectSend[sendPeer] != 0UL)
        NCCLCHECKGOTO(bootstrapRecv(comm->bootstrap, sendPeer, bootstrapTag, &flag, sizeof(int)), ret, fail);
      if (comm->connectRecv[recvPeer] != 0UL)
        NCCLCHECKGOTO(bootstrapRecv(comm->bootstrap, recvPeer, bootstrapTag, &flag, sizeof(int)), ret, fail);
    } else {
      if (comm->connectSend[sendPeer] != 0UL || comm->connectRecv[recvPeer] != 0UL) {
        NCCLCHECKGOTO(bootstrapSend(comm->bootstrap, sendPeer, bootstrapTag, &flag, sizeof(int)), ret, fail);
        NCCLCHECKGOTO(bootstrapRecv(comm->bootstrap, sendPeer, bootstrapTag, &flag, sizeof(int)), ret, fail);
      }
    }
    comm->connectRecv[recvPeer] = comm->connectSend[sendPeer] = 0UL;
  }

  free(data);
  free(sendData);
  free(recvData);

  if (highestTransportType != NULL) *highestTransportType = highestType;
  TIME_PRINT("P2P Setup/Connect");
exit:
  NCCLCHECK(ncclStrongStreamWaitStream(ncclCudaGraphNone(), &comm->sharedRes->deviceStream, &comm->sharedRes->hostStream));
  NCCLCHECK(ncclStrongStreamRelease(ncclCudaGraphNone(), &comm->sharedRes->hostStream));
  return ret;
fail:
  goto exit;
}

extern struct ncclTransport collNetTransport;

// All ranks must participate in collNetSetup call
// We do not NCCLCHECK this call because we would fall back to P2P network in case CollNet setup fails
int ncclTransportCollNetSetup(struct ncclComm* comm, struct ncclTopoGraph* collNetGraph, struct ncclChannel* channel, int masterRank, int masterPeer, int collNetGraphChannelId, int type, ncclConnect* connect) {
  int fail = 1;
  int rank = comm->rank;
  int nranks = comm->nRanks;
  int nMasters = comm->nNodes;
  int isMaster = (rank == masterRank) ? 1 : 0;

  // check if we can connect to collnet, whose root is the nranks-th rank
  struct ncclPeerInfo *myInfo = comm->peerInfo+rank, *peerInfo = comm->peerInfo+nranks;
  peerInfo->rank = nranks;

  if (isMaster && type == collNetSend) {
    INFO(NCCL_ALL, "ncclTransportCollNetSetup CollNet [send] : rank %d collNetRank %d collNetNranks %d received connect from rank %d", rank, comm->node, nMasters, masterPeer);
    TRACE(NCCL_INIT, "CollNet [send] : rank %d collNetRank %d collNetNranks %d received connect from rank %d", rank, comm->node, nMasters, masterPeer);
  }

  // select
  struct ncclChannelPeer* root = channel->peers[nranks];
  // connector index: 0 for recv, 1 for send
  struct ncclConnector* conn = (type == collNetRecv) ? root->recv+type : root->send+type;
  struct ncclTransportComm* transportComm = (type == collNetRecv) ? &(collNetTransport.recv) : &(collNetTransport.send);
  conn->transportComm = transportComm;
  // setup
  struct ncclConnect myConnect;
  if (isMaster) {
    NCCLCHECK(transportComm->setup(comm, collNetGraph, myInfo, peerInfo, &myConnect, conn, collNetGraphChannelId, type));
  }
  // prepare connect handles
  ncclResult_t res;
  struct {
    int isMaster;
    ncclConnect connect;
  } *allConnects = NULL;
  ncclConnect *masterConnects = NULL;
  NCCLCHECK(ncclCalloc(&masterConnects, nMasters));
  if (type == collNetRecv) {  // recv side: AllGather
    // all ranks must participate
    NCCLCHECK(ncclCalloc(&allConnects, nranks));
    allConnects[rank].isMaster = isMaster;
    memcpy(&(allConnects[rank].connect), &myConnect, sizeof(struct ncclConnect));
    NCCLCHECKGOTO(bootstrapAllGather(comm->bootstrap, allConnects, sizeof(*allConnects)), res, cleanup);
    // consolidate
    int c = 0;
    for (int r = 0; r < nranks; r++) {
      if (allConnects[r].isMaster) {
        memcpy(masterConnects+c, &(allConnects[r].connect), sizeof(struct ncclConnect));
        c++;
      }
    }
  } else { // send side : copy in connect info received from peer recv master
    if (isMaster) memcpy(masterConnects+comm->node, connect, sizeof(struct ncclConnect));
  }
  // connect
  if (isMaster) {
    NCCLCHECKGOTO(transportComm->connect(comm, masterConnects, nMasters, comm->node, conn), res, cleanup);
    struct ncclDevChannelPeer* devRoot;
    CUDACHECKGOTO(cudaMemcpy(&devRoot, channel->devPeers + nranks, sizeof(struct ncclDevChannelPeer*), cudaMemcpyDeviceToHost), res, cleanup);
    struct ncclConnInfo* devConnInfo = (type == collNetRecv) ? devRoot->recv + type : devRoot->send + type;
    CUDACHECKGOTO(cudaMemcpy(devConnInfo, &conn->conn, sizeof(struct ncclConnInfo), cudaMemcpyHostToDevice), res, cleanup);
  }
  if (isMaster && type == collNetRecv) {
    memcpy(connect, masterConnects+comm->node, sizeof(struct ncclConnect));
    TRACE(NCCL_INIT, "CollNet [recv] : rank %d collNetRank %d collNetNranks %d sent connect to rank %d", rank, comm->node, nMasters, masterPeer);
  }
  fail = 0;
cleanup:
  if (allConnects != NULL) free(allConnects);
  if (masterConnects != NULL) free(masterConnects);
  return fail;
}

ncclResult_t ncclTransportCollNetCheck(struct ncclComm* comm, int collNetSetupFail) {
  // AllGather collNet setup results
  int allGatherFailures[NCCL_MAX_LOCAL_RANKS] = {0};
  allGatherFailures[comm->localRank] = collNetSetupFail;
  NCCLCHECK(bootstrapIntraNodeAllGather(comm->bootstrap, comm->localRankToRank, comm->localRank, comm->localRanks, allGatherFailures, sizeof(int)));
  for (int i=0; i<comm->localRanks; i++) {
    if (allGatherFailures[i] != 0) {
      collNetSetupFail = 1;
      break;
    }
  }
  if (collNetSetupFail) {
    if (comm->localRank == 0) WARN("Cannot initialize CollNet, using point-to-point network instead");
    return ncclSystemError;
  }
  return ncclSuccess;
}

ncclResult_t ncclTransportCollNetFree(struct ncclComm* comm) {
  // Free collNet resources
  for (int r=0; r<comm->nChannels; r++) {
    struct ncclChannel* channel = comm->channels+r;
    struct ncclChannelPeer* peer = channel->peers[comm->nRanks];
    if (peer) {
      if (ncclAtomicRefCountDecrement(&peer->refCount) == 0) {
        for (int b=0; b<NCCL_MAX_CONNS; b++) {
          struct ncclConnector* send = peer->send + b;
          if (send->transportResources && send->transportComm) NCCLCHECK(send->transportComm->free(send));
          send->transportResources = NULL; // avoid double free
        }
        for (int b=0; b<NCCL_MAX_CONNS; b++) {
          struct ncclConnector* recv = peer->recv + b;
          if (recv->transportResources && recv->transportComm) NCCLCHECK(recv->transportComm->free(recv));
          recv->transportResources = NULL; // avoid double free
        }
      }
    }
  }
  return ncclSuccess;
}
