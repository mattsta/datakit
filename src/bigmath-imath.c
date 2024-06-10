#include "bigmath.h"

#include "../deps/imath/imath.c"

#include <stdio.h>

#define MPZ(x) (mpz_t *)(x)

void bigmathInit(bigmath *b) {
    mp_int_init(MPZ(b));
}

bool bigmathInitFromString(bigmath *b, const void *str) {
    /* Create a bigmath from 'str' (null terminated) in base 10 */
    mp_result result = mp_int_read_string(MPZ(b), 10, str);
    return result == MP_OK;
}

void bigmathInitUnsigned(bigmath *b, uint64_t value) {
    mp_int_init_uvalue(MPZ(b), value);
}

void bigmathInitSigned(bigmath *b, int64_t value) {
    mp_int_init_value(MPZ(b), value);
}

void bigmathInitCopy(bigmath *dst, bigmath *src) {
    mp_int_init_copy(MPZ(dst), MPZ(src));
}

void bigmathFree(bigmath *b) {
    mp_int_free(MPZ(b));
}

void bigmathRepr(bigmath *b) {
    char buf[1024] = {0};
    mp_int_to_string(MPZ(b), 10, buf, sizeof(buf));
    printf("%s\n", buf);
}

void bigmathShiftLeft(bigmath *b, uint32_t howMuch, bigmath *result) {
    mp_int_mul_pow2(MPZ(b), howMuch, MPZ(result));
}

void bigmathMultiply(bigmath *a, bigmath *b, bigmath *result) {
    mp_int_mul(MPZ(a), MPZ(b), MPZ(result));
}

void bigmathMultiplyValue(bigmath *a, int64_t value, bigmath *result) {
    mp_int_mul_value(MPZ(a), value, MPZ(result));
}

int bigmathCompare(bigmath *a, bigmath *b) {
    return mp_int_compare(MPZ(a), MPZ(b));
}

void bigmathAdd(bigmath *a, bigmath *b, bigmath *result) {
    mp_int_add(MPZ(a), MPZ(b), MPZ(result));
}

void bigmathDivideRemainder(bigmath *top, bigmath *bottom,
                            bigmath *resultQuotent, bigmath *resultRemainder) {
    mp_int_div(MPZ(top), MPZ(bottom), MPZ(resultQuotent), MPZ(resultRemainder));
#if 0
    bigmathRepr(resultQuotent);
    bigmathRepr(resultRemainder);
    bigmathRepr(top);
    bigmathRepr(bottom);
#endif
}

void bigmathExponent(uint64_t base, uint64_t exp, bigmath *result) {
    mp_int_expt_value(base, exp, MPZ(result));
}

uint64_t bigmathToNativeUnsigned(bigmath *b) {
    mp_usmall result;
    mp_int_to_uint(MPZ(b), &result);
    return result;
}

int64_t bigmathToNativeSigned(bigmath *b) {
    mp_small result;
    mp_int_to_int(MPZ(b), &result);
    return result;
}

void bigmathReset(bigmath *b) {
    mp_int_clear(MPZ(b));
}
