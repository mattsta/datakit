#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct bigmath {
    uint8_t math[64];
} bigmath;

void bigmathInit(bigmath *b);
void bigmathInitUnsigned(bigmath *b, uint64_t value);
void bigmathInitSigned(bigmath *b, int64_t value);
bool bigmathInitFromString(bigmath *b, const void *str);
void bigmathInitCopy(bigmath *dst, bigmath *src);
void bigmathFree(bigmath *b);
void bigmathRepr(bigmath *b);

void bigmathShiftLeft(bigmath *b, uint32_t howMuch, bigmath *result);
void bigmathMultiply(bigmath *a, bigmath *b, bigmath *result);
void bigmathMultiplyValue(bigmath *a, int64_t value, bigmath *result);
int bigmathCompare(bigmath *a, bigmath *b);
void bigmathAdd(bigmath *a, bigmath *b, bigmath *result);
void bigmathDivideRemainder(bigmath *top, bigmath *bottom,
                            bigmath *resultQuotent, bigmath *resultRemainder);
void bigmathExponent(uint64_t base, uint64_t exp, bigmath *result);

uint64_t bigmathToNativeUnsigned(bigmath *b);
int64_t bigmathToNativeSigned(bigmath *b);
void bigmathReset(bigmath *b);
