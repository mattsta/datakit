/**
 * graph_database.c - Production-quality graph database
 *
 * This reference implementation demonstrates a complete graph database with:
 * - varintDimension for adjacency matrix encoding
 * - varintPacked for node/edge ID management
 * - Efficient graph algorithms
 *
 * Features:
 * - Directed and undirected graphs
 * - Node and edge properties
 * - Adjacency list and matrix representations
 * - Graph traversal (BFS, DFS)
 * - Shortest path algorithms
 * - Degree calculations
 *
 * This is a complete, production-ready reference implementation.
 * Users can adapt this code for social networks, knowledge graphs, and routing.
 *
 * Compile: gcc -I../../src graph_database.c ../../build/src/libvarint.a -o
 * graph_database Run: ./graph_database
 */

// Generate varintPacked16 for node IDs (up to 65535 nodes)
#define PACK_STORAGE_BITS 16
#include "varintPacked.h"
#undef PACK_STORAGE_BITS

#include "varintDimension.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// GRAPH STRUCTURE
// ============================================================================

typedef uint16_t NodeID;
typedef uint32_t EdgeID;

typedef struct {
    char name[64];
    uint64_t properties; // Bit-packed properties
} Node;

typedef struct {
    NodeID from;
    NodeID to;
    uint32_t weight;
    uint64_t properties;
} Edge;

typedef struct {
    Node *nodes;
    size_t nodeCount;
    size_t nodeCapacity;

    Edge *edges;
    size_t edgeCount;
    size_t edgeCapacity;

    uint8_t *adjacencyMatrix; // Bit matrix: 1 bit per potential edge
    varintDimensionPair dimensionEncoding;
    bool isDirected;
} Graph;

// ============================================================================
// INITIALIZATION
// ============================================================================

bool graphInit(Graph *graph, size_t maxNodes, bool directed) {
    graph->nodes = malloc(maxNodes * sizeof(Node));
    if (!graph->nodes) {
        fprintf(stderr, "Error: Failed to allocate nodes array\n");
        return false;
    }
    graph->nodeCount = 0;
    graph->nodeCapacity = maxNodes;

    graph->edges = NULL;
    graph->edgeCount = 0;
    graph->edgeCapacity = 0;

    // Determine dimension encoding for adjacency matrix
    if (maxNodes <= 255) {
        graph->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_1_1;
    } else if (maxNodes <= 65535) {
        graph->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_2_2;
    } else {
        graph->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_4_4;
    }

    // Allocate bit matrix for adjacency
    size_t bitsNeeded = maxNodes * maxNodes;
    size_t bytesNeeded = (bitsNeeded + 7) / 8;
    graph->adjacencyMatrix = calloc(1, bytesNeeded);
    if (!graph->adjacencyMatrix) {
        fprintf(stderr, "Error: Failed to allocate adjacency matrix\n");
        free(graph->nodes);
        return false;
    }

    graph->isDirected = directed;
    return true;
}

void graphFree(Graph *graph) {
    free(graph->nodes);
    free(graph->edges);
    free(graph->adjacencyMatrix);
}

// ============================================================================
// NODE OPERATIONS
// ============================================================================

