# Varint Library Example Suite

This directory contains comprehensive examples demonstrating all varint encodings and their combinations.

## Directory Structure

```
examples/
├── standalone/          # Individual module examples
│   ├── example_tagged.c
│   ├── example_external.c
│   ├── example_split.c
│   ├── example_chained.c
│   ├── example_packed.c
│   ├── example_dimension.c
│   ├── example_bitstream.c
│   └── rle_codec.c                    [NEW]
│
├── integration/         # Combination examples
│   ├── database_system.c
│   ├── network_protocol.c
│   ├── column_store.c
│   ├── game_engine.c
│   ├── sensor_network.c
│   ├── ml_features.c
│   ├── vector_clock.c                 [NEW]
│   ├── delta_compression.c            [NEW]
│   └── sparse_matrix_csr.c            [NEW]
│
├── reference/          # Complete reference implementations
│   ├── kv_store.c
│   ├── timeseries_db.c
│   └── graph_database.c
│
└── advanced/           # Production-quality real-world systems
    ├── blockchain_ledger.c
    ├── dns_server.c
    ├── game_replay_system.c
    ├── bytecode_vm.c
    ├── inverted_index.c
    ├── financial_orderbook.c
    ├── log_aggregation.c
    ├── geospatial_routing.c
    ├── bloom_filter.c                 [NEW]
    ├── autocomplete_trie.c            [NEW]
    ├── pointcloud_octree.c            [NEW]
    ├── trie_pattern_matcher.c
    └── trie_interactive.c
```

## Standalone Examples

Each standalone example demonstrates a single varint type with:

- Basic encode/decode operations
- Boundary value testing
- Common use patterns
- Error handling

| Example                        | Module          | Description                                               |
| ------------------------------ | --------------- | --------------------------------------------------------- |
| `example_tagged.c`             | varintTagged    | Sortable database keys                                    |
| `example_external.c`           | varintExternal  | Zero-overhead encoding                                    |
| `example_split.c`              | varintSplit     | Three-level encoding                                      |
| `example_chained.c`            | varintChained   | Legacy Protocol Buffers format                            |
| `example_packed.c`             | varintPacked    | Fixed-width bit-packed arrays                             |
| `example_dimension.c`          | varintDimension | Matrix storage                                            |
| `example_bitstream.c`          | varintBitstream | Bit-level operations                                      |
| `rle_codec.c`                  | varintExternal  | Run-length encoding (11x-2560x compression)               |
| `example_delta.c` **[NEW]**    | varintDelta     | Delta encoding with ZigZag (70-90% compression)           |
| `example_for.c` **[NEW]**      | varintFOR       | Frame-of-Reference (2-7.5x compression)                   |
| `example_group.c` **[NEW]**    | varintGroup     | Shared metadata encoding (30-40% savings)                 |
| `example_pfor.c` **[NEW]**     | varintPFOR      | Patched FOR with exceptions (57-83% compression)          |
| `example_dict.c` **[NEW]**     | varintDict      | Dictionary encoding (83-87% compression, 8x)              |
| `example_bitmap.c` **[NEW]**   | varintBitmap    | Hybrid dense/sparse (Roaring-style)                       |
| `example_adaptive.c` **[NEW]** | varintAdaptive  | Automatic encoding selection (1.35x-6.45x compression)    |
| `example_float.c` **[NEW]**    | varintFloat     | Variable-precision floating point (1.5x-4.0x compression) |

## Integration Examples

Real-world scenarios combining multiple varint types:

### database_system.c

**Combines**: varintTagged (keys) + varintExternal (values) + varintPacked (indexes)

- B-tree implementation with sortable varint keys
- Column store with external metadata
- Packed integer indexes

### network_protocol.c

**Combines**: varintBitstream (headers) + varintChained (Protocol Buffers compatibility)

- Custom protocol with bit-packed headers
- Protocol Buffers wire format encoding
- Message framing

### column_store.c

**Combines**: varintExternal (columns) + varintDimension (metadata)

- Columnar data storage
- Schema-driven encoding
- Efficient compression

### game_engine.c

**Combines**: varintPacked (coordinates) + varintBitstream (flags)

- Entity position storage (12-bit coordinates)
- Compact state flags
- Network synchronization

### sensor_network.c

**Combines**: varintExternal (timestamps) + varintPacked (readings)

- Time-series data storage
- Multi-sensor reading arrays
- Delta encoding

### ml_features.c

**Combines**: varintDimension (sparse matrices) + varintExternal (values)

- Sparse feature matrices
- Variable-width feature IDs
- Efficient storage for ML datasets

### vector_clock.c **[NEW]**

**Combines**: varintTagged (actor-counter pairs)

- Distributed event ordering and causal consistency
- Sparse clock representation (87.5% compression for 4 nodes)
- 923x compression for 1000-node systems
- Conflict detection for concurrent updates
- Applications: Dynamo, Cassandra, Riak, CouchDB

### delta_compression.c **[NEW]**

**Combines**: varintExternal (delta encoding) + ZigZag (signed deltas)

