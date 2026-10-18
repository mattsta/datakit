#pragma once

/* bigmath - pluggable arbitrary-precision integers.
 *
 * The struct is an opaque caller-owned buffer (stack or embedded);
 * whichever backend is compiled in (bigmath-imath.c today,
 * bigmath-libbf.c as an alternative) stores its state in place and
 * must _Static_assert its state type fits math[]. Backend contract:
 *
 * - Every Init* variant makes the struct valid; pair each with a
 *   lifecycle end (Reset or Free) on every path.
 * - Reset = "zero and reusable" AND releases any internal storage
 *   the value spilled beyond the inline buffer; a Reset struct can
 *   be re-Init'd or reused directly.
 * - Free releases internal storage only - never the struct itself
 *   (the caller owns it, and it is usually a stack local).
 * - Operations allow result aliasing an operand.
 * - ToNative* require the value to fit the native type: out-of-range
 *   asserts in debug and returns 0 in release. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct bigmath {
    uint8_t math[64];
} bigmath;

void bigmathInit(bigmath *b);
void bigmathInitUnsigned(bigmath *b, uint64_t value);
void bigmathInitSigned(bigmath *b, int64_t value);
bool bigmathInitFromString(bigmath *b, const void *str);
void bigmathInitCopy(bigmath *dst, bigmath *src);
void bigmathFree(bigmath *b);

/* Print b in base 10 (leading '-' when negative) and a newline, to fp
 * or to stdout. Values of any size print in full, never truncated. */
void bigmathReprFile(bigmath *b, FILE *fp);
void bigmathRepr(bigmath *b);

void bigmathShiftLeft(bigmath *b, uint32_t howMuch, bigmath *result);
void bigmathMultiply(bigmath *a, bigmath *b, bigmath *result);
void bigmathMultiplyValue(bigmath *a, int64_t value, bigmath *result);
int bigmathCompare(bigmath *a, bigmath *b);
void bigmathAdd(bigmath *a, bigmath *b, bigmath *result);
void bigmathDivideRemainder(bigmath *top, bigmath *bottom,
                            bigmath *resultQuotent, bigmath *resultRemainder);

/* Digit-generation divide: requires 0 <= top < bottom * 10. Replaces
 * top with top % bottom and returns the single-digit quotient.
 * Runs allocation-free (at most nine compare/subtract passes),
 * unlike DivideRemainder's general long division. */
uint32_t bigmathDivideDigitInPlace(bigmath *top, bigmath *bottom);
void bigmathExponent(uint64_t base, uint64_t exp, bigmath *result);

uint64_t bigmathToNativeUnsigned(bigmath *b);
int64_t bigmathToNativeSigned(bigmath *b);
void bigmathReset(bigmath *b);
