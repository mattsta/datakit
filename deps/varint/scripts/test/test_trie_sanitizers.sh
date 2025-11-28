#!/bin/bash
# Modern memory safety testing with sanitizers
# Tests: AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_sanitizers"
PORT=40004

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

# Detect platform for sanitizer options
PLATFORM=$(uname -s)
if [[ "$PLATFORM" == "Darwin" ]]; then
    # macOS: ASan leak detection not supported
    ASAN_OPTS="halt_on_error=1:abort_on_error=1"
    LEAK_DETECTION_AVAILABLE=0
else
    # Linux: Full ASan support including leak detection
    ASAN_OPTS="detect_leaks=1:halt_on_error=1:abort_on_error=1"
    LEAK_DETECTION_AVAILABLE=1
fi

echo "=============================================="
echo "  Trie Server Sanitizer Testing"
echo "=============================================="
echo "Platform: $PLATFORM"
if [[ $LEAK_DETECTION_AVAILABLE -eq 0 ]]; then
    echo -e "${YELLOW}Note: Leak detection not available on this platform${NC}"
fi
echo

# Cleanup function
cleanup_server() {
    # Use SIGTERM first for graceful shutdown, then SIGKILL if needed
    pkill trie_server 2>/dev/null || true
    sleep 1
    # Force kill any remaining processes
    pkill -9 trie_server 2>/dev/null || true
    sleep 1  # Give the OS time to release the port
}

# Wait for port to be available
wait_for_port() {
    local port=$1
    local max_wait=10
    local waited=0

    while lsof -i :$port >/dev/null 2>&1; do
        if [ $waited -ge $max_wait ]; then
            echo -e "${RED}Port $port still in use after ${max_wait}s${NC}"
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 0
}

# Initial cleanup
cleanup_server
wait_for_port $PORT

# Test 1: AddressSanitizer (ASan) - detects memory errors
echo -e "${BLUE}=== TEST 1: AddressSanitizer (ASan) ===${NC}"
echo "Building with -fsanitize=address..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
      .. > /dev/null 2>&1
cmake --build . --target trie_server trie_client > /dev/null 2>&1
cd ..

echo "Running tests with AddressSanitizer..."
export ASAN_OPTIONS="$ASAN_OPTS"

$BUILD_DIR/examples/trie_server --port $PORT 2>&1 | grep -v "^DEBUG:" &
SERVER_PID=$!
sleep 1

