#pragma once

#include <limits.h>

#if defined(__LP64__) || defined(_LP64)
#define DK_WORDSIZE 64
#elif defined(__WORDSIZE)
#define DK_WORDSIZE __WORDSIZE
#else
#warning "Couldn't detect word size, assuming 32-bit build."
#define DK_WORDSIZE 32
#endif
