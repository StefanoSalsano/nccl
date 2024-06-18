#ifndef DEFINE_OUT_PRINTF_H_
#define DEFINE_OUT_PRINTF_H_

//define ENABLE_OUT to enable debug printing in CUDA kernels
//#define ENABLE_OUT

#ifdef ENABLE_OUT
#define OUT(...) printf(__VA_ARGS__)
#else
#define OUT(...)
#endif

#endif // end include guard