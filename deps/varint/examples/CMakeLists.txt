# Examples CMakeLists.txt

cmake_minimum_required(VERSION 3.10...3.28)

# Include parent source directory
include_directories(../src)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -std=c11 -O2")

# Link against varint library
link_directories(${CMAKE_BINARY_DIR}/src)

# Standalone examples
add_executable(example_tagged standalone/example_tagged.c)
target_link_libraries(example_tagged varint)

add_executable(example_external standalone/example_external.c)
target_link_libraries(example_external varint)

add_executable(example_split standalone/example_split.c)
target_link_libraries(example_split varint)

add_executable(example_chained standalone/example_chained.c)
target_link_libraries(example_chained varint)

add_executable(example_packed standalone/example_packed.c)
target_link_libraries(example_packed varint)

add_executable(example_dimension standalone/example_dimension.c)
target_link_libraries(example_dimension varintDimension)

add_executable(example_bitstream standalone/example_bitstream.c)
target_link_libraries(example_bitstream varint)

# Integration examples
add_executable(database_system integration/database_system.c)
target_link_libraries(database_system varint)

add_executable(network_protocol integration/network_protocol.c)
target_link_libraries(network_protocol varint)

add_executable(column_store integration/column_store.c)
target_link_libraries(column_store varint)

add_executable(game_engine integration/game_engine.c)
target_link_libraries(game_engine varint)

add_executable(sensor_network integration/sensor_network.c)
target_link_libraries(sensor_network varint)

add_executable(ml_features integration/ml_features.c)
target_link_libraries(ml_features varint)

# Reference implementations
add_executable(kv_store reference/kv_store.c)
target_link_libraries(kv_store varint)

add_executable(timeseries_db reference/timeseries_db.c)
target_link_libraries(timeseries_db varint)

add_executable(graph_database reference/graph_database.c)
target_link_libraries(graph_database varint)

# Advanced examples (high-performance real-world systems)
add_executable(blockchain_ledger advanced/blockchain_ledger.c)
target_link_libraries(blockchain_ledger varint m)

add_executable(dns_server advanced/dns_server.c)
target_link_libraries(dns_server varint)

add_executable(game_replay_system advanced/game_replay_system.c)
target_link_libraries(game_replay_system varint m)

add_executable(bytecode_vm advanced/bytecode_vm.c)
target_link_libraries(bytecode_vm varint)

add_executable(inverted_index advanced/inverted_index.c)
target_link_libraries(inverted_index varint m)

add_executable(financial_orderbook advanced/financial_orderbook.c)
target_link_libraries(financial_orderbook varint m)

add_executable(log_aggregation advanced/log_aggregation.c)
target_link_libraries(log_aggregation varint)

add_executable(geospatial_routing advanced/geospatial_routing.c)
target_link_libraries(geospatial_routing varint m)

add_executable(trie_pattern_matcher advanced/trie_pattern_matcher.c)
target_link_libraries(trie_pattern_matcher varint)

add_executable(trie_interactive advanced/trie_interactive.c)
target_link_libraries(trie_interactive varint)

add_executable(trie_server advanced/trie_server.c)
target_link_libraries(trie_server varint)

add_executable(trie_client advanced/trie_client.c)
target_link_libraries(trie_client varint)

add_executable(autocomplete_trie advanced/autocomplete_trie.c)
target_link_libraries(autocomplete_trie varint m)

add_executable(pointcloud_octree advanced/pointcloud_octree.c)
target_link_libraries(pointcloud_octree varint m)

# Custom target to build all examples
add_custom_target(examples DEPENDS
    example_tagged
    example_external
    example_split
    example_chained
    example_packed
    example_dimension
    example_bitstream
    database_system
    network_protocol
    column_store
    game_engine
    sensor_network
    ml_features
    kv_store
    timeseries_db
    graph_database
    blockchain_ledger
    dns_server
    game_replay_system
    bytecode_vm
    inverted_index
    financial_orderbook
    log_aggregation
    geospatial_routing
    trie_pattern_matcher
    trie_interactive
    trie_server
    trie_client
    autocomplete_trie
    pointcloud_octree
)

message(STATUS "Examples configured. Build with 'make examples'")
