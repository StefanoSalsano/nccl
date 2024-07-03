#ifndef DEFINE_OUT_PRINTF_H_
#define DEFINE_OUT_PRINTF_H_

#include <stdarg.h>

//define ENABLE_OUT to enable debug printing in CUDA kernels
#define ENABLE_OUT


//#define OUT(...) add_hostname(__VA_ARGS__)

#ifdef ENABLE_OUT
#define OUT(...) printf(__VA_ARGS__)
#define OUT2(STRING, ...) printf(STRING, __VA_ARGS__)

#else
#define OUT(...)
#endif

#endif // end include guard