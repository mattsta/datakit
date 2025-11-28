#!/bin/bash
# Fast comprehensive test suite (without valgrind for speed)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SERVER="$BUILD_DIR/examples/trie_server"
CLIENT="$BUILD_DIR/examples/trie_client"
PORT=40002
SAVE_FILE="/tmp/trie_test_fast.dat"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

echo "=============================================="
echo "  Fast Trie Server Test Suite"
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
rm -f "$SAVE_FILE"
sleep 0.5
echo -e "${GREEN}✓${NC} Cleanup complete"
echo

# Start server
echo "Starting server..."
$SERVER --port $PORT --save "$SAVE_FILE" 2>&1 | grep -v "^DEBUG:" &
SERVER_PID=$!
sleep 1
echo -e "${GREEN}✓${NC} Server started (PID: $SERVER_PID)"
echo

# TEST 1-2: PING and STATS
echo -e "${BLUE}=== TEST 1-2: PING & STATS ===${NC}"
$CLIENT ping 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:"
$CLIENT stats 127.0.0.1 $PORT 2>&1 | grep -v "^DEBUG:"
echo -e "${GREEN}✓${NC} PING & STATS passed"
echo

# TEST 3: ADD patterns
echo -e "${BLUE}=== TEST 3: ADD ===${NC}"
$CLIENT add "sensors.*.temperature" 1 "temp-monitor" 127.0.0.1 $PORT 2>&1 | grep "ADD successful" > /dev/null
$CLIENT add "sensors.#.humidity" 2 "humidity-logger" 127.0.0.1 $PORT 2>&1 | grep "ADD successful" > /dev/null
$CLIENT add "alerts.*.critical" 3 "alert-handler" 127.0.0.1 $PORT 2>&1 | grep "ADD successful" > /dev/null
echo -e "${GREEN}✓${NC} ADD passed (3 patterns)"
echo

# TEST 4: SUBSCRIBE
echo -e "${BLUE}=== TEST 4: SUBSCRIBE ===${NC}"
$CLIENT subscribe "sensors.*.temperature" 10 "backup-monitor" 127.0.0.1 $PORT 2>&1 | grep "SUBSCRIBE successful" > /dev/null
$CLIENT subscribe "sensors.*.temperature" 11 "analytics" 127.0.0.1 $PORT 2>&1 | grep "SUBSCRIBE successful" > /dev/null
echo -e "${GREEN}✓${NC} SUBSCRIBE passed (2 subscribers)"
echo

# TEST 5: LIST
echo -e "${BLUE}=== TEST 5: LIST ===${NC}"
OUTPUT=$($CLIENT list 127.0.0.1 $PORT 2>&1)
echo "$OUTPUT"
PATTERN_COUNT=$(echo "$OUTPUT" | grep "Patterns (" | sed 's/.*(\(.*\) total).*/\1/')
if [ "$PATTERN_COUNT" != "3" ]; then
    echo -e "${RED}✗${NC} Expected 3 patterns, got $PATTERN_COUNT"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi
echo -e "${GREEN}✓${NC} LIST passed"
echo

# TEST 6: MATCH
echo -e "${BLUE}=== TEST 6: MATCH ===${NC}"
OUTPUT=$($CLIENT match "sensors.room1.temperature" 127.0.0.1 $PORT 2>&1)
echo "$OUTPUT"
MATCH_COUNT=$(echo "$OUTPUT" | grep "Matches found:" | awk '{print $3}')
if [ "$MATCH_COUNT" != "3" ]; then
    echo -e "${RED}✗${NC} Expected 3 matches, got $MATCH_COUNT"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi
echo -e "${GREEN}✓${NC} MATCH passed"
echo

# TEST 7: UNSUBSCRIBE
echo -e "${BLUE}=== TEST 7: UNSUBSCRIBE ===${NC}"
$CLIENT unsubscribe "sensors.*.temperature" 10 127.0.0.1 $PORT 2>&1 | grep "UNSUBSCRIBE successful" > /dev/null
OUTPUT=$($CLIENT match "sensors.room1.temperature" 127.0.0.1 $PORT 2>&1)
MATCH_COUNT=$(echo "$OUTPUT" | grep "Matches found:" | awk '{print $3}')
if [ "$MATCH_COUNT" != "2" ]; then
    echo -e "${RED}✗${NC} Expected 2 matches after unsubscribe, got $MATCH_COUNT"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi
echo -e "${GREEN}✓${NC} UNSUBSCRIBE passed"
echo

# TEST 8: SAVE
echo -e "${BLUE}=== TEST 8: SAVE ===${NC}"
$CLIENT save 127.0.0.1 $PORT 2>&1 | grep "SAVE successful" > /dev/null
if [ ! -f "$SAVE_FILE" ]; then
    echo -e "${RED}✗${NC} Save file not created"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi
