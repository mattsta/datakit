# varintDD: Double-Double (106-bit) Arithmetic

## Overview

**varintDD** implements double-double arithmetic: a number is held as an _unevaluated sum_ of two IEEE-754 doubles, `value == hi + lo`, giving roughly 106 significand bits and ~31 decimal digits without ever leaving the FPU.

It exists for the gap between "a `double` is not quite enough" and "reach for a bignum library". Long accumulations, ill-conditioned sums, iterative refinement, deep fractal zooms, simulation clocks that tick tens of millions of times — cases where 53 bits of significand runs out before the algorithm does, but where a 20-50x slowdown for soft-float `__float128` or an 80x slowdown for arbitrary precision would be intolerable.

**Key Features**: error-free transformations built on hardware FMA, full add/sub/mul/div/sqrt, exact `int64` conversion, decimal printing, SIMD array operations (NEON/AVX2/scalar), compensated reductions that outrun the naive loop they replace, and a runtime self-check that catches `-ffast-math` breaking the whole thing silently.

## Key Characteristics

| Property       | Value                                               |
| -------------- | --------------------------------------------------- |
| Implementation | Header (.h, mostly `static inline`) + Compiled (.c) |
| Size           | 16 bytes (two packed doubles)                       |
| Significand    | ~106 bits (~31 decimal digits)                      |
| Exponent range | Unchanged from `double`                             |
| SIMD Support   | AVX2 (x86-64), NEON (aarch64), scalar fallback      |
| Allocation     | None, anywhere                                      |
| Best For       | Long accumulations, ill-conditioned reductions      |
| Cost vs double | 4.9x multiply, 11.4x add, 14.0x divide (latency)    |

## Representation

A double-double is the pair `{hi, lo}` with the invariant:

```
hi == (double)(hi + lo)      and      |lo| <= ulp(hi) / 2
```

That is, `hi` is the value correctly rounded to a `double`, and `lo` is exactly the amount that rounding discarded. Both statements matter:

- `varintDDToDouble()` is just `hi` — no arithmetic, because `hi` is _by definition_ the value rounded to double.
- The bound on `|lo|` is what [varintDDStream](varintDDStream.md) exploits to compress arrays of these values.

Pairs assembled by hand are not necessarily normalized. Run them through `varintDDNormalize()` before doing arithmetic; every function in the header assumes the invariant holds on input and guarantees it on output.

## The Error-Free Transformations

Everything else is built from three primitives that compute a result **and** the exact amount that result discarded.

### Knuth's two-sum — exact for any two doubles

```c
sum   = a + b
bKept = sum - a          /* the part of b that survived */
aKept = sum - bKept      /* the part of a that survived */
error = (a - aKept) + (b - bKept)
```

Six operations, no branches, no bit manipulation. `sum + error` reproduces `a + b` with **no error whatsoever**. No ordering requirement.

### Dekker's fast-two-sum — 3 operations, requires `|a| >= |b|`

```c
sum   = a + b
error = b - (sum - a)
```

The larger operand keeps all its bits, so only the smaller one can lose any, and one subtraction recovers the loss. This is the renormalization workhorse.

### Two-product via FMA — 2 operations

```c
product = a * b
error   = fma(a, b, -product)
```

The FMA evaluates `a*b - product` with a single rounding, which _is_ the 53-bit tail the multiply discarded. Without hardware FMA, Dekker's splitting method costs 17 operations instead; `VARINT_DD_HAS_FMA` selects between them.

## API Reference

### Type and construction

```c
typedef struct varintDD {
    double hi;   /* the value rounded to a double */
    double lo;   /* the rounding error hi discarded */
} varintDD;

varintDD varintDDFromDouble(double a);
varintDD varintDDFromInt64(int64_t v);      /* exact for EVERY int64 */
varintDD varintDDZero(void);
double   varintDDToDouble(const varintDD a);
varintDD varintDDNormalize(const varintDD a);
```

### Error-free transformations

```c
varintDD varintDDTwoSum(double a, double b);       /* exact, any order */
varintDD varintDDFastTwoSum(double a, double b);   /* exact when |a| >= |b| */
varintDD varintDDTwoDiff(double a, double b);      /* exact */
varintDD varintDDTwoProduct(double a, double b);   /* exact */
varintDD varintDDTwoSquare(double a);              /* exact */
```

