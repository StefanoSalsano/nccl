#ifndef DEFINE_OUT_PRINTF_H_
#define DEFINE_OUT_PRINTF_H_

#include <stdarg.h>

//define ENABLE_OUT to enable debug printing in CUDA kernels
#define ENABLE_OUT

__device_ void add_hostname(const char* format, ...) {
    
    // Start processing the variable arguments
    va_list args;
    va_start(args, format);
    
    // Print the hostname
    printf("%s: ", "hostname");
    
    // Print the formatted string
    vprintf(format, args);
    
    // Clean up the variable argument list
    va_end(args);
};


#ifdef ENABLE_OUT
//#define OUT(...) printf(__VA_ARGS__)
#define OUT(...) add_hostname(__VA_ARGS__)
#else
#define OUT(...)
#endif

#endif // end include guard