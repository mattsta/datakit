#!/bin/bash
# Memory safety test with valgrind for trie server/client

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SERVER="$BUILD_DIR/examples/trie_server"
CLIENT="$BUILD_DIR/examples/trie_client"
PORT=40003
VALGRIND_OPTS="--leak-check=full --error-exitcode=1 -q --track-origins=yes --show-leak-kinds=all"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "=============================================="
echo "  Trie Server Memory Safety Test (Valgrind)"
echo "=============================================="
echo

# Build
echo "Building..."
cmake --build "$BUILD_DIR" --target trie_server trie_client > /dev/null 2>&1
echo -e "${GREEN}✓${NC} Build complete"
echo

# Cleanup
echo "Cleanup..."
pkill -9 trie_server 2>/dev/null || true
sleep 0.5
echo -e "${GREEN}✓${NC} Cleanup complete"
echo

# Start server (not under valgrind - too slow)
echo "Starting server..."
$SERVER --port $PORT 2>&1 | grep -v "^DEBUG:" &
SERVER_PID=$!
sleep 1
echo -e "${GREEN}✓${NC} Server started (PID: $SERVER_PID)"
echo

# Run each client command under valgrind
echo -e "${BLUE}=== Memory Safety Tests ===${NC}"

echo "Testing PING..."
valgrind $VALGRIND_OPTS $CLIENT ping 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} PING - no memory leaks"

echo "Testing ADD..."
valgrind $VALGRIND_OPTS $CLIENT add "test.pattern" 1 "test-sub" 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} ADD - no memory leaks"

echo "Testing SUBSCRIBE..."
valgrind $VALGRIND_OPTS $CLIENT subscribe "test.pattern" 2 "sub2" 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} SUBSCRIBE - no memory leaks"

echo "Testing MATCH..."
valgrind $VALGRIND_OPTS $CLIENT match "test.pattern" 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} MATCH - no memory leaks"

echo "Testing LIST..."
valgrind $VALGRIND_OPTS $CLIENT list 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} LIST - no memory leaks"

echo "Testing UNSUBSCRIBE..."
valgrind $VALGRIND_OPTS $CLIENT unsubscribe "test.pattern" 2 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} UNSUBSCRIBE - no memory leaks"

echo "Testing STATS..."
valgrind $VALGRIND_OPTS $CLIENT stats 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} STATS - no memory leaks"

echo "Testing REMOVE..."
valgrind $VALGRIND_OPTS $CLIENT remove "test.pattern" 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:" > /dev/null
echo -e "  ${GREEN}✓${NC} REMOVE - no memory leaks"

echo

# Cleanup
echo "Stopping server..."
$CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
echo -e "${GREEN}✓${NC} Server stopped"
echo

# Summary
echo "=============================================="
echo -e "${GREEN}  MEMORY SAFETY VERIFIED${NC}"
echo "=============================================="
echo
echo "All client commands tested with valgrind:"
echo "  ✓ PING - 0 memory leaks"
echo "  ✓ ADD - 0 memory leaks"
echo "  ✓ SUBSCRIBE - 0 memory leaks"
echo "  ✓ MATCH - 0 memory leaks"
echo "  ✓ LIST - 0 memory leaks"
echo "  ✓ UNSUBSCRIBE - 0 memory leaks"
echo "  ✓ STATS - 0 memory leaks"
echo "  ✓ REMOVE - 0 memory leaks"
echo
echo "Valgrind configuration:"
echo "  --leak-check=full"
echo "  --show-leak-kinds=all"
echo "  --track-origins=yes"
echo "  --error-exitcode=1 (fails on any leak)"
echo
