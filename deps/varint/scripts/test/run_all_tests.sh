#!/bin/bash
# Comprehensive test runner for varint library
# Compiles and runs all test files with optional sanitizers

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SANITIZER="${1:-none}"
BUILD_DIR="$REPO_ROOT/build_tests"
SRC_DIR="$REPO_ROOT/src"

# Sanitizer flags
if [ "$SANITIZER" = "asan" ]; then
    SAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -O1 -g"
    echo "Running with AddressSanitizer"
elif [ "$SANITIZER" = "ubsan" ]; then
    SAN_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -O1 -g"
    echo "Running with UndefinedBehaviorSanitizer"
elif [ "$SANITIZER" = "both" ]; then
    SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g"
    echo "Running with ASan + UBSan"
else
    SAN_FLAGS="-O2"
    echo "Running without sanitizers"
fi

mkdir -p "$BUILD_DIR"
cd "$SRC_DIR"

TESTS=(
    "varintDeltaTest"
    "varintFORTest"
    "varintPFORTest"
    "varintFloatTest"
    "varintBitmapTest"
    "varintDictTest"
    "varintAdaptiveTest"
    "varintGroupTest"
)

echo "========================================"
echo "Building and Running Varint Tests"
echo "Sanitizer: $SANITIZER"
echo "========================================"
echo ""

PASSED=0
FAILED=0

for test in "${TESTS[@]}"; do
    echo "=== $test ==="

    # Map test name to the correct define
    case "$test" in
        varintDeltaTest) DEFINE="-DVARINT_DELTA_TEST_STANDALONE" ;;
        varintFORTest) DEFINE="-DVARINT_FOR_TEST_STANDALONE" ;;
        varintPFORTest) DEFINE="-DVARINT_PFOR_TEST_STANDALONE" ;;
        varintFloatTest) DEFINE="-DVARINT_FLOAT_TEST_STANDALONE" ;;
        varintBitmapTest) DEFINE="-DVARINT_BITMAP_TEST_STANDALONE" ;;
        varintDictTest) DEFINE="-DVARINT_DICT_TEST_STANDALONE" ;;
        varintAdaptiveTest) DEFINE="-DVARINT_ADAPTIVE_TEST_STANDALONE" ;;
        varintGroupTest) DEFINE="-DVARINT_GROUP_TEST_STANDALONE" ;;
        *) DEFINE="" ;;
    esac

    # Compile - include all varint implementation files except tests and demo
    if gcc $SAN_FLAGS $DEFINE -I. -o "$BUILD_DIR/$test" "${test}.c" \
        varintExternal.c varintExternalBigEndian.c varintTagged.c \
        varintDelta.c varintFOR.c varintPFOR.c varintFloat.c \
        varintBitmap.c varintDict.c varintAdaptive.c varintGroup.c \
        varintChained.c varintChainedSimple.c \
        -lm 2>&1 | tee "$BUILD_DIR/${test}_build.log"; then

        # Run
        if "$BUILD_DIR/$test" 2>&1 | tee "$BUILD_DIR/${test}_run.log"; then
            echo "✓ PASSED"
            PASSED=$((PASSED + 1))
        else
            echo "✗ FAILED (runtime error)"
            FAILED=$((FAILED + 1))
        fi
    else
        echo "✗ FAILED (compilation error)"
        FAILED=$((FAILED + 1))
    fi
    echo ""
done

echo "========================================"
echo "Test Results"
echo "========================================"
echo "Passed: $PASSED/${#TESTS[@]}"
echo "Failed: $FAILED/${#TESTS[@]}"
echo ""

if [ $FAILED -gt 0 ]; then
    echo "Some tests failed. Check logs in: $BUILD_DIR/"
    exit 1
fi

echo "All tests passed!"
exit 0