### Arithmetic

```c
varintDD varintDDAdd(const varintDD a, const varintDD b);        /* ~20 flops */
varintDD varintDDAddFast(const varintDD a, const varintDD b);    /* ~11 flops */
varintDD varintDDSub(const varintDD a, const varintDD b);
varintDD varintDDAddDouble(const varintDD a, double b);
varintDD varintDDMul(const varintDD a, const varintDD b);
varintDD varintDDMulDouble(const varintDD a, double b);
varintDD varintDDSquare(const varintDD a);
varintDD varintDDDiv(const varintDD a, const varintDD b);
varintDD varintDDDivDouble(const varintDD a, double b);
varintDD varintDDSqrt(const varintDD a);
varintDD varintDDNegate(const varintDD a);
varintDD varintDDAbs(const varintDD a);
```

### Comparison and classification

```c
int  varintDDCompare(const varintDD a, const varintDD b);   /* <0, 0, >0 */
bool varintDDIsZero(const varintDD a);
bool varintDDIsNaN(const varintDD a);
bool varintDDIsFinite(const varintDD a);
```

### Text conversion

```c
#define VARINT_DD_STRING_MAX     48
#define VARINT_DD_DIGITS_MAX     31
#define VARINT_DD_DIGITS_DEFAULT 30

size_t varintDDToString(char *dst, size_t dstLen, varintDD value,
                        uint32_t digits);
```

`printf` has no conversion that can show a double-double — `%g` and `%e` see one limb and stop, so the entire trailing limb is invisible. Digits come out by repeated scaling in double-double arithmetic rather than through a bignum, which is what keeps this allocation free. About **30 significant digits are trustworthy**; asking for 31 may leave the last one off by one. `VARINT_DD_STRING_MAX` bytes is always enough.

### Compensated accumulator

```c
typedef struct varintDDAccum { double sum; double comp; } varintDDAccum;

varintDDAccum varintDDAccumInit(void);
void          varintDDAccumAdd(varintDDAccum *acc, double value);
varintDD      varintDDAccumResult(const varintDDAccum *acc);
varintDDAccum varintDDAccumMerge(const varintDDAccum a, const varintDDAccum b);
```

### Array operations (vectorized)

```c
const char *varintDDBackend(void);        /* "AVX2", "NEON", or "scalar" */
size_t      varintDDBackendLanes(void);

void varintDDAddArray(varintDD *dst, const varintDD *a, const varintDD *b,
                      size_t count);
void varintDDMulArray(varintDD *dst, const varintDD *a, const varintDD *b,
                      size_t count);

varintDD varintDDSumArray(const varintDD *values, size_t count);
varintDD varintDDSumDoubles(const double *values, size_t count);
varintDD varintDDDotDoubles(const double *a, const double *b, size_t count);

void varintDDSplitLimbs(const varintDD *src, double *hi, double *lo,
                        size_t count);
void varintDDJoinLimbs(varintDD *dst, const double *hi, const double *lo,
                       size_t count);
size_t varintDDNonzeroLimbMask(const double *lo, size_t count,
                               uint8_t *bitmap);
```

### Self-check

```c
bool varintDDSelfCheck(void);
```

## Real-World Examples

### Example 1: A simulation clock that does not drift

```c
#include "varintDD.h"

/* dt = 1/3 microsecond: not representable in binary, like most
 * real timesteps. After 10 million ticks the plain double clock has
 * drifted by tens of picoseconds; the double-double clock is exact. */
const double dt = (1.0 / 3.0) * 1e-6;
const varintDD step = varintDDFromDouble(dt);

varintDD clock = varintDDZero();

for (size_t i = 0; i < 10000000; i++) {
    clock = varintDDAdd(clock, step);
    /* ... advance the simulation ... */
}

char text[VARINT_DD_STRING_MAX];
varintDDToString(text, sizeof(text), clock, 30);
printf("elapsed: %s seconds\n", text);
```

### Example 2: Summation that survives cancellation

```c
#include "varintDD.h"

/* Large values that cancel, with small ones between them. Naive
 * double summation annihilates every small term and returns 0. */
double values[100001];
values[0] = 1e16;
values[100000] = -1e16;
for (size_t i = 1; i < 100000; i++) {
    values[i] = 1.0;
}

const varintDD total = varintDDSumDoubles(values, 100001);

printf("%.17g\n", varintDDToDouble(total));   /* 99999, not 0 */
```

