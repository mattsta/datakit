#!/bin/bash
#
# Pub/Sub Demo Script
# Demonstrates the enhanced trie server's publish/subscribe capabilities
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SERVER_PORT=9999
CLIENT="./build/examples/trie_client"
SERVER_LOG="/tmp/trie_server_demo.log"

echo -e "${BLUE}=== Trie Server Pub/Sub Demo ===${NC}\n"

# Check if server is running
if ! pgrep -f "trie_server" > /dev/null; then
    echo -e "${YELLOW}Note: Please start the server in another terminal:${NC}"
    echo "  ./build/examples/trie_server"
    echo ""
    read -p "Press Enter when server is running..."
fi

sleep 1

# Test 1: Basic Ping
echo -e "\n${GREEN}Test 1: Server Health Check${NC}"
echo "Command: $CLIENT ping"
$CLIENT ping
echo ""

# Test 2: Add some patterns for matching
echo -e "${GREEN}Test 2: Add Pattern Subscribers${NC}"
echo "Adding pattern: sensors.*.temperature"
$CLIENT add "sensors.*.temperature" 100 "temp-collector" > /dev/null
echo "Adding pattern: sensors.*.humidity"
$CLIENT add "sensors.*.humidity" 101 "humidity-collector" > /dev/null
echo "Adding pattern: alerts.#"
$CLIENT add "alerts.#" 102 "alert-system" > /dev/null
echo -e "${BLUE}✓ Patterns added${NC}"
echo ""

# Test 3: List patterns
echo -e "${GREEN}Test 3: List All Patterns${NC}"
$CLIENT list
echo ""

# Test 4: Match patterns
echo -e "${GREEN}Test 4: Pattern Matching${NC}"
echo "Testing pattern match for 'sensors.room1.temperature':"
$CLIENT match "sensors.room1.temperature"
echo ""

echo "Testing pattern match for 'alerts.critical.fire':"
$CLIENT match "alerts.critical.fire"
echo ""

# Test 5: Publish messages (legacy - won't trigger live notifications)
echo -e "${GREEN}Test 5: Legacy Pattern Matching (No Pub/Sub)${NC}"
echo "Publishing to matched patterns (no subscribers listening):"
$CLIENT publish "sensors.room1.temperature" "25.5C" > /dev/null
echo -e "${BLUE}✓ Message published (but no live subscribers to receive it)${NC}"
echo ""

# Test 6: Live Subscribe and Publish Demo
echo -e "${GREEN}Test 6: Live Pub/Sub Demo${NC}"
echo -e "${YELLOW}This demo shows real-time notifications.${NC}"
echo "We'll start a subscriber in the background..."
echo ""

# Start a background subscriber
echo "Starting subscriber: sub-live 'sensors.*.temperature' QoS=0"
$CLIENT sub-live "sensors.*.temperature" 0 0 "demo-monitor" > /tmp/subscriber1.log 2>&1 &
SUB1_PID=$!
echo -e "${BLUE}✓ Subscriber started (PID: $SUB1_PID)${NC}"

sleep 2

# Publish some messages
echo ""
echo "Publishing temperature readings..."
for i in 1 2 3; do
    room="room$i"
    temp="2$i.${i}C"
    echo "  → sensors.$room.temperature = $temp"
    $CLIENT publish "sensors.$room.temperature" "$temp" > /dev/null
    sleep 0.5
done

echo ""
echo -e "${BLUE}Checking subscriber output:${NC}"
sleep 1
head -20 /tmp/subscriber1.log || echo "No output yet"

# Cleanup subscriber
kill $SUB1_PID 2>/dev/null || true

echo ""
echo -e "${GREEN}Test 7: Multiple Subscribers${NC}"
echo "Starting two subscribers on different patterns..."
echo ""

# Subscriber 1: Temperature
echo "Subscriber 1: sensors.*.temperature"
$CLIENT sub-live "sensors.*.temperature" 0 0 "temp-sub" > /tmp/sub_temp.log 2>&1 &
TEMP_PID=$!

# Subscriber 2: Humidity
echo "Subscriber 2: sensors.*.humidity"
$CLIENT sub-live "sensors.*.humidity" 0 0 "humidity-sub" > /tmp/sub_humidity.log 2>&1 &
HUM_PID=$!

sleep 2

# Publish to both
echo ""
echo "Publishing sensor data..."
$CLIENT publish "sensors.room1.temperature" "24.5C" > /dev/null
echo "  → Published: sensors.room1.temperature = 24.5C"
sleep 0.3

$CLIENT publish "sensors.room1.humidity" "65%" > /dev/null
echo "  → Published: sensors.room1.humidity = 65%"
sleep 0.3

$CLIENT publish "sensors.room2.temperature" "26.1C" > /dev/null
echo "  → Published: sensors.room2.temperature = 26.1C"
sleep 0.3

echo ""
echo -e "${BLUE}Temperature subscriber received:${NC}"
sleep 1
grep NOTIFICATION /tmp/sub_temp.log | head -5 || echo "  (no notifications yet)"

echo ""
echo -e "${BLUE}Humidity subscriber received:${NC}"
grep NOTIFICATION /tmp/sub_humidity.log | head -5 || echo "  (no notifications yet)"

# Cleanup
kill $TEMP_PID 2>/dev/null || true
kill $HUM_PID 2>/dev/null || true

echo ""
echo -e "${GREEN}Test 8: QoS Levels${NC}"
echo "Subscribing with QoS=1 (reliable delivery)..."
$CLIENT sub-live "alerts.critical.*" 1 0 "alert-handler" > /tmp/sub_qos1.log 2>&1 &
QOS_PID=$!

sleep 2

echo "Publishing critical alert..."
$CLIENT publish "alerts.critical.security" "Unauthorized access detected!" > /dev/null

sleep 1

echo ""
echo -e "${BLUE}QoS=1 subscriber output (with ACKs):${NC}"
head -15 /tmp/sub_qos1.log || echo "  (no output)"

kill $QOS_PID 2>/dev/null || true

echo ""
echo -e "${GREEN}Test 9: Server Statistics${NC}"
$CLIENT stats

echo ""
echo -e "${GREEN}Test 10: Subscription Management${NC}"
echo "Getting active subscriptions (this requires a persistent connection)..."
echo -e "${YELLOW}Note: This would show subscriptions if we had a persistent client.${NC}"

# Cleanup
echo ""
echo -e "${BLUE}=== Demo Complete ===${NC}"
echo ""
echo "Summary of features demonstrated:"
echo "  ✓ Basic server health (ping, stats)"
echo "  ✓ Pattern management (add, list, match)"
echo "  ✓ Message publishing"
echo "  ✓ Live subscriptions with real-time notifications"
echo "  ✓ Multiple concurrent subscribers"
echo "  ✓ Quality of Service (QoS 0 and QoS 1)"
echo "  ✓ Automatic message acknowledgment"
echo ""
echo "To explore further:"
echo "  - Start server: ./build/examples/trie_server"
echo "  - Subscribe: ./build/examples/trie_client sub-live 'pattern' 0 0 'name'"
echo "  - Publish: ./build/examples/trie_client publish 'pattern' 'message'"
echo ""

# Cleanup temp files
rm -f /tmp/subscriber1.log /tmp/sub_temp.log /tmp/sub_humidity.log /tmp/sub_qos1.log

echo -e "${GREEN}Done!${NC}"
