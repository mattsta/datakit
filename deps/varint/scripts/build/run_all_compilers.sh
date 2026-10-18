#!/bin/bash
# Comprehensive compiler warning verification for varint library
# Tests with both GCC and Clang strict warnings
# Shows ALL output inline - does NOT hide warnings

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$REPO_ROOT/compiler_check_results"
mkdir -p "$RESULTS_DIR"

echo "========================================"
echo "Varint Library Compiler Warning Check"
echo "========================================"
echo ""
echo "Testing with GCC and Clang strict warnings..."
echo ""

# Run GCC check - SHOW ALL OUTPUT INLINE (using relative path)
echo "=== Running GCC Check ==="
echo ""
"${SCRIPT_DIR}/check_warnings.sh" gcc 2>&1 | tee "$RESULTS_DIR/gcc_results.txt"
GCC_EXIT=$?
echo ""

# Run Clang check - SHOW ALL OUTPUT INLINE (using relative path)
echo "=== Running Clang Check ==="
echo ""
"${SCRIPT_DIR}/check_warnings.sh" clang 2>&1 | tee "$RESULTS_DIR/clang_results.txt"
CLANG_EXIT=$?
echo ""

# Determine status based on exit codes
if [ $GCC_EXIT -eq 0 ]; then
    GCC_STATUS="PASS"
else
    GCC_STATUS="FAIL"
fi

if [ $CLANG_EXIT -eq 0 ]; then
    CLANG_STATUS="PASS"
else
    CLANG_STATUS="FAIL"
fi

echo ""
echo "========================================"
echo "Summary"
echo "========================================"
echo "GCC:   $GCC_STATUS (exit code: $GCC_EXIT)"
echo "Clang: $CLANG_STATUS (exit code: $CLANG_EXIT)"
echo ""
echo "Detailed results saved to: $RESULTS_DIR/"
echo ""

# Create summary.txt file for CI verification
cat > "$RESULTS_DIR/summary.txt" << EOF
GCC:   $GCC_STATUS
Clang: $CLANG_STATUS
EOF

# Exit with error if either compiler failed
if [ "$GCC_STATUS" = "FAIL" ] || [ "$CLANG_STATUS" = "FAIL" ]; then
    echo "❌ COMPILER CHECK FAILED"
    echo ""
    echo "Check the output above for specific warnings and errors."
    echo "All warnings must be fixed before merging."
    exit 1
fi

echo "✅ ALL COMPILER CHECKS PASSED"
exit 0
