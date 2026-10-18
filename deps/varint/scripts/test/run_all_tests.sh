#!/bin/bash
# Comprehensive test runner for the varint library.
#
# Builds the full CMake tree with the requested sanitizer into a
# dedicated build directory, then runs every registered ctest suite.
# CMakeLists.txt is the single source of truth for the test list, the
# per-test defines, and the per-test source dependencies — a suite added
# there is automatically covered here, so this script never drifts out
# of sync with the tree the way a hand-maintained test list does.
#
# Usage: run_all_tests.sh [none|asan|ubsan|both]   (default: none)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SANITIZER="${1:-none}"
BUILD_DIR="$REPO_ROOT/build_tests/$SANITIZER"

case "$SANITIZER" in
    asan)
        SAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -O1 -g"
        echo "Running with AddressSanitizer"
        ;;
    ubsan)
        SAN_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -O1 -g"
        echo "Running with UndefinedBehaviorSanitizer"
        ;;
    both)
        SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g"
        echo "Running with ASan + UBSan"
        ;;
    none)
        SAN_FLAGS=""
        echo "Running without sanitizers"
        ;;
    *)
        echo "Unknown sanitizer '$SANITIZER' (want: none|asan|ubsan|both)" >&2
        exit 2
        ;;
esac

echo "========================================"
echo "Building and Running Varint Tests"
echo "Sanitizer: $SANITIZER"
echo "Build dir: $BUILD_DIR"
echo "========================================"
echo ""

# Debug keeps the tree at -O0 so the sanitizer flags' -O1 governs and
# heavy optimization doesn't mask the diagnostics being tested for.
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="$SAN_FLAGS" \
    > "$BUILD_DIR.configure.log" 2>&1 || {
        cat "$BUILD_DIR.configure.log"
        echo "✗ FAILED (configure error)"
        exit 1
    }

if ! cmake --build "$BUILD_DIR" -j > "$BUILD_DIR.build.log" 2>&1; then
    tail -50 "$BUILD_DIR.build.log"
    echo "✗ FAILED (compilation error) — full log: $BUILD_DIR.build.log"
    exit 1
fi

if ! ctest --test-dir "$BUILD_DIR" --output-on-failure; then
    echo ""
    echo "Some tests failed."
    exit 1
fi

echo ""
echo "All tests passed!"
exit 0
