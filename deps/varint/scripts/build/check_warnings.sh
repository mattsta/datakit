#!/bin/bash
# Automated warning checker for varint library
# Checks all source files with both GCC and Clang strict warnings
# SHOWS ALL WARNINGS INLINE - does NOT hide anything

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPILER="${1:-clang}"
SRC_DIR="$REPO_ROOT/src"
FAILED_FILES=()
TOTAL=0
PASSED=0

echo "================================="
echo "Checking with: $COMPILER"
echo "================================="
echo ""

if [ "$COMPILER" = "clang" ]; then
    FLAGS="-O2 -Wall -Wextra -Wpedantic -Wsign-conversion -Wconversion -Werror"
else
    FLAGS="-O2 -Wall -Wextra -Werror"
fi

# Find all .c files
for file in $(find "$SRC_DIR" -name "*.c" | sort); do
    TOTAL=$((TOTAL + 1))
    printf "Checking %-40s ... " "$file"

    if $COMPILER $FLAGS -I "$SRC_DIR" -c "$file" -o /tmp/test.o 2>/tmp/err.log; then
        echo "✓"
        PASSED=$((PASSED + 1))
    else
        echo "✗ FAILED"
        FAILED_FILES+=("$file")
        echo ""
        echo "────────────────────────────────────────"
        echo "ERRORS in $file:"
        echo "────────────────────────────────────────"
        cat /tmp/err.log  # SHOW COMPLETE ERROR OUTPUT
        echo "────────────────────────────────────────"
        echo ""
    fi
done

echo ""
echo "================================="
echo "Results: $PASSED/$TOTAL files passed"
echo "================================="

if [ ${#FAILED_FILES[@]} -gt 0 ]; then
    echo ""
    echo "❌ FAILED FILES (${#FAILED_FILES[@]} total):"
    for file in "${FAILED_FILES[@]}"; do
        echo "  - $file"
    done
    echo ""
    echo "All warnings shown inline above."
    exit 1
fi

echo ""
echo "✅ ALL FILES CLEAN"
exit 0