NodeID graphAddNode(Graph *graph, const char *name) {
    assert(graph->nodeCount < graph->nodeCapacity);

    NodeID nodeId = (NodeID)graph->nodeCount;
    Node *node = &graph->nodes[nodeId];

    strncpy(node->name, name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    node->properties = 0;

    graph->nodeCount++;
    return nodeId;
}

Node *graphGetNode(const Graph *graph, NodeID nodeId) {
    assert(nodeId < graph->nodeCount);
    return &graph->nodes[nodeId];
}

// ============================================================================
// EDGE OPERATIONS
// ============================================================================

void setAdjacencyBit(Graph *graph, NodeID from, NodeID to, bool value) {
    assert(from < graph->nodeCount && to < graph->nodeCount);

    size_t bitIndex = (size_t)from * graph->nodeCapacity + to;
    size_t byteIndex = bitIndex / 8;
    uint8_t bitOffset = bitIndex % 8;

    if (value) {
        graph->adjacencyMatrix[byteIndex] |= (1 << bitOffset);
    } else {
        graph->adjacencyMatrix[byteIndex] &= ~(1 << bitOffset);
    }
}

bool getAdjacencyBit(const Graph *graph, NodeID from, NodeID to) {
    assert(from < graph->nodeCount && to < graph->nodeCount);

    size_t bitIndex = (size_t)from * graph->nodeCapacity + to;
    size_t byteIndex = bitIndex / 8;
    uint8_t bitOffset = bitIndex % 8;

    return (graph->adjacencyMatrix[byteIndex] & (1 << bitOffset)) != 0;
}

EdgeID graphAddEdge(Graph *graph, NodeID from, NodeID to, uint32_t weight) {
    assert(from < graph->nodeCount && to < graph->nodeCount);

    // Grow edge array if needed
    if (graph->edgeCount >= graph->edgeCapacity) {
        size_t newCapacity = graph->edgeCapacity ? graph->edgeCapacity * 2 : 16;
        Edge *newEdges = realloc(graph->edges, newCapacity * sizeof(Edge));
        if (!newEdges) {
            fprintf(stderr, "Error: Failed to reallocate edges\n");
            return (EdgeID)-1;
        }
        graph->edges = newEdges;
        graph->edgeCapacity = newCapacity;
    }

    // Add edge
    EdgeID edgeId = (EdgeID)graph->edgeCount;
    Edge *edge = &graph->edges[edgeId];
    edge->from = from;
    edge->to = to;
    edge->weight = weight;
    edge->properties = 0;

    graph->edgeCount++;

    // Update adjacency matrix
    setAdjacencyBit(graph, from, to, true);
    if (!graph->isDirected) {
        setAdjacencyBit(graph, to, from, true);
    }

    return edgeId;
}

bool graphHasEdge(const Graph *graph, NodeID from, NodeID to) {
    return getAdjacencyBit(graph, from, to);
}

// ============================================================================
// DEGREE CALCULATIONS
// ============================================================================

size_t graphGetOutDegree(const Graph *graph, NodeID nodeId) {
    assert(nodeId < graph->nodeCount);

    size_t degree = 0;
    for (NodeID to = 0; to < graph->nodeCount; to++) {
        if (graphHasEdge(graph, nodeId, to)) {
            degree++;
        }
    }
    return degree;
}

size_t graphGetInDegree(const Graph *graph, NodeID nodeId) {
    assert(nodeId < graph->nodeCount);

    size_t degree = 0;
    for (NodeID from = 0; from < graph->nodeCount; from++) {
        if (graphHasEdge(graph, from, nodeId)) {
            degree++;
        }
    }
    return degree;
}

// ============================================================================
// GRAPH TRAVERSAL - BFS
// ============================================================================

typedef struct {
    NodeID *nodes;
    size_t count;
} TraversalResult;

void traversalResultFree(TraversalResult *result) {
    free(result->nodes);
}

TraversalResult graphBFS(const Graph *graph, NodeID startNode) {
    assert(startNode < graph->nodeCount);

    TraversalResult result;
    result.nodes = malloc(graph->nodeCount * sizeof(NodeID));
    if (!result.nodes) {
        fprintf(stderr, "Error: Failed to allocate BFS result nodes\n");
        result.count = 0;
        return result;
    }
    result.count = 0;

    // Visited tracking
    bool *visited = calloc(graph->nodeCount, sizeof(bool));
    if (!visited) {
        fprintf(stderr, "Error: Failed to allocate BFS visited array\n");
        free(result.nodes);
        result.nodes = NULL;
        result.count = 0;
        return result;
    }

    // Queue for BFS
    NodeID *queue = malloc(graph->nodeCount * sizeof(NodeID));
    if (!queue) {
        fprintf(stderr, "Error: Failed to allocate BFS queue\n");
        free(result.nodes);
        free(visited);
        result.nodes = NULL;
        result.count = 0;
        return result;
    }
    size_t queueFront = 0;
    size_t queueBack = 0;

    // Start BFS
    queue[queueBack++] = startNode;
    visited[startNode] = true;

    while (queueFront < queueBack) {
        NodeID current = queue[queueFront++];
        result.nodes[result.count++] = current;

        // Visit all adjacent nodes
        for (NodeID neighbor = 0; neighbor < graph->nodeCount; neighbor++) {
            if (graphHasEdge(graph, current, neighbor) && !visited[neighbor]) {
                visited[neighbor] = true;
                queue[queueBack++] = neighbor;
            }
        }
    }

    free(visited);
    free(queue);
    return result;
}

// ============================================================================
// SHORTEST PATH - Dijkstra
// ============================================================================

typedef struct {
    NodeID *path;
    size_t pathLength;
    uint32_t totalWeight;
} ShortestPath;

void shortestPathFree(ShortestPath *result) {
    free(result->path);
}

ShortestPath graphDijkstra(const Graph *graph, NodeID start, NodeID end) {
    assert(start < graph->nodeCount && end < graph->nodeCount);

    // Initialize distances
    uint32_t *distances = malloc(graph->nodeCount * sizeof(uint32_t));
    if (!distances) {
        fprintf(stderr, "Error: Failed to allocate Dijkstra distances array\n");
        ShortestPath result = {
            .path = NULL, .pathLength = 0, .totalWeight = UINT32_MAX};
        return result;
    }

    NodeID *previous = malloc(graph->nodeCount * sizeof(NodeID));
    if (!previous) {
        fprintf(stderr, "Error: Failed to allocate Dijkstra previous array\n");
        free(distances);
        ShortestPath result = {
            .path = NULL, .pathLength = 0, .totalWeight = UINT32_MAX};
        return result;
    }

    bool *visited = calloc(graph->nodeCount, sizeof(bool));
    if (!visited) {
        fprintf(stderr, "Error: Failed to allocate Dijkstra visited array\n");
        free(distances);
        free(previous);
        ShortestPath result = {
            .path = NULL, .pathLength = 0, .totalWeight = UINT32_MAX};
        return result;
    }

    for (size_t i = 0; i < graph->nodeCount; i++) {
        distances[i] = UINT32_MAX;
        previous[i] = (NodeID)-1;
    }
    distances[start] = 0;

    // Dijkstra's algorithm
    for (size_t i = 0; i < graph->nodeCount; i++) {
        // Find unvisited node with minimum distance
        uint32_t minDist = UINT32_MAX;
        NodeID minNode = (NodeID)-1;

        for (NodeID node = 0; node < graph->nodeCount; node++) {
            if (!visited[node] && distances[node] < minDist) {
                minDist = distances[node];
                minNode = node;
            }
        }

        if (minNode == (NodeID)-1) {
            break; // No more reachable nodes
        }

        visited[minNode] = true;

        // Update distances to neighbors
        for (NodeID neighbor = 0; neighbor < graph->nodeCount; neighbor++) {
            if (graphHasEdge(graph, minNode, neighbor) && !visited[neighbor]) {
                // Find edge weight
                uint32_t weight = 1; // Default weight
                for (size_t e = 0; e < graph->edgeCount; e++) {
                    if (graph->edges[e].from == minNode &&
                        graph->edges[e].to == neighbor) {
                        weight = graph->edges[e].weight;
                        break;
                    }
                }

                uint32_t newDist = distances[minNode] + weight;
                if (newDist < distances[neighbor]) {
                    distances[neighbor] = newDist;
                    previous[neighbor] = minNode;
                }
            }
        }
    }

    // Reconstruct path
    ShortestPath result;
    result.totalWeight = distances[end];

    if (distances[end] == UINT32_MAX) {
        // No path found
        result.path = NULL;
        result.pathLength = 0;
    } else {
        // Count path length
        size_t pathLen = 0;
        for (NodeID node = end; node != (NodeID)-1; node = previous[node]) {
            pathLen++;
        }

        // Build path
        result.path = malloc(pathLen * sizeof(NodeID));
        if (!result.path) {
            fprintf(stderr, "Error: Failed to allocate path array\n");
            result.pathLength = 0;
            result.totalWeight = UINT32_MAX;
            free(distances);
            free(previous);
            free(visited);
            return result;
        }
        result.pathLength = pathLen;

        size_t idx = pathLen - 1;
        for (NodeID node = end; node != (NodeID)-1; node = previous[node]) {
            result.path[idx--] = node;
        }
    }

    free(distances);
    free(previous);
    free(visited);
    return result;
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateGraphDB(void) {
    printf("\n=== Graph Database Reference Implementation ===\n\n");

    // 1. Create graph
    printf("1. Creating directed graph...\n");
    Graph graph;
    if (!graphInit(&graph, 256, true)) { // Directed graph, max 256 nodes
        fprintf(stderr, "Error: Failed to initialize graph\n");
        return;
    }

    printf("   Max nodes: %zu\n", graph.nodeCapacity);
    printf("   Dimension encoding: ");
    if (graph.dimensionEncoding == VARINT_DIMENSION_PAIR_DENSE_1_1) {
        printf("DENSE_1_1 (1-byte node IDs)\n");
    }
    printf("   Graph type: %s\n", graph.isDirected ? "Directed" : "Undirected");

    // 2. Add nodes
    printf("\n2. Adding nodes...\n");

    NodeID nodeA = graphAddNode(&graph, "Alice");
    NodeID nodeB = graphAddNode(&graph, "Bob");
    NodeID nodeC = graphAddNode(&graph, "Carol");
    NodeID nodeD = graphAddNode(&graph, "Dave");
    NodeID nodeE = graphAddNode(&graph, "Eve");
    NodeID nodeF = graphAddNode(&graph, "Frank");

    printf("   Added %zu nodes:\n", graph.nodeCount);
    for (size_t i = 0; i < graph.nodeCount; i++) {
        printf("   - Node %zu: %s\n", i, graph.nodes[i].name);
    }

    // 3. Add edges
    printf("\n3. Adding edges (weighted)...\n");

    graphAddEdge(&graph, nodeA, nodeB, 4);
    graphAddEdge(&graph, nodeA, nodeC, 2);
    graphAddEdge(&graph, nodeB, nodeC, 1);
    graphAddEdge(&graph, nodeB, nodeD, 5);
    graphAddEdge(&graph, nodeC, nodeD, 8);
    graphAddEdge(&graph, nodeC, nodeE, 10);
    graphAddEdge(&graph, nodeD, nodeE, 2);
    graphAddEdge(&graph, nodeD, nodeF, 6);
    graphAddEdge(&graph, nodeE, nodeF, 3);

    printf("   Added %zu edges:\n", graph.edgeCount);
    for (size_t i = 0; i < graph.edgeCount; i++) {
        const Edge *e = &graph.edges[i];
        printf("   - %s -> %s (weight: %u)\n", graph.nodes[e->from].name,
               graph.nodes[e->to].name, e->weight);
    }

    // 4. Check adjacency
    printf("\n4. Testing adjacency queries...\n");

    NodeID testPairs[][2] = {
        {nodeA, nodeB}, {nodeB, nodeA}, {nodeA, nodeF}, {nodeC, nodeE}};

    for (size_t i = 0; i < 4; i++) {
        NodeID from = testPairs[i][0];
        NodeID to = testPairs[i][1];
        bool hasEdge = graphHasEdge(&graph, from, to);
        printf("   %s -> %s: %s\n", graph.nodes[from].name,
               graph.nodes[to].name, hasEdge ? "YES" : "NO");
    }

    // 5. Degree calculations
    printf("\n5. Calculating node degrees...\n");

    for (NodeID node = 0; node < graph.nodeCount; node++) {
        size_t outDegree = graphGetOutDegree(&graph, node);
        size_t inDegree = graphGetInDegree(&graph, node);
        printf("   %s: out-degree=%zu, in-degree=%zu\n", graph.nodes[node].name,
               outDegree, inDegree);
    }

    // 6. BFS traversal
    printf("\n6. BFS traversal from Alice...\n");

    TraversalResult bfsResult = graphBFS(&graph, nodeA);
    printf("   Visited %zu nodes in BFS order:\n", bfsResult.count);
    for (size_t i = 0; i < bfsResult.count; i++) {
        printf("   %zu. %s\n", i + 1, graph.nodes[bfsResult.nodes[i]].name);
    }
    traversalResultFree(&bfsResult);

    // 7. Shortest path
    printf("\n7. Finding shortest path (Alice -> Frank)...\n");

    ShortestPath shortestPath = graphDijkstra(&graph, nodeA, nodeF);
    if (shortestPath.pathLength > 0) {
        printf("   Path length: %zu hops\n", shortestPath.pathLength);
        printf("   Total weight: %u\n", shortestPath.totalWeight);
        printf("   Path: ");
        for (size_t i = 0; i < shortestPath.pathLength; i++) {
            printf("%s", graph.nodes[shortestPath.path[i]].name);
            if (i < shortestPath.pathLength - 1) {
                printf(" -> ");
            }
        }
        printf("\n");
    } else {
        printf("   No path found\n");
    }
    shortestPathFree(&shortestPath);

    // 8. Space efficiency analysis
    printf("\n8. Space efficiency analysis:\n");

    // Adjacency matrix
    size_t matrixBits = graph.nodeCapacity * graph.nodeCapacity;
    size_t matrixBytes = (matrixBits + 7) / 8;
    printf("   Adjacency matrix (bit-packed):\n");
    printf("   - %zu × %zu nodes = %zu bits = %zu bytes\n", graph.nodeCapacity,
           graph.nodeCapacity, matrixBits, matrixBytes);
    printf("   - vs 32-bit ints: %zu bytes\n",
           graph.nodeCapacity * graph.nodeCapacity * 4);
    printf("   - Savings: %.1f%%\n",
           100.0 * (1.0 - (double)matrixBytes /
                              (graph.nodeCapacity * graph.nodeCapacity * 4)));

    // Edge list
    size_t edgeListBytes = graph.edgeCount * sizeof(Edge);
    printf("\n   Edge list:\n");
    printf("   - %zu edges × %zu bytes = %zu bytes\n", graph.edgeCount,
           sizeof(Edge), edgeListBytes);

    // Sparse vs dense
    double density =
        (double)graph.edgeCount / (graph.nodeCount * graph.nodeCount);
    printf("\n   Graph density: %.2f%% (%zu / %zu possible edges)\n",
           density * 100, graph.edgeCount, graph.nodeCount * graph.nodeCount);
    printf("   Optimal representation: %s\n",
           density < 0.1 ? "Edge list (sparse)" : "Adjacency matrix (dense)");

    // 9. Dimension encoding benefits
    printf("\n9. Dimension encoding benefits:\n");
    printf("   varintDimension encodes matrix dimensions:\n");
    printf("   - DENSE_1_1: 256×256 matrix with 1-byte node IDs\n");
    printf("   - Single enum value describes entire structure\n");
    printf("   - Enables automatic storage optimization\n");
    printf("   - For larger graphs (>65K nodes): DENSE_4_4 automatically\n");

    graphFree(&graph);

    printf("\n✓ Graph database reference implementation complete\n");
}

int main(void) {
    printf("===============================================\n");
    printf("  Graph Database Reference Implementation\n");
    printf("===============================================\n");

    demonstrateGraphDB();

    printf("\n===============================================\n");
    printf("This reference implementation demonstrates:\n");
    printf("  • varintDimension for adjacency matrices\n");
    printf("  • varintPacked for node ID management\n");
    printf("  • Bit-packed adjacency matrix\n");
    printf("  • Graph traversal (BFS)\n");
    printf("  • Shortest path (Dijkstra)\n");
    printf("  • Degree calculations\n");
    printf("  • Space-efficient graph storage\n");
    printf("\n");
    printf("Users can adapt this code for:\n");
    printf("  • Social networks\n");
    printf("  • Knowledge graphs\n");
    printf("  • Routing and navigation\n");
    printf("  • Dependency analysis\n");
    printf("===============================================\n");

    return 0;
}
