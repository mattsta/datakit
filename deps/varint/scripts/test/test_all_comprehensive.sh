#!/bin/bash
# Comprehensive sanitizer test suite for all varint examples
# Tests with AddressSanitizer + UndefinedBehaviorSanitizer

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# Platform-specific timeout command
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD="gtimeout"
else
    # No timeout available (fallback for macOS without coreutils)
    TIMEOUT_CMD=""
fi

# Helper function for running commands with timeout
run_with_timeout() {
    local duration=$1
    shift
    if [ -n "$TIMEOUT_CMD" ]; then
        $TIMEOUT_CMD $duration "$@"
    else
        "$@"
    fi
}

# Platform-specific sanitizer options
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS: Disable leak detection (not supported)
    export ASAN_OPTIONS=detect_leaks=0
fi

# Architecture-specific flags (x86 SIMD extensions)
ARCH=$(uname -m)
if [[ "$ARCH" == "x86_64" || "$ARCH" == "amd64" ]]; then
    ARCH_FLAGS="-mf16c -mavx2"
else
    # ARM, ARM64, or other architectures - no x86 SIMD flags
    ARCH_FLAGS=""
fi

CFLAGS="-I$REPO_ROOT/src -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
PASSED=0
FAILED=0
SKIPPED=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

declare -a FAILED_TESTS

test_example() {
    local source_file="$1"
    local extra_sources="$2"
    local extra_flags="$3"
    local name=$(basename "$source_file" .c)

    echo "========================================"
    echo "Testing: $name"
    echo "========================================"

    # Compile
    local compile_cmd="gcc $CFLAGS $extra_flags $source_file $extra_sources -o /tmp/${name}_test 2>&1"
    if ! eval $compile_cmd; then
        echo -e "${RED}✗ FAILED${NC} (compilation failed)"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$name (compile)")
        return 1
    fi

    # Run
    if run_with_timeout 5 /tmp/${name}_test > /tmp/${name}_output.txt 2>&1; then
        echo -e "${GREEN}✓ PASSED${NC}"
        PASSED=$((PASSED + 1))
        return 0
    else
        local exit_code=$?
        echo -e "${RED}✗ FAILED${NC} (exit code: $exit_code)"
        echo "--- Output (last 30 lines) ---"
        tail -30 /tmp/${name}_output.txt
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$name (runtime)")
        return 1
    fi
}

echo "=========================================="
echo "COMPREHENSIVE VARINT SANITIZER TEST SUITE"
echo "=========================================="
echo "Using: AddressSanitizer + UndefinedBehaviorSanitizer"
echo ""

# STANDALONE EXAMPLES
echo ""
echo "=========================================="
echo "STANDALONE EXAMPLES"
echo "=========================================="

test_example "examples/standalone/example_tagged.c" "src/varintTagged.c" ""
test_example "examples/standalone/example_external.c" "src/varintExternal.c" ""
test_example "examples/standalone/example_split.c" "src/varintExternal.c" ""
test_example "examples/standalone/example_chained.c" "src/varintChained.c" ""
test_example "examples/standalone/example_packed.c" "" ""
test_example "examples/standalone/example_bitstream.c" "" ""
test_example "examples/standalone/example_dimension.c" "src/varintDimension.c src/varintExternal.c" "$ARCH_FLAGS"
test_example "examples/standalone/rle_codec.c" "src/varintExternal.c" ""
test_example "examples/standalone/example_delta.c" "src/varintDelta.c src/varintExternal.c" ""
test_example "examples/standalone/example_for.c" "src/varintFOR.c src/varintExternal.c src/varintTagged.c" ""
test_example "examples/standalone/example_group.c" "src/varintGroup.c src/varintExternal.c" ""
test_example "examples/standalone/example_pfor.c" "src/varintPFOR.c src/varintExternal.c src/varintTagged.c" ""
test_example "examples/standalone/example_dict.c" "src/varintDict.c src/varintExternal.c src/varintTagged.c" ""
test_example "examples/standalone/example_bitmap.c" "src/varintBitmap.c src/varintExternal.c" ""

# INTEGRATION EXAMPLES (may need multiple source files)
echo ""
echo "=========================================="
echo "INTEGRATION EXAMPLES"
echo "=========================================="

test_example "examples/integration/game_engine.c" "" ""
test_example "examples/integration/network_protocol.c" "src/varintChained.c src/varintExternal.c" ""
test_example "examples/integration/sensor_network.c" "src/varintExternal.c" ""
test_example "examples/integration/column_store.c" "src/varintDimension.c src/varintExternal.c" "$ARCH_FLAGS"
test_example "examples/integration/ml_features.c" "src/varintDimension.c src/varintExternal.c" "$ARCH_FLAGS"
test_example "examples/integration/database_system.c" "src/varintTagged.c src/varintExternal.c" ""
test_example "examples/integration/vector_clock.c" "src/varintTagged.c" ""
test_example "examples/integration/delta_compression.c" "src/varintExternal.c" "-lm"
test_example "examples/integration/sparse_matrix_csr.c" "src/varintDimension.c src/varintExternal.c" "-lm $ARCH_FLAGS"

# ADVANCED EXAMPLES
echo ""
echo "=========================================="
echo "ADVANCED EXAMPLES"
echo "=========================================="

# Trie examples need special handling (server runs in background)
echo "Testing: trie_server (background process)"

# Kill any existing trie_server processes and free port 9999
pkill -9 -f trie_server_test 2>/dev/null || true
sleep 0.5