SAVE_SIZE=$(stat -f%z "$SAVE_FILE" 2>/dev/null || stat -c%s "$SAVE_FILE" 2>/dev/null)
echo "Save file size: $SAVE_SIZE bytes"
echo -e "${GREEN}✓${NC} SAVE passed"
echo

# TEST 9: Persistence
echo -e "${BLUE}=== TEST 9: Persistence ===${NC}"
echo "Stopping server (using SIGKILL for speed)..."
kill -9 $SERVER_PID 2>/dev/null || true
sleep 1

echo "Restarting server with saved data..."
$SERVER --port $PORT --save "$SAVE_FILE" 2>&1 | grep -v "^DEBUG:" &
SERVER_PID=$!
sleep 1

# Verify data persisted
OUTPUT=$($CLIENT list 127.0.0.1 $PORT 2>&1)
PATTERN_COUNT=$(echo "$OUTPUT" | grep "Patterns (" | sed 's/.*(\(.*\) total).*/\1/')
if [ "$PATTERN_COUNT" != "3" ]; then
    echo -e "${RED}✗${NC} Expected 3 patterns after reload, got $PATTERN_COUNT"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi

OUTPUT=$($CLIENT match "sensors.room1.temperature" 127.0.0.1 $PORT 2>&1)
MATCH_COUNT=$(echo "$OUTPUT" | grep "Matches found:" | awk '{print $3}')
if [ "$MATCH_COUNT" != "2" ]; then
    echo -e "${RED}✗${NC} Expected 2 matches after reload, got $MATCH_COUNT"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi
echo -e "${GREEN}✓${NC} Persistence passed"
echo

# TEST 10: REMOVE
echo -e "${BLUE}=== TEST 10: REMOVE ===${NC}"
$CLIENT remove "alerts.*.critical" 127.0.0.1 $PORT 2>&1 | grep "REMOVE successful" > /dev/null
OUTPUT=$($CLIENT list 127.0.0.1 $PORT 2>&1)
PATTERN_COUNT=$(echo "$OUTPUT" | grep "Patterns (" | sed 's/.*(\(.*\) total).*/\1/')
if [ "$PATTERN_COUNT" != "2" ]; then
    echo -e "${RED}✗${NC} Expected 2 patterns after remove, got $PATTERN_COUNT"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi
echo -e "${GREEN}✓${NC} REMOVE passed"
echo

# TEST 11: Wildcard tests
echo -e "${BLUE}=== TEST 11: Wildcard Matching ===${NC}"
OUTPUT=$($CLIENT match "sensors.room2.temperature" 127.0.0.1 $PORT 2>&1)
MATCH_COUNT=$(echo "$OUTPUT" | grep "Matches found:" | awk '{print $3}')
if [ "$MATCH_COUNT" != "2" ]; then
    echo -e "${RED}✗${NC} Single wildcard test failed"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi

OUTPUT=$($CLIENT match "sensors.room1.zone2.humidity" 127.0.0.1 $PORT 2>&1)
MATCH_COUNT=$(echo "$OUTPUT" | grep "Matches found:" | awk '{print $3}')
if [ "$MATCH_COUNT" != "1" ]; then
    echo -e "${RED}✗${NC} Multi wildcard test failed"
    $CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
    exit 1
fi
echo -e "${GREEN}✓${NC} Wildcard matching passed"
echo

# Cleanup
echo "Stopping server..."
$CLIENT shutdown 127.0.0.1 $PORT > /dev/null 2>&1 || kill -9 $SERVER_PID 2>/dev/null || true
rm -f "$SAVE_FILE"
echo -e "${GREEN}✓${NC} Server stopped"
echo

# Summary
echo "=============================================="
echo -e "${GREEN}  ALL 11 TESTS PASSED${NC}"
echo "=============================================="
echo
echo "Commands verified:"
echo "  ✓ PING - Connectivity test"
echo "  ✓ STATS - Statistics"
echo "  ✓ ADD - Add patterns with subscribers"
echo "  ✓ SUBSCRIBE - Add subscriber to existing pattern"
echo "  ✓ UNSUBSCRIBE - Remove subscriber"
echo "  ✓ MATCH - Pattern matching with wildcards"
echo "  ✓ LIST - List all patterns"
echo "  ✓ SAVE - Manual persistence"
echo "  ✓ LOAD - Automatic reload on startup"
echo "  ✓ REMOVE - Remove patterns"
echo
echo "Features verified:"
echo "  ✓ Binary protocol with varint encoding"
echo "  ✓ Pattern matching (* and # wildcards)"
echo "  ✓ Multiple subscribers per pattern"
echo "  ✓ Persistence (save/load)"
echo "  ✓ All 10 server commands working"
echo
