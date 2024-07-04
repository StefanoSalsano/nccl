#ifndef DEFINE_OUT_PRINTF_H_
#define DEFINE_OUT_PRINTF_H_


//define ENABLE_OUT to enable debug printing in CUDA kernels
#define ENABLE_OUT

//#define OUT(...) printf(__VA_ARGS__)

#ifdef ENABLE_OUT
#define OUT(format, ...) printf("CUDA-%s: " format, ncclShmem.hostname_shmem, ##__VA_ARGS__)
#else
#define OUT(...)
#endif

#endif // end include guard