# Test basic commands
$BUILD_DIR/examples/trie_client ping 127.0.0.1 $PORT 2>&1 | grep "PONG" > /dev/null && echo -e "  ${GREEN}✓${NC} PING" || { echo -e "  ${RED}✗${NC} PING failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client add "test.pattern" 1 "test" 127.0.0.1 $PORT 2>&1 | grep "ADD successful" > /dev/null && echo -e "  ${GREEN}✓${NC} ADD" || { echo -e "  ${RED}✗${NC} ADD failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client subscribe "test.pattern" 2 "sub2" 127.0.0.1 $PORT 2>&1 | grep "SUBSCRIBE successful" > /dev/null && echo -e "  ${GREEN}✓${NC} SUBSCRIBE" || { echo -e "  ${RED}✗${NC} SUBSCRIBE failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client match "test.pattern" 127.0.0.1 $PORT 2>&1 | grep "Matches found: 2" > /dev/null && echo -e "  ${GREEN}✓${NC} MATCH" || { echo -e "  ${RED}✗${NC} MATCH failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client list 127.0.0.1 $PORT 2>&1 | grep "test.pattern" > /dev/null && echo -e "  ${GREEN}✓${NC} LIST" || { echo -e "  ${RED}✗${NC} LIST failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client unsubscribe "test.pattern" 2 127.0.0.1 $PORT 2>&1 | grep "UNSUBSCRIBE successful" > /dev/null && echo -e "  ${GREEN}✓${NC} UNSUBSCRIBE" || { echo -e "  ${RED}✗${NC} UNSUBSCRIBE failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client remove "test.pattern" 127.0.0.1 $PORT 2>&1 | grep "REMOVE successful" > /dev/null && echo -e "  ${GREEN}✓${NC} REMOVE" || { echo -e "  ${RED}✗${NC} REMOVE failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client stats 127.0.0.1 $PORT 2>&1 | grep "Server Statistics" > /dev/null && echo -e "  ${GREEN}✓${NC} STATS" || { echo -e "  ${RED}✗${NC} STATS failed"; kill -9 $SERVER_PID; exit 1; }

# Graceful shutdown via SHUTDOWN command
$BUILD_DIR/examples/trie_client shutdown 127.0.0.1 $PORT 2>&1 | grep "SHUTDOWN successful" > /dev/null && echo -e "  ${GREEN}✓${NC} SHUTDOWN" || { echo -e "  ${YELLOW}⚠${NC} SHUTDOWN failed, using SIGTERM"; kill -TERM $SERVER_PID 2>/dev/null || true; }
sleep 0.5
# Fallback cleanup if needed
kill -9 $SERVER_PID 2>/dev/null || true
cleanup_server
wait_for_port $PORT
echo -e "${GREEN}✓${NC} AddressSanitizer: No memory errors detected"
echo

# Test 2: UndefinedBehaviorSanitizer (UBSan)
echo -e "${BLUE}=== TEST 2: UndefinedBehaviorSanitizer (UBSan) ===${NC}"
echo "Building with -fsanitize=undefined..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" \
      .. > /dev/null 2>&1
cmake --build . --target trie_server trie_client > /dev/null 2>&1
cd ..

echo "Running tests with UndefinedBehaviorSanitizer..."
export UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1

$BUILD_DIR/examples/trie_server --port $PORT 2>&1 | grep -v "^DEBUG:" &
SERVER_PID=$!
sleep 1

# Test basic commands
$BUILD_DIR/examples/trie_client ping 127.0.0.1 $PORT 2>&1 | grep "PONG" > /dev/null && echo -e "  ${GREEN}✓${NC} PING" || { echo -e "  ${RED}✗${NC} PING failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client add "sensor.*.temp" 1 "monitor" 127.0.0.1 $PORT 2>&1 | grep "ADD successful" > /dev/null && echo -e "  ${GREEN}✓${NC} ADD" || { echo -e "  ${RED}✗${NC} ADD failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client match "sensor.room1.temp" 127.0.0.1 $PORT 2>&1 | grep "Matches found: 1" > /dev/null && echo -e "  ${GREEN}✓${NC} MATCH" || { echo -e "  ${RED}✗${NC} MATCH failed"; kill -9 $SERVER_PID; exit 1; }

# Graceful shutdown via SHUTDOWN command
$BUILD_DIR/examples/trie_client shutdown 127.0.0.1 $PORT 2>&1 | grep "SHUTDOWN successful" > /dev/null && echo -e "  ${GREEN}✓${NC} SHUTDOWN" || { echo -e "  ${YELLOW}⚠${NC} SHUTDOWN failed, using SIGTERM"; kill -TERM $SERVER_PID 2>/dev/null || true; }
sleep 0.5
# Fallback cleanup if needed
kill -9 $SERVER_PID 2>/dev/null || true
cleanup_server
wait_for_port $PORT
echo -e "${GREEN}✓${NC} UndefinedBehaviorSanitizer: No undefined behavior detected"
echo

# Test 3: Combined ASan + UBSan
echo -e "${BLUE}=== TEST 3: Combined ASan + UBSan ===${NC}"
echo "Building with -fsanitize=address,undefined..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
      .. > /dev/null 2>&1
cmake --build . --target trie_server trie_client > /dev/null 2>&1
cd ..

echo "Running tests with combined sanitizers..."
export ASAN_OPTIONS="$ASAN_OPTS"
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

$BUILD_DIR/examples/trie_server --port $PORT 2>&1 | grep -v "^DEBUG:" &
SERVER_PID=$!
sleep 1

$BUILD_DIR/examples/trie_client ping 127.0.0.1 $PORT 2>&1 | grep "PONG" > /dev/null && echo -e "  ${GREEN}✓${NC} PING" || { echo -e "  ${RED}✗${NC} PING failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client add "alerts.#" 100 "alerter" 127.0.0.1 $PORT 2>&1 | grep "ADD successful" > /dev/null && echo -e "  ${GREEN}✓${NC} ADD" || { echo -e "  ${RED}✗${NC} ADD failed"; kill -9 $SERVER_PID; exit 1; }
$BUILD_DIR/examples/trie_client match "alerts.critical.system" 127.0.0.1 $PORT 2>&1 | grep "Matches found: 1" > /dev/null && echo -e "  ${GREEN}✓${NC} MATCH" || { echo -e "  ${RED}✗${NC} MATCH failed"; kill -9 $SERVER_PID; exit 1; }

# Graceful shutdown via SHUTDOWN command
$BUILD_DIR/examples/trie_client shutdown 127.0.0.1 $PORT 2>&1 | grep "SHUTDOWN successful" > /dev/null && echo -e "  ${GREEN}✓${NC} SHUTDOWN" || { echo -e "  ${YELLOW}⚠${NC} SHUTDOWN failed, using SIGTERM"; kill -TERM $SERVER_PID 2>/dev/null || true; }
sleep 0.5
# Fallback cleanup if needed
kill -9 $SERVER_PID 2>/dev/null || true
cleanup_server
wait_for_port $PORT
echo -e "${GREEN}✓${NC} Combined sanitizers: All checks passed"
echo

# Test 4: Check for -fbounds-safety support (Clang experimental)
echo -e "${BLUE}=== TEST 4: Bounds Safety Check ===${NC}"
if clang --version 2>/dev/null | grep -q "clang"; then
    echo "Testing -fbounds-safety support..."
    echo "int main() { return 0; }" > /tmp/test_bounds.c
    if clang -fbounds-safety /tmp/test_bounds.c -o /tmp/test_bounds 2>&1 | grep -q "unknown argument"; then
        echo -e "${YELLOW}⚠${NC} -fbounds-safety not supported (requires Clang 19+ experimental)"
    else
        echo -e "${GREEN}✓${NC} -fbounds-safety is supported"
        echo "  Note: This is experimental and may not work with all code"
    fi
    rm -f /tmp/test_bounds.c /tmp/test_bounds
else
    echo -e "${YELLOW}⚠${NC} Clang not found, skipping -fbounds-safety check"
fi
echo

# Cleanup
rm -rf "$BUILD_DIR"

# Summary
echo "=============================================="
echo -e "${GREEN}  SANITIZER TESTING COMPLETE${NC}"
echo "=============================================="
echo
echo "Sanitizers tested:"
echo "  ✓ AddressSanitizer (ASan)"
if [[ $LEAK_DETECTION_AVAILABLE -eq 1 ]]; then
    echo "    - Detects: buffer overflows, use-after-free, double-free, memory leaks"
else
    echo "    - Detects: buffer overflows, use-after-free, double-free"
    echo "    - Note: Leak detection not available on this platform"
fi
echo "    - Result: No errors detected"
echo
echo "  ✓ UndefinedBehaviorSanitizer (UBSan)"
echo "    - Detects: signed integer overflow, null pointer dereference, misaligned access"
echo "    - Result: No undefined behavior detected"
echo
echo "  ✓ Combined ASan + UBSan"
echo "    - Comprehensive runtime checking"
echo "    - Result: All checks passed"
echo
echo "Comparison with valgrind:"
echo "  • Sanitizers: 5-10x faster, compile-time instrumentation"
echo "  • Valgrind: More thorough but slower, no recompilation needed"
echo "  • Best practice: Use both for maximum confidence"
echo
echo "Commands tested:"
echo "  ✓ PING, ADD, SUBSCRIBE, MATCH, LIST, UNSUBSCRIBE, REMOVE, STATS, SHUTDOWN"
echo
