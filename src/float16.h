#pragma once

#include <stdint.h>

#if __F16C__
#include <immintrin.h>
/* The 0 here is the rounding mode.
 *  0x00 = Round to Nearest
 *  0x01 = Round Down
 *  0x02 = Round Up
 *  0x03 = Truncate */
#define float16Encode(v) _cvtss_sh(v, 0)
#define float16Decode(v) _cvtsh_ss(v)
#else
#define float16Encode(v) float16Encode_(v)
#define float16Decode(v) float16Decode_(v)
#endif

/* Make these macros because Intel CPUs are adding hardware-level
 * bfloat16 conversion soon, then we can special case these with
 * CPU intrinsics like above for float16 */
#define bfloat16Encode(v) bfloat16Encode_(v)
#define bfloat16Decode(v) bfloat16Decode_(v)

/* Always make the function versions available even if we are using builtins */
uint16_t float16Encode_(float value);
float float16Decode_(uint16_t value);

uint16_t bfloat16Encode_(float value);
float bfloat16Decode_(uint16_t value);

#ifdef DATAKIT_TEST
int float16Test(int argc, char *argv[]);
#endif