This takes a plain `double` array and returns a double-double, so nothing upstream has to change. It also runs **faster** than the naive loop — see [Performance](#performance-characteristics).

### Example 3: Accurate dot product

```c
#include "varintDD.h"

/* Recovers the discarded tail of every product BEFORE the addition
 * can lose it - two error-free transformations per element. */
const varintDD result = varintDDDotDoubles(a, b, count);

if (fabs(varintDDToDouble(result)) < tolerance) {
    /* a genuinely tiny dot product, not an accumulation artifact */
}
```

### Example 4: Integers past 2^53

```c
#include "varintDD.h"

/* A double holds integers exactly only to 2^53; past that they snap
 * to even values. A double-double holds every int64 exactly. */
const int64_t eventId = 9007199254740993LL;   /* 2^53 + 1 */

printf("as double: %.0f\n", (double)eventId); /* 9007199254740992 - wrong */

char text[VARINT_DD_STRING_MAX];
varintDDToString(text, sizeof(text), varintDDFromInt64(eventId), 20);
printf("as varintDD: %s\n", text);            /* exact */
```

Useful when mixing large counters with fractional scales — nanosecond timestamps converted to seconds, for instance, where a `double` loses ~200ns of resolution.

### Example 5: Iterative refinement

```c
#include "varintDD.h"

/* Solve to double precision, then refine the residual at 106 bits.
 * The residual is the difference of two nearly equal quantities -
 * exactly where a double has nothing left to say. */
varintDD x = varintDDFromDouble(approximateSolution);

for (int iteration = 0; iteration < 3; iteration++) {
    const varintDD residual = varintDDSub(target, evaluate(x));
    x = varintDDAdd(x, varintDDDiv(residual, derivative(x)));
}
```

### Example 6: Elementwise array arithmetic

```c
#include "varintDD.h"

printf("backend: %s (%zu lanes)\n",
       varintDDBackend(), varintDDBackendLanes());

varintDDAddArray(dst, a, b, count);   /* dst may alias a or b */
varintDDMulArray(dst, a, b, count);
```

The vector and scalar paths are **bit-identical**, not merely close — the test suite asserts that with `memcmp` across every remainder class.

## SIMD Details

### Where SIMD actually helps

Double-double arithmetic is branch-free straight-line floating point, which is exactly the shape an auto-vectorizer recognizes. At `-O3` clang compiles a plain

```c
for (i) dst[i] = varintDDAdd(a[i], b[i]);
```

into the same `ld2.2d` / `fadd.2d` NEON sequence `varintDDAddArray` writes by hand. **For elementwise operations the explicit backend buys nothing** beyond making the behaviour guaranteed rather than hoped for.

**Reductions are the opposite case, and it is decisive.** A compiler may not reassociate a floating-point reduction — doing so changes the result — so it cannot vectorize `acc = f(acc, x[i])` at all. The explicit backend keeps one accumulator per lane, which both breaks the serial dependency chain and reduces error growth:

| Build                      | Compensated sum | vs naive double loop |
| -------------------------- | --------------- | -------------------- |
| NEON backend               | 0.54 ns/value   | **0.57x**            |
| `-DVARINT_DD_FORCE_SCALAR` | 1.75 ns/value   | 1.77x                |

A ~2.9x swing from the identical source. And note the first row: compensated summation is _faster than the naive loop it replaces_, because splitting the accumulator across lanes wins more than the compensation arithmetic costs.

### Vector primitive layer

Rather than write add/multiply/reduce three times, `varintDD.c` abstracts the handful of primitives they need and expresses each algorithm once:

```c
#define vdblAdd(a, b)          /* _mm256_add_pd  | vaddq_f64  */
#define vdblSub(a, b)          /* _mm256_sub_pd  | vsubq_f64  */
#define vdblMul(a, b)          /* _mm256_mul_pd  | vmulq_f64  */
#define vdblMulSub(a, b, c)    /* a*b - c, single rounding    */
#define vdblMulAdd(a, b, c)    /* a*b + c, single rounding    */
```

### Deinterleave

`varintDD` is an array of structs; the vector algorithms need structure-of-arrays. NEON does this in one instruction each way (`vld2q_f64` / `vst2q_f64`); AVX2 costs an unpack pair plus a lane permute.

### Forcing the scalar path

Build with `-DVARINT_DD_FORCE_SCALAR` to compile the portable path on a machine that has a vector unit. That exists to be used: it is how the fallback gets test coverage on developer hardware (`varint-dd-scalar` and `varint-dd-stream-scalar` in ctest), and it is how the contribution of SIMD is measured — build `varintDDBench` both ways and compare, rather than trying to defeat the auto-vectorizer inside one binary.

## Performance Characteristics

Measured on an Apple M-series, 262,144 values, median of 15 runs.

### Latency (dependent chain — what an iterative kernel feels)

| Operation | `double` | `varintDD` | Ratio     |
| --------- | -------- | ---------- | --------- |
| add       | 0.93 ns  | 10.65 ns   | **11.4x** |
| multiply  | 1.40 ns  | 6.83 ns    | **4.9x**  |
| divide    | 3.43 ns  | 48.09 ns   | **14.0x** |

For comparison: soft-float `__float128` is ~20-50x, and a bignum library at the same width is ~80x.

### Throughput (independent work, L1-resident)

| Operation | plain loop | `varintDD*Array` | vs `double` |
| --------- | ---------- | ---------------- | ----------- |
| add       | 1.05 ns    | 1.17 ns          | 9.9x        |
| multiply  | 0.68 ns    | 0.81 ns          | 6.9x        |

Note a double-double array is twice the bytes of a double array, so part of any bulk slowdown is memory traffic rather than arithmetic.

### Error bounds

| Operation                     | Relative error                                                                       |
| ----------------------------- | ------------------------------------------------------------------------------------ |
| two-sum / two-product         | **exact** (0)                                                                        |
| `varintDDAdd`                 | < 2^-105 (measured 2.5e-32)                                                          |
| `varintDDMul`                 | < 2^-105 (measured 4.1e-32)                                                          |
| `varintDDDiv`, `varintDDSqrt` | ~1 ulp at 106 bits                                                                   |
| `varintDDAddFast`             | as above, **except** under leading-limb cancellation, where it degrades toward 1e-16 |

## Choosing Between the Two Additions

`varintDDAdd` (~20 flops) recovers the residual of the trailing-limb sum with a second exact transform. `varintDDAddFast` (~11 flops) folds the trailing limbs with a plain add and drops that residual.

Measured over 200,000 random pairs (`examples/standalone/example_dd.c` reports this at runtime):

| Case                       | Results differ | Worst gap                                                                     |
| -------------------------- | -------------- | ----------------------------------------------------------------------------- |
| Same sign, no cancellation | ~19%           | below the accurate form's own error bound — nothing lost                      |
| Leading limbs annihilate   | ~54%           | **~1e-16** — the cheap form has fallen back to roughly plain double precision |

**Rule of thumb**: accumulating same-sign magnitudes, use `AddFast`. Anything that can subtract, use `varintDDAdd`. When in doubt use `varintDDAdd` — its risk profile is the one you can reason about from the call site.

## Build Requirements

Every algorithm here depends on each individual `double` operation being correctly rounded to `double`, and on the compiler **not** applying real-number algebra to the expressions. Under `-ffast-math` the sequence

```c
s = a + b;  bb = s - a;  aa = s - bb;  err = (a - aa) + (b - bb);
```

simplifies to `err == 0` and the extra precision silently vanishes — the type degrades into a slow `double` and nothing announces it.

Three layers of defence:

1. `varintDD.h` **fails to compile** under `__FAST_MATH__`.
2. It also rejects `__FLT_EVAL_METHOD__ == 2` (x87 evaluating `double` as `long double`).
3. `varintDDSelfCheck()` catches everything the preprocessor cannot see — individual `-fassociative-math` style flags, reassociation introduced at LTO time, a non-default FPU rounding mode, a toolchain whose `fma()` is not correctly rounded.

**Call `varintDDSelfCheck()` once at startup** in any program whose correctness depends on the extra precision being real. It costs a few dozen flops.

```c
if (!varintDDSelfCheck()) {
    fprintf(stderr, "build does not preserve IEEE semantics\n");
    return 1;
}
```

FMA _contraction_ is a separate matter and is handled rather than forbidden: `varintDDMul` spells its fused multiply-add explicitly, which both improves accuracy and keeps the scalar and vector paths bit-identical whatever `-ffp-contract` is set to. Leaving it implicit was a real defect during development — the compiler contracted the scalar cross terms while the vector intrinsics were not contracted, and the two paths disagreed in the last bit.

## When to Use varintDD

### Use when:

- Accumulating **millions of values** where rounding compounds
- Sums are **ill-conditioned** (large terms that cancel)
- You need integers **beyond 2^53** alongside fractional values
- Iterating long enough that the **rounding floor** becomes the limit
- `double` is insufficient but `__float128` or a bignum is too slow

### Don't use when:

- **Truncation error dominates.** This is the most common mistake. In a numerical integration, the integrator's own O(dt^n) approximation is usually far larger than any rounding, and no number of significand bits will touch it. `examples/advanced/orbit_propagator.c` demonstrates both cases side by side: identical position error at both widths, 79,000x better energy conservation. Know which one you are looking at.
- You need a **numerically sound formula**. Precision buys headroom for a sound algorithm; it does not rescue an unsound one. The one-pass variance shortcut `E[x^2] - E[x]^2` still produces negative variances at 106 bits.
- You need **more exponent range** — that is unchanged from `double`.
- Values are **near the denormal floor**: full precision requires roughly `|x| > 2e-292`, below which the trailing limb goes denormal.
- **Infinities** flow through — error terms evaluate `inf - inf` and degrade to NaN.
- You need **correctly rounded** results. The guarantees here are relative error bounds, not nearest-representable.

## Implementation Details

### Source Files

- **Header**: `src/varintDD.h` (type, transformations, scalar arithmetic — mostly `static inline`)
- **Implementation**: `src/varintDD.c` (SIMD array ops, decimal conversion, self-check, tests)
- **Test entry**: `src/varintDDTest.c`
- **Benchmark**: `src/varintDDBench.c`

### Dependencies

- `varint.h` — `__BEGIN_DECLS` / `__END_DECLS`
- `<math.h>` — `fma`, `sqrt`, `ldexp`, `isfinite`

### Testing

`src/varintDD.c` (test section, `VARINT_DD_TEST`). Correctness is **not** checked against the same arithmetic under test. A separate oracle evaluates sums and products exactly in integer arithmetic:

- `ddExact` — one term as `mant * 2^exp` in a 128-bit integer. Every finite double is exactly `(-1)^s * M * 2^E` with `M < 2^53`, and the product of two doubles needs at most 106 bits.
- `ddAcc` — a sum of terms in 1536 bits of two's-complement fixed point. This is the layer 128 bits cannot do: the exact product of two double-_doubles_ spans ~212 significant bits before any alignment. Nothing here needs a big-integer multiply, because a double-double product expands into four exact double products which are then summed.

Suites: transformations bit-exact over 200k cases; normalization invariants across the full exponent range; add/multiply error bounds; the cheap add's same-sign bound; divide/sqrt round trips; every `int64` exact; reductions against the Kahan-Babuška-Neumaier criterion; decimal conversion against `printf("%" PRId64 ")` for integers and `strtod` round-trip for arbitrary values; vector/scalar bit-equality across every remainder class; limb split/join and the nonzero bitmap.

Registered as `varint-dd` and `varint-dd-scalar` in ctest.

## See Also

- [varintDDStream](varintDDStream.md) — compressing arrays of these values
- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- `examples/standalone/example_dd.c` — annotated tour of every feature here
- `examples/advanced/orbit_propagator.c` — truncation error versus rounding error

## References

- Dekker 1971, _A floating-point technique for extending the available precision_ — fast-two-sum, the splitting multiply
- Knuth, _TAOCP_ vol. 2 — the branch-free two-sum
- Hida, Li & Bailey 2001, _Algorithms for quad-double precision floating point arithmetic_ — the add/mul/div/sqrt sequences used here
- Joldes, Muller & Popescu 2017, _Tight and rigorous error bounds for basic building blocks of double-word arithmetic_ — the proven bounds the tests assert against
- Neumaier 1974 — the compensated summation variant behind `varintDDAccum`