- Facebook Gorilla-style time series compression
- Delta-of-delta encoding (7.6-7.9x compression)
- 76-100% of deltas fit in 1 byte
- Applications: Prometheus, InfluxDB, monitoring systems

### sparse_matrix_csr.c **[NEW]**

**Combines**: varintExternal (indices) + varintDimension (metadata)

- Compressed Sparse Row format for scientific computing
- 77.67x compression (1000×1000 @ 1% density)
- 16.9% additional savings with varint vs fixed-width
- Matrix-vector multiply (SpMV), transpose, addition
- Applications: FEM, graph algorithms, ML, sparse linear algebra

## Reference Implementations

Complete, production-ready implementations:

### kv_store.c

**Full key-value store** with:

- varintTagged keys (sortable, memcmp-friendly)
- varintExternal values (space-efficient)
- varintPacked indexes (B-tree node arrays)
- Serialization/deserialization
- Persistence layer

### timeseries_db.c

**Time-series database** with:

- varintExternal timestamps (40-bit unix time)
- varintPacked sensor readings (14-bit values)
- Compression and downsampling
- Range queries

### graph_database.c

**Graph storage system** with:

- varintDimension adjacency matrices (bit matrices)
- varintTagged node IDs (sortable)
- varintExternal edge weights
- Graph traversal algorithms

## Advanced Examples

Production-quality real-world systems with comprehensive benchmarks. See [advanced/README.md](advanced/README.md) for full details.

**Highlights:**

- **blockchain_ledger.c** - Cryptocurrency transactions (10x compression)
- **dns_server.c** - DNS packet encoding (1M+ queries/sec)
- **game_replay_system.c** - Delta compression (100:1 ratio)
- **bytecode_vm.c** - VM instruction encoding (50-70% smaller)
- **inverted_index.c** - Search engine posting lists (20-30x compression)
- **financial_orderbook.c** - HFT order processing (sub-microsecond)
- **log_aggregation.c** - Log collection (100:1 compression)
- **geospatial_routing.c** - GPS coordinate compression (20-40x)
- **bloom_filter.c** **[NEW]** - Probabilistic set membership (2.5M+ ops/sec, 8x compression)
- **autocomplete_trie.c** **[NEW]** - Typeahead search (0.5-2 μs queries, 273x serialization ratio)
- **pointcloud_octree.c** **[NEW]** - 3D spatial data (1.61x compression, sub-ms queries)
- **trie_pattern_matcher.c** - AMQP routing (2391x faster, 0.7 bytes/pattern)
- **trie_interactive.c** - Interactive pattern matcher with CRUD and persistence

## Building Examples

```bash
# From repository root
mkdir -p build
cd build
cmake ..
make examples

# Run individual examples
./build/examples/example_tagged
./build/examples/database_system
./build/examples/kv_store
```

## Example Template

Each example follows this pattern:

```c
#include "../src/varint*.h"
#include <stdio.h>
#include <assert.h>

// 1. Data structure definition
typedef struct { ... } MyStructure;

// 2. Core operations
void myEncode(...) { ... }
void myDecode(...) { ... }

// 3. Usage demonstration
void demonstrate() { ... }

// 4. Testing with assertions
void test() { ... }

// 5. Main with output
int main() {
    demonstrate();
    test();
    printf("All tests passed!\n");
    return 0;
}
```

## Learning Path

### Beginners

1. Start with `example_tagged.c` - simplest self-describing format
2. Try `example_external.c` - understand external metadata
3. Explore `example_packed.c` - fixed-width arrays

### Intermediate

1. Study `database_system.c` - see how types combine
2. Examine `network_protocol.c` - bit-level efficiency
3. Review `column_store.c` - schema-driven design

### Advanced

1. Implement `kv_store.c` modifications
2. Extend `timeseries_db.c` with new features
3. Optimize `graph_database.c` for your use case

### Expert

1. Study production systems in `advanced/` directory
2. Start with `bytecode_vm.c` for fundamental patterns
3. Progress to `trie_pattern_matcher.c` for data structures
4. Explore `trie_interactive.c` for CRUD operations and persistence
5. Master `blockchain_ledger.c` or `financial_orderbook.c` for complete systems
6. See `advanced/README.md` for detailed learning path

## Testing

All examples include:

- ✅ Assertions for correctness
- ✅ Boundary value tests
- ✅ Round-trip encode/decode verification
- ✅ Memory leak checks (use valgrind)

Run with valgrind:

```bash
valgrind --leak-check=full ./build/examples/kv_store
```

## Performance Benchmarking

Compare varint types for your workload:

```c
// See src/varintCompare.c for comprehensive benchmarks
// Adapt for your specific use case
```

## Contributing Examples

To add a new example:

1. Choose appropriate directory (standalone/integration/reference)
2. Follow the example template
3. Include comprehensive comments
4. Add test cases with assertions
5. Update this README
6. Submit pull request

## Support

- **Documentation**: See `docs/` directory
- **API Reference**: See `docs/modules/*.md`
- **Decision Guide**: See `docs/CHOOSING_VARINTS.md`
- **Issues**: GitHub issues

## License

Examples are released under the same license as the library (Apache-2.0).
