#include "bigmath.h"

#include "datakit.h"

/* Point imath's allocator seams at the zmalloc family so bignum
 * storage is accounted and OOM is fail-fast; the standalone imath
 * library target keeps its system-allocator defaults. */
#define IMATH_CALLOC(count, size) zcalloc(count, size)
#define IMATH_REALLOC(ptr, size) zrealloc(ptr, size)
#define IMATH_FREE(ptr) zfree(ptr)

#include "../deps/imath/imath.c"

#include "cleaner.h"

#include <assert.h>
#include <stdio.h>

defcleanerName(char *, zfree, Chars);

#define MPZ(x) (mpz_t *)(x)

/* bigmath's opaque byte buffer IS the mpz_t in place; any growth of
 * the imath struct past the public buffer silently corrupts every
 * caller's stack, so the fit is enforced at compile time. */
static_assert(sizeof(mpz_t) <= sizeof(((bigmath *)0)->math),
              "bigmath.math[] too small for imath mpz_t");

void bigmathInit(bigmath *b) {
    mp_int_init(MPZ(b));
}

bool bigmathInitFromString(bigmath *b, const void *str) {
    /* Create a bigmath from 'str' (null terminated) in base 10.
     * The struct must be initialized first: read_string writes
     * through the tagged digit pointer, and on a fresh struct that
     * would be a realloc of garbage. */
    bigmathInit(b);
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
    /* bigmath structs are caller-owned (stack or embedded), so only
     * the internal digit storage is released - never the container.
     * (imath's mp_int_free() would free() the container itself, which
     * is undefined behavior for every current call pattern.) */
    mp_int_clear(MPZ(b));
}

void bigmathReprFile(bigmath *b, FILE *fp) {
    /* mp_int_string_len() counts sign, digits and terminator. It may
     * over-estimate but never under-estimates, so a buffer of that
     * size always holds the full value. */
    const mp_result need = mp_int_string_len(MPZ(b), 10);
    assert(need >= 2);

    char stackBuf[1024];
    cleanerName(zfree, Chars) char *heapBuf = nullptr;
    char *buf = stackBuf;
    if ((size_t)need > sizeof(stackBuf)) {
        heapBuf = zmalloc((size_t)need);
        buf = heapBuf;
    }

    [[maybe_unused]] const mp_result res =
        mp_int_to_string(MPZ(b), 10, buf, (size_t)need);
    assert(res == MP_OK);
    fprintf(fp, "%s\n", buf);
}

void bigmathRepr(bigmath *b) {
    bigmathReprFile(b, stdout);
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
    /* Distinct outputs are a hard precondition: aliased q/r would
     * silently clobber each other (imath only asserts this in debug) */
    assert(resultQuotent != resultRemainder);
    mp_int_div(MPZ(top), MPZ(bottom), MPZ(resultQuotent), MPZ(resultRemainder));
#if 0
    bigmathRepr(resultQuotent);
    bigmathRepr(resultRemainder);
    bigmathRepr(top);
    bigmathRepr(bottom);
#endif
}

uint32_t bigmathDivideDigitInPlace(bigmath *top, bigmath *bottom) {
    uint32_t digit = 0;

    while (mp_int_compare(MPZ(top), MPZ(bottom)) >= 0) {
        mp_int_sub(MPZ(top), MPZ(bottom), MPZ(top));
        digit++;
        assert(digit <= 9);
    }

    return digit;
}

void bigmathExponent(uint64_t base, uint64_t exp, bigmath *result) {
    mp_int_expt_value(base, exp, MPZ(result));
}

uint64_t bigmathToNativeUnsigned(bigmath *b) {
    /* Out-of-range values return 0 (never uninitialized garbage);
     * callers own keeping values in native range */
    mp_usmall result = 0;
    [[maybe_unused]] const mp_result status = mp_int_to_uint(MPZ(b), &result);
    assert(status == MP_OK);
    return result;
}

int64_t bigmathToNativeSigned(bigmath *b) {
    mp_small result = 0;
    [[maybe_unused]] const mp_result status = mp_int_to_int(MPZ(b), &result);
    assert(status == MP_OK);
    return result;
}

void bigmathReset(bigmath *b) {
    /* Reset means "zero and reusable" like every framework Reset -
     * AND it must release any digit storage that spilled past imath's
     * inline buffer, because values above ~384 bits calloc a digit
     * array that nothing else frees (Reset is the lifecycle end at
     * every call site). clear + init returns to the inline buffer. */
    mp_int_clear(MPZ(b));
    mp_int_init(MPZ(b));
}
