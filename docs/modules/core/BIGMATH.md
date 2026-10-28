# bigmath: arbitrary-precision integers

**Header**: `bigmath.h` **Source**: `bigmath-imath.c` (the backend built today; `bigmath-libbf.c` is the
alternative)

`bigmath` is a signed integer of any size. The `bigmath` struct is a 64-byte buffer the caller owns (a stack
local or a field); the backend keeps small values inside it and spills larger ones to the heap. datakit
uses it for exact decimal formatting of doubles (`strDoubleFormat.c`); any code that needs integers past
128 bits can use it the same way.

## Lifecycle

Every value goes through one Init and one end:

| Start                                                                                 | End                                                                              |
| ------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| `bigmathInit(b)` (zero)                                                               | `bigmathFree(b)`: releases what the value spilled to the heap (never the struct) |
| `bigmathInitUnsigned(b, u)` / `bigmathInitSigned(b, i)`                               | `bigmathReset(b)`: zero and reusable, the heap released too                      |
| `bigmathInitFromString(b, "123...")` (base 10, NUL-terminated; false on a bad string) |                                                                                  |
| `bigmathInitCopy(dst, src)`                                                           |                                                                                  |

A `Reset` struct can be used again directly or re-Init'd. Pair each Init with Reset or Free on every path,
error paths included: a value that grew past the inline buffer holds heap memory.

## Arithmetic

| Call                                        | Result                                                                                                           |
| ------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| `bigmathAdd(a, b, result)`                  | a + b                                                                                                            |
| `bigmathMultiply(a, b, result)`             | a \* b                                                                                                           |
| `bigmathMultiplyValue(a, v, result)`        | a \* v (v an `int64_t`)                                                                                          |
| `bigmathShiftLeft(b, n, result)`            | b \* 2^n                                                                                                         |
| `bigmathExponent(base, exp, result)`        | base^exp (both `uint64_t`)                                                                                       |
| `bigmathDivideRemainder(top, bottom, q, r)` | q = top / bottom, r = top % bottom                                                                               |
| `bigmathDivideDigitInPlace(top, bottom)`    | for 0 <= top < 10 \* bottom: returns top / bottom (one digit) and leaves top % bottom in top, without allocating |
| `bigmathCompare(a, b)`                      | <0, 0, >0                                                                                                        |

Every result must be an initialized value (Init it first; it is overwritten). A result may alias an
operand (`bigmathAdd(a, b, a)` is fine), but `DivideRemainder`'s quotient and remainder must be two
different values.

## Reading values out

- `bigmathToNativeUnsigned(b)` / `bigmathToNativeSigned(b)`: the value as a native integer. It must fit:
  out of range asserts in debug builds and returns 0 in release builds. Check with `bigmathCompare` against
  the type's limits first when the value may not fit.
- `bigmathRepr(b)` / `bigmathReprFile(b, fp)`: the value in base 10, in full, and a newline.

## Example: 2^200 / 3

```c
#include "bigmath.h"

bigmath n, three, q, r;
bigmathInit(&n); /* every result is an initialized value */
bigmathInit(&q);
bigmathInit(&r);
bigmathInitUnsigned(&three, 3);
bigmathExponent(2, 200, &n);

bigmathDivideRemainder(&n, &three, &q, &r);
bigmathRepr(&q);                                  /* 535...0485 */
const uint64_t rest = bigmathToNativeUnsigned(&r); /* 1: fits */

bigmathFree(&n);
bigmathFree(&three);
bigmathFree(&q);
bigmathFree(&r);
```

Not thread-safe per value; separate values are independent.