if gcc $CFLAGS examples/advanced/trie_server.c src/varintTagged.c -o /tmp/trie_server_test 2>&1; then
    /tmp/trie_server_test > /tmp/trie_server_output.txt 2>&1 &
    TRIE_SERVER_PID=$!
    sleep 1
    if kill -0 $TRIE_SERVER_PID 2>/dev/null; then
        echo -e "${GREEN}✓ PASSED${NC} (server started)"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗ FAILED${NC} (server crashed)"
        cat /tmp/trie_server_output.txt
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("trie_server")
        TRIE_SERVER_PID=""
    fi
else
    echo -e "${RED}✗ FAILED${NC} (compilation)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("trie_server (compile)")
    TRIE_SERVER_PID=""
fi

# Test trie client (connects to server)
if [ -n "$TRIE_SERVER_PID" ]; then
    echo "========================================"
    echo "Testing: trie_client"
    echo "========================================"

    # Compile client
    if gcc $CFLAGS examples/advanced/trie_client.c src/varintTagged.c -o /tmp/trie_client_test 2>&1; then
        # Test with ping command
        if run_with_timeout 2 /tmp/trie_client_test ping 127.0.0.1 9999 > /tmp/trie_client_output.txt 2>&1; then
            echo -e "${GREEN}✓ PASSED${NC}"
            PASSED=$((PASSED + 1))
        else
            echo -e "${RED}✗ FAILED${NC} (runtime)"
            cat /tmp/trie_client_output.txt
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("trie_client (runtime)")
        fi
    else
        echo -e "${RED}✗ FAILED${NC} (compilation)"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("trie_client (compile)")
    fi

    # Graceful shutdown via SHUTDOWN command
    /tmp/trie_client_test shutdown 127.0.0.1 9999 > /dev/null 2>&1 || kill $TRIE_SERVER_PID 2>/dev/null || true
    wait $TRIE_SERVER_PID 2>/dev/null || true
fi

# Other advanced examples
test_example "examples/advanced/inverted_index.c" "src/varintChained.c src/varintExternal.c src/varintTagged.c" "-lm"
test_example "examples/advanced/blockchain_ledger.c" "src/varintChained.c src/varintExternal.c src/varintTagged.c" "-lm"
test_example "examples/advanced/bytecode_vm.c" "src/varintChained.c src/varintExternal.c src/varintTagged.c" ""
test_example "examples/advanced/dns_server.c" "src/varintChained.c src/varintExternal.c" ""
test_example "examples/advanced/financial_orderbook.c" "src/varintExternal.c src/varintTagged.c" ""
test_example "examples/advanced/game_replay_system.c" "src/varintExternal.c" "-lm"
test_example "examples/advanced/geospatial_routing.c" "src/varintExternal.c src/varintTagged.c" "-lm"
test_example "examples/advanced/log_aggregation.c" "src/varintChained.c src/varintExternal.c" ""
test_example "examples/advanced/bloom_filter.c" "src/varintChained.c src/varintExternal.c" "-lm"
test_example "examples/advanced/autocomplete_trie.c" "src/varintExternal.c src/varintTagged.c" ""
test_example "examples/advanced/pointcloud_octree.c" "src/varintExternal.c src/varintDimension.c" "-lm $ARCH_FLAGS"

# Trie pattern matcher and interactive need stdin
echo ""
echo "Testing: trie_pattern_matcher (core functionality tests)"
if gcc $CFLAGS examples/advanced/trie_pattern_matcher.c src/varintChained.c src/varintExternal.c src/varintTagged.c -o /tmp/trie_pattern_matcher_test 2>&1; then
    # Core tests only (heavy benchmarks skipped with sanitizers)
    run_with_timeout 10 /tmp/trie_pattern_matcher_test > /tmp/trie_pattern_matcher_output.txt 2>&1 && {
        echo -e "${GREEN}✓ PASSED${NC}"
        PASSED=$((PASSED + 1))
    } || {
        echo -e "${RED}✗ FAILED${NC}"
        tail -20 /tmp/trie_pattern_matcher_output.txt
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("trie_pattern_matcher")
    }
else
    echo -e "${RED}✗ FAILED${NC} (compilation)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("trie_pattern_matcher (compile)")
fi

echo ""
echo "Testing: trie_interactive (with test input)"
if gcc $CFLAGS examples/advanced/trie_interactive.c src/varintTagged.c -o /tmp/trie_interactive_test 2>&1; then
    echo -e "insert test\ninsert hello\nsearch test\nquit" | run_with_timeout 2 /tmp/trie_interactive_test > /tmp/trie_interactive_output.txt 2>&1 && {
        echo -e "${GREEN}✓ PASSED${NC}"
        PASSED=$((PASSED + 1))
    } || {
        echo -e "${RED}✗ FAILED${NC}"
        tail -20 /tmp/trie_interactive_output.txt
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("trie_interactive")
    }
else
    echo -e "${RED}✗ FAILED${NC} (compilation)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("trie_interactive (compile)")
fi

# REFERENCE EXAMPLES
echo ""
echo "=========================================="
echo "REFERENCE EXAMPLES"
echo "=========================================="

test_example "examples/reference/kv_store.c" "src/varintTagged.c" ""
test_example "examples/reference/graph_database.c" "src/varintDimension.c src/varintExternal.c" "$ARCH_FLAGS"
test_example "examples/reference/timeseries_db.c" "src/varintExternal.c src/varintChained.c" ""

# SUMMARY
echo ""
echo "=========================================="
echo "TEST SUMMARY"
echo "=========================================="
echo -e "Passed:  ${GREEN}$PASSED${NC}"
echo -e "Failed:  ${RED}$FAILED${NC}"
echo -e "Skipped: ${YELLOW}$SKIPPED${NC}"
echo "Total:   $((PASSED + FAILED + SKIPPED))"

if [ $FAILED -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  - $test"
    done
    exit 1
else
    echo ""
    echo -e "${GREEN}ALL TESTS PASSED!${NC}"
    exit 0
fi
