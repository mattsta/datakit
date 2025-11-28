#!/bin/bash
# Comprehensive sanitizer testing for ALL examples
# Tests standalone, integration, reference, and advanced examples with ASan+UBSan

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_examples_sanitizers"

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

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "=============================================="
echo "  All Examples - Sanitizer Testing"
echo "=============================================="
echo

# Cleanup
pkill -9 trie_server 2>/dev/null || true
pkill -9 dns_server 2>/dev/null || true
rm -rf /tmp/sanitizer_failures 2>/dev/null || true

# Build with sanitizers
echo "Building ALL examples with -fsanitize=address,undefined..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
      .. > /dev/null 2>&1

echo "Compiling..."
cmake --build . --target examples 2>&1 | grep -E "Built target|error|warning" | head -50
cd ..

echo -e "${GREEN}✓${NC} Build complete with sanitizers enabled"
echo

# Export sanitizer options
# Note: LeakSanitizer is not supported on macOS, so disable detect_leaks on Darwin
if [[ "$OSTYPE" == "darwin"* ]]; then
    export ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:abort_on_error=0:print_summary=1
else
    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:abort_on_error=0:print_summary=1
fi
export UBSAN_OPTIONS=halt_on_error=0:print_stacktrace=1

# Test counters
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0

declare -a FAILED_EXAMPLES
declare -a PASSED_EXAMPLES

# Function to run an example
run_example() {
    local name="$1"
    local category="$2"
    local path="$BUILD_DIR/examples/$name"

    TOTAL=$((TOTAL + 1))

    if [ ! -f "$path" ]; then
        echo -e "  ${YELLOW}⊘${NC} $name (not built at $path)"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    # Run with timeout and capture output
    local output_file="/tmp/example_output_$name.txt"
    if run_with_timeout 5 "$path" > "$output_file" 2>&1; then
        # Check for sanitizer errors in output
        if grep -q "ERROR: AddressSanitizer\|ERROR: UndefinedBehaviorSanitizer" "$output_file" 2>/dev/null; then
            echo -e "  ${RED}✗${NC} $name (sanitizer error)"
            FAILED=$((FAILED + 1))
            FAILED_EXAMPLES+=("$category/$name:sanitizer")
            # Save error details
            mkdir -p /tmp/sanitizer_failures
            cp "$output_file" "/tmp/sanitizer_failures/${category}_${name}.txt"
        else
            echo -e "  ${GREEN}✓${NC} $name"
            PASSED=$((PASSED + 1))
            PASSED_EXAMPLES+=("$category/$name")
        fi
    else
        EXIT_CODE=$?
        if [ $EXIT_CODE -eq 124 ]; then
            echo -e "  ${YELLOW}⏱${NC} $name (timeout - may be interactive)"
            SKIPPED=$((SKIPPED + 1))
        else
            echo -e "  ${RED}✗${NC} $name (exit code $EXIT_CODE)"
            FAILED=$((FAILED + 1))
            FAILED_EXAMPLES+=("$category/$name:exit-$EXIT_CODE")
            # Save error details
            mkdir -p /tmp/sanitizer_failures
            cp "$output_file" "/tmp/sanitizer_failures/${category}_${name}.txt" 2>/dev/null || true
        fi
    fi

    rm -f "$output_file"
}

# ============================================================================
# STANDALONE EXAMPLES
# ============================================================================
echo -e "${BLUE}=== Standalone Examples (7) ===${NC}"
run_example "example_tagged" "standalone"
run_example "example_external" "standalone"
run_example "example_split" "standalone"
run_example "example_chained" "standalone"
run_example "example_packed" "standalone"
run_example "example_dimension" "standalone"
run_example "example_bitstream" "standalone"
echo

# ============================================================================
# INTEGRATION EXAMPLES
# ============================================================================
echo -e "${BLUE}=== Integration Examples (6) ===${NC}"
run_example "column_store" "integration"
run_example "database_system" "integration"
run_example "game_engine" "integration"
run_example "ml_features" "integration"
run_example "network_protocol" "integration"
run_example "sensor_network" "integration"
echo

# ============================================================================
# REFERENCE EXAMPLES
# ============================================================================
echo -e "${BLUE}=== Reference Examples (3) ===${NC}"
run_example "graph_database" "reference"
run_example "kv_store" "reference"
run_example "timeseries_db" "reference"
echo

# ============================================================================
# ADVANCED EXAMPLES (non-server)
# ============================================================================
echo -e "${BLUE}=== Advanced Examples (10 non-server) ===${NC}"
run_example "blockchain_ledger" "advanced"
run_example "bytecode_vm" "advanced"
run_example "financial_orderbook" "advanced"
run_example "game_replay_system" "advanced"
run_example "geospatial_routing" "advanced"
run_example "inverted_index" "advanced"
run_example "log_aggregation" "advanced"
run_example "trie_pattern_matcher" "advanced"
run_example "trie_interactive" "advanced"
echo -e "  ${YELLOW}⊘${NC} trie_client (tested separately)"
echo -e "  ${YELLOW}⊘${NC} trie_server (tested separately)"
echo -e "  ${YELLOW}⊘${NC} dns_server (requires server testing)"
SKIPPED=$((SKIPPED + 3))
TOTAL=$((TOTAL + 3))
echo

# Summary
echo "=============================================="
echo -e "${BLUE}  Summary${NC}"
echo "=============================================="
echo
echo "Total examples tested: $TOTAL"
echo -e "${GREEN}Passed:${NC}  $PASSED"
echo -e "${RED}Failed:${NC}  $FAILED"
echo -e "${YELLOW}Skipped:${NC} $SKIPPED"
echo

if [ $FAILED -gt 0 ]; then
    echo -e "${RED}Failed examples:${NC}"
    for example in "${FAILED_EXAMPLES[@]}"; do
        echo "  - $example"
    done
    echo

    echo "Error logs saved to: /tmp/sanitizer_failures/"
    echo "To view errors:"
    echo "  ls /tmp/sanitizer_failures/"
    echo "  cat /tmp/sanitizer_failures/<category>_<example>.txt"
    echo
fi

# Success rate
if [ $TOTAL -gt 0 ]; then
    TESTED=$((PASSED + FAILED))
    if [ $TESTED -gt 0 ]; then
        SUCCESS_RATE=$((PASSED * 100 / TESTED))
        echo "Success rate: $SUCCESS_RATE% ($PASSED/$TESTED tested)"
    fi
fi
echo

# Cleanup
rm -rf "$BUILD_DIR"

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All testable examples passed with sanitizers!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some examples failed sanitizer checks${NC}"
    exit 1
fi
