#!/bin/bash
# Unit test runner for 8 new varint encodings
# Uses ctest.h framework with proper assertions

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# Platform-specific timeout command
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD="timeout 5"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD="gtimeout 5"
else
    # No timeout available (fallback for macOS without coreutils)
    TIMEOUT_CMD=""
fi

# Platform-specific sanitizer options
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS: Disable leak detection (not supported)
    export ASAN_OPTIONS=detect_leaks=0
fi

CFLAGS="-I$REPO_ROOT/src -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -Wall -Wextra"
PASSED=0
FAILED=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

declare -a FAILED_TESTS

run_test() {
    local test_name="$1"
    local source_files="$2"
    local standalone_flag="$3"

    echo "========================================"
    echo "Testing: $test_name"
    echo "========================================"

    # Compile
    if ! gcc $CFLAGS -D${standalone_flag} $source_files -o /tmp/${test_name}_test 2>&1; then
        echo -e "${RED}✗ COMPILATION FAILED${NC}"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$test_name (compile)")
        return 1
    fi

    # Run
    if ${TIMEOUT_CMD} /tmp/${test_name}_test > /tmp/${test_name}_output.txt 2>&1; then
        local exit_code=$?
        if [ $exit_code -eq 0 ]; then
            echo -e "${GREEN}✓ ALL TESTS PASSED${NC}"
            PASSED=$((PASSED + 1))
            return 0
        else
            echo -e "${RED}✗ TESTS FAILED (exit code: $exit_code)${NC}"
            echo "--- Output ---"
            cat /tmp/${test_name}_output.txt
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_name (runtime)")
            return 1
        fi
    else
        echo -e "${RED}✗ TIMEOUT OR CRASH${NC}"
        echo "--- Output ---"
        cat /tmp/${test_name}_output.txt
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$test_name (timeout)")
        return 1
    fi
}

echo "=========================================="
echo "VARINT UNIT TEST SUITE (NEW ENCODINGS)"
echo "=========================================="
echo "Using: AddressSanitizer + UndefinedBehaviorSanitizer"
echo ""

# Run all 8 unit tests
run_test "varintDelta" \
    "src/varintDeltaTest.c src/varintDelta.c src/varintExternal.c" \
    "VARINT_DELTA_TEST_STANDALONE"

run_test "varintFOR" \
    "src/varintFORTest.c src/varintFOR.c src/varintExternal.c src/varintTagged.c" \
    "VARINT_FOR_TEST_STANDALONE"

run_test "varintGroup" \
    "src/varintGroupTest.c src/varintGroup.c src/varintExternal.c src/varintTagged.c" \
    "VARINT_GROUP_TEST_STANDALONE"

run_test "varintPFOR" \
    "src/varintPFORTest.c src/varintPFOR.c src/varintExternal.c src/varintTagged.c" \
    "VARINT_PFOR_TEST_STANDALONE"

run_test "varintDict" \
    "src/varintDictTest.c src/varintDict.c src/varintExternal.c src/varintTagged.c" \
    "VARINT_DICT_TEST_STANDALONE"

run_test "varintBitmap" \
    "src/varintBitmapTest.c src/varintBitmap.c src/varintExternal.c" \
    "VARINT_BITMAP_TEST_STANDALONE"

run_test "varintAdaptive" \
    "src/varintAdaptiveTest.c src/varintAdaptive.c src/varintDelta.c src/varintFOR.c src/varintPFOR.c src/varintDict.c src/varintBitmap.c src/varintExternal.c src/varintTagged.c" \
    "VARINT_ADAPTIVE_TEST_STANDALONE"

run_test "varintFloat" \
    "src/varintFloatTest.c src/varintFloat.c src/varintExternal.c src/varintTagged.c src/varintDelta.c" \
    "VARINT_FLOAT_TEST_STANDALONE"

# Summary
echo ""
echo "=========================================="
echo "TEST SUMMARY"
echo "=========================================="
echo -e "Passed:  ${GREEN}$PASSED${NC}"
echo -e "Failed:  ${RED}$FAILED${NC}"
echo "Total:   $((PASSED + FAILED))"

if [ $FAILED -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  - $test"
    done
    exit 1
else
    echo ""
    echo -e "${GREEN}ALL UNIT TESTS PASSED!${NC}"
    exit 0
fi
