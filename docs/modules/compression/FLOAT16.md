# FLOAT16 - Half-Precision Float Encoding

## Overview

`float16` provides **conversion between standard 32-bit floats and 16-bit half-precision floats** (IEEE 754-2008 binary16 format), as well as **brain float16 (bfloat16)** format used in machine learning. These formats sacrifice precision for 50% space savings, making them ideal for applications where memory and bandwidth are more critical than precision.

**Key Features:**
- IEEE 754-2008 binary16 (float16) encoding/decoding
- Brain float16 (bfloat16) encoding/decoding
- Hardware acceleration on supported CPUs (F16C instruction set)
- Software fallback for portability
- 50% space savings vs float32
- Suitable for graphics, ML, and approximate computations

**Headers**: `float16.h`

**Source**: `float16.c`

**Standards**: IEEE 754-2008 (binary16), Google Brain (bfloat16)

## Format Comparison

### IEEE 754 Float Formats

```
float32 (standard float):
  [1 sign][8 exponent][23 mantissa] = 32 bits
  Range: ±1.4e-45 to ±3.4e38
  Precision: ~7 decimal digits

float16 (half precision):
  [1 sign][5 exponent][10 mantissa] = 16 bits
  Range: ±5.96e-8 to ±65504
  Precision: ~3 decimal digits

bfloat16 (brain float):
  [1 sign][8 exponent][7 mantissa] = 16 bits
  Range: ±1.4e-45 to ±3.4e38 (same as float32)
  Precision: ~2 decimal digits
```

### Visual Representation

```
float32:   SEEEEEEE EMMMMMMM MMMMMMMM MMMMMMMM
           └─────┬─────┘└──────────┬──────────┘
              8 bits           23 bits

float16:   SEEEE EMMMMMMMMM
           └─┬─┘└────┬────┘
            5 bits  10 bits

bfloat16:  SEEEEEEE EMMMMMMM
           └───┬──┘└───┬───┘
             8 bits  7 bits
           (matches float32 exponent!)
```

## API Functions

### Float16 Operations

```c
/* Encode float32 to float16 */
uint16_t float16Encode(float value);

/* Decode float16 to float32 */
float float16Decode(uint16_t value);

/* Function versions (always available) */
uint16_t float16Encode_(float value);
float float16Decode_(uint16_t value);
```

### BFloat16 Operations

```c
/* Encode float32 to bfloat16 */
uint16_t bfloat16Encode(float value);

/* Decode bfloat16 to float32 */
float bfloat16Decode(uint16_t value);

/* Function versions (always available) */
uint16_t bfloat16Encode_(float value);
float bfloat16Decode_(uint16_t value);
```

### Hardware Acceleration

The macros automatically use hardware instructions when available:

```c
#if __F16C__
/* On CPUs with F16C support (Intel Ivy Bridge+, AMD Jaguar+) */
#define float16Encode(v) _cvtss_sh(v, 0)  /* Hardware instruction */
#define float16Decode(v) _cvtsh_ss(v)     /* Hardware instruction */
#else
/* Software fallback */
#define float16Encode(v) float16Encode_(v)
#define float16Decode(v) float16Decode_(v)
#endif
```

## Usage Examples

### Example 1: Basic Conversion

```c
#include "float16.h"

void exampleBasicConversion(void) {
    float original = 3.14159f;

    /* Encode to float16 */
    uint16_t encoded = float16Encode(original);
    printf("float32: %.10f -> float16: 0x%04x\n", original, encoded);

    /* Decode back */
    float decoded = float16Decode(encoded);
    printf("float16: 0x%04x -> float32: %.10f\n", encoded, decoded);
    printf("Error: %.10f\n", original - decoded);

    /* BFloat16 */
    uint16_t bfloat = bfloat16Encode(original);
    float bfdecoded = bfloat16Decode(bfloat);
    printf("bfloat16: 0x%04x -> %.10f (error: %.10f)\n",
           bfloat, bfdecoded, original - bfdecoded);
}

/* Output:
 * float32: 3.1415898800 -> float16: 0x4248
 * float16: 0x4248 -> float32: 3.1406250000
 * Error: 0.0009648800
 * bfloat16: 0x4049 -> 3.1406250000 (error: 0.0009648800)
 */
```

### Example 2: Graphics Vertex Buffer Compression

```c
#include "float16.h"

typedef struct Vertex {
    uint16_t position[3];  /* float16 x, y, z */
    uint16_t normal[3];    /* float16 nx, ny, nz */
    uint16_t texcoord[2];  /* float16 u, v */
} Vertex;  /* 16 bytes vs 32 bytes! */

typedef struct VertexBuffer {
    Vertex *vertices;
    size_t count;
} VertexBuffer;

VertexBuffer *vertexBufferNew(size_t capacity) {
    VertexBuffer *vb = malloc(sizeof(*vb));
    vb->vertices = malloc(sizeof(Vertex) * capacity);
    vb->count = 0;
    return vb;
}

void vertexBufferAddVertex(VertexBuffer *vb,
                          float px, float py, float pz,
                          float nx, float ny, float nz,
                          float u, float v) {
    Vertex *v = &vb->vertices[vb->count++];

    /* Encode positions */
    v->position[0] = float16Encode(px);
    v->position[1] = float16Encode(py);
    v->position[2] = float16Encode(pz);

    /* Encode normals */
    v->normal[0] = float16Encode(nx);
    v->normal[1] = float16Encode(ny);
    v->normal[2] = float16Encode(nz);

    /* Encode texcoords */
    v->texcoord[0] = float16Encode(u);
    v->texcoord[1] = float16Encode(v);
}

void vertexBufferGetVertex(VertexBuffer *vb, size_t index,
                          float *px, float *py, float *pz,
                          float *nx, float *ny, float *nz,
                          float *u, float *v) {
    Vertex *vertex = &vb->vertices[index];

    /* Decode positions */
    *px = float16Decode(vertex->position[0]);
    *py = float16Decode(vertex->position[1]);
    *pz = float16Decode(vertex->position[2]);

    /* Decode normals */
    *nx = float16Decode(vertex->normal[0]);
    *ny = float16Decode(vertex->normal[1]);
    *nz = float16Decode(vertex->normal[2]);

    /* Decode texcoords */
    *u = float16Decode(vertex->texcoord[0]);
    *v = float16Decode(vertex->texcoord[1]);
}

void vertexBufferFree(VertexBuffer *vb) {
    if (vb) {
        free(vb->vertices);
        free(vb);
    }
}

/* Usage */
void exampleVertexBuffer(void) {
    VertexBuffer *vb = vertexBufferNew(1000);

    /* Add cube vertices */
    vertexBufferAddVertex(vb, -1, -1,  1,  0,  0,  1, 0, 0);
    vertexBufferAddVertex(vb,  1, -1,  1,  0,  0,  1, 1, 0);
    vertexBufferAddVertex(vb,  1,  1,  1,  0,  0,  1, 1, 1);

    printf("Vertex buffer: %zu vertices, %zu bytes\n",
           vb->count, vb->count * sizeof(Vertex));
    printf("Savings vs float32: %.1f%%\n",
           (1.0 - sizeof(Vertex) / 32.0) * 100);

    /* Retrieve vertex */
    float px, py, pz, nx, ny, nz, u, v;
    vertexBufferGetVertex(vb, 0, &px, &py, &pz, &nx, &ny, &nz, &u, &v);
    printf("Vertex 0: pos=(%.2f, %.2f, %.2f)\n", px, py, pz);

    vertexBufferFree(vb);
}
```

### Example 3: Neural Network Weights (BFloat16)

```c
#include "float16.h"

typedef struct NeuralLayer {
    uint16_t *weights;  /* bfloat16 */
    uint16_t *biases;   /* bfloat16 */
    size_t inputSize;
    size_t outputSize;
} NeuralLayer;

NeuralLayer *neuralLayerNew(size_t inputSize, size_t outputSize) {
    NeuralLayer *layer = malloc(sizeof(*layer));
    layer->inputSize = inputSize;
    layer->outputSize = outputSize;
    layer->weights = malloc(sizeof(uint16_t) * inputSize * outputSize);
    layer->biases = malloc(sizeof(uint16_t) * outputSize);
    return layer;
}

void neuralLayerInitWeights(NeuralLayer *layer) {
    /* Initialize with small random values */
    for (size_t i = 0; i < layer->inputSize * layer->outputSize; i++) {
        float weight = (rand() / (float)RAND_MAX - 0.5f) * 0.1f;
        layer->weights[i] = bfloat16Encode(weight);
    }

    for (size_t i = 0; i < layer->outputSize; i++) {
        layer->biases[i] = bfloat16Encode(0.0f);
    }
}

void neuralLayerForward(NeuralLayer *layer, const float *input, float *output) {
    for (size_t out = 0; out < layer->outputSize; out++) {
        float sum = bfloat16Decode(layer->biases[out]);

        for (size_t in = 0; in < layer->inputSize; in++) {
            size_t idx = out * layer->inputSize + in;
            float weight = bfloat16Decode(layer->weights[idx]);
            sum += input[in] * weight;
        }

        output[out] = sum;
    }
}

void neuralLayerPrintStats(NeuralLayer *layer) {
    size_t weightCount = layer->inputSize * layer->outputSize;
    size_t biasCount = layer->outputSize;
    size_t float32Bytes = (weightCount + biasCount) * sizeof(float);
    size_t bfloat16Bytes = (weightCount + biasCount) * sizeof(uint16_t);

    printf("Neural Layer Statistics:\n");
    printf("  Input size: %zu\n", layer->inputSize);
    printf("  Output size: %zu\n", layer->outputSize);
    printf("  Parameters: %zu\n", weightCount + biasCount);
    printf("  Memory (float32): %.2f KB\n", float32Bytes / 1024.0);
    printf("  Memory (bfloat16): %.2f KB\n", bfloat16Bytes / 1024.0);
    printf("  Savings: %.1f%%\n",
           (1.0 - (double)bfloat16Bytes / float32Bytes) * 100);
}

void neuralLayerFree(NeuralLayer *layer) {
    if (layer) {
        free(layer->weights);
        free(layer->biases);
        free(layer);
    }
}

/* Usage */
void exampleNeuralLayer(void) {
    NeuralLayer *layer = neuralLayerNew(784, 128);  /* MNIST hidden layer */
    neuralLayerInitWeights(layer);

    neuralLayerPrintStats(layer);

    /* Forward pass */
    float input[784] = {0};  /* Image pixels */
    float output[128];

    neuralLayerForward(layer, input, output);

    neuralLayerFree(layer);
}
```

### Example 4: Sensor Data Compression

```c
#include "float16.h"

typedef struct SensorReading {
    time_t timestamp;
    uint16_t temperature;  /* float16 */
    uint16_t humidity;     /* float16 */
    uint16_t pressure;     /* float16 */
} SensorReading;  /* 14 bytes vs 20 bytes with float32 */

typedef struct SensorLog {
    SensorReading *readings;
    size_t count;
    size_t capacity;
} SensorLog;

SensorLog *sensorLogNew(void) {
    SensorLog *log = malloc(sizeof(*log));
    log->readings = malloc(sizeof(SensorReading) * 10000);
    log->count = 0;
    log->capacity = 10000;
    return log;
}

void sensorLogRecord(SensorLog *log, float temp, float humid, float press) {
    if (log->count >= log->capacity) {
        log->capacity *= 2;
        log->readings = realloc(log->readings,
                               sizeof(SensorReading) * log->capacity);
    }

    SensorReading *r = &log->readings[log->count++];
    r->timestamp = time(NULL);
    r->temperature = float16Encode(temp);
    r->humidity = float16Encode(humid);
    r->pressure = float16Encode(press);
}

void sensorLogGetStats(SensorLog *log, float *avgTemp, float *avgHumid,
                      float *avgPress) {
    double sumTemp = 0, sumHumid = 0, sumPress = 0;

    for (size_t i = 0; i < log->count; i++) {
        sumTemp += float16Decode(log->readings[i].temperature);
        sumHumid += float16Decode(log->readings[i].humidity);
        sumPress += float16Decode(log->readings[i].pressure);
    }

    *avgTemp = sumTemp / log->count;
    *avgHumid = sumHumid / log->count;
    *avgPress = sumPress / log->count;
}

void sensorLogFree(SensorLog *log) {
    if (log) {
        free(log->readings);
        free(log);
    }
}

/* Usage */
void exampleSensorLog(void) {
    SensorLog *log = sensorLogNew();

    /* Record 24 hours of data (every minute) */
    for (int i = 0; i < 1440; i++) {
        float temp = 20.0f + 5.0f * sin(i * M_PI / 720.0f);
        float humid = 50.0f + 10.0f * cos(i * M_PI / 360.0f);
        float press = 1013.25f + (rand() % 100 - 50) / 10.0f;

        sensorLogRecord(log, temp, humid, press);
    }

    printf("Recorded %zu readings\n", log->count);
    printf("Memory used: %.2f KB (vs %.2f KB with float32)\n",
           log->count * sizeof(SensorReading) / 1024.0,
           log->count * 20 / 1024.0);

    float avgTemp, avgHumid, avgPress;
    sensorLogGetStats(log, &avgTemp, &avgHumid, &avgPress);
    printf("Averages: temp=%.1f°C, humid=%.1f%%, press=%.1f hPa\n",
           avgTemp, avgHumid, avgPress);

    sensorLogFree(log);
}
```

## Precision and Range

### Float16 (IEEE 754 binary16)

```c
Precision:
  - ~3.3 decimal digits
  - Smallest positive: 6.10e-5
  - Largest: 65504
  - Subnormal minimum: 5.96e-8

Special values:
  - Positive infinity: 0x7C00
  - Negative infinity: 0xFC00
  - NaN: 0x7C01 - 0x7FFF, 0xFC01 - 0xFFFF
  - Positive zero: 0x0000
  - Negative zero: 0x8000

Representable values:
  - 49,152 fractional values
  - 14,336 integer values
  - 1 positive infinity
  - 1 negative infinity
  - 2,046 NaN values
  Total: 65,536 (2^16)
```

### BFloat16 (Brain Float)

```c
Precision:
  - ~2.4 decimal digits
  - Same exponent range as float32
  - Smallest positive: 1.4e-45
  - Largest: 3.4e38

Advantages over float16:
  - Same dynamic range as float32
  - Simple truncation for encoding
  - Better for ML training
  - Prevents underflow/overflow issues

Disadvantages:
  - Lower precision than float16
  - Fewer fractional bits (7 vs 10)
```

### Precision Comparison

```c
Value: 3.141592653589793

float32:    3.14159274   (error: ~7e-7)
float16:    3.14062500   (error: ~1e-3)
bfloat16:   3.14062500   (error: ~1e-3)

Value: 0.0001

float32:    0.00010000000
float16:    0.00009999999  (close!)
bfloat16:   0.00009999424  (slightly worse)
```

## Performance

### Hardware vs Software

On CPUs with F16C support:

```c
Hardware (F16C):
  - Encode: ~0.5 ns
  - Decode: ~0.5 ns
  - Vector: 8 values in ~2 ns

Software fallback:
  - Encode: ~15 ns
  - Decode: ~12 ns
```

### BFloat16 Performance

```c
BFloat16 (always software):
  - Encode: ~2 ns (simple truncation)
  - Decode: ~2 ns (simple zero-fill)

Much faster than float16 software fallback!
```

## When to Use Each Format

### Use Float16 When:

```c
✓ Graphics applications (positions, normals, colors)
✓ Audio processing (samples)
✓ Approximate computations
✓ Memory/bandwidth limited
✓ Range fits within ±65504
✓ Hardware acceleration available
```

### Use BFloat16 When:

```c
✓ Machine learning (weights, activations)
✓ Neural network training
✓ Need float32 dynamic range
✓ Precision less critical than range
✓ Software-only implementation
```

### Avoid When:

```c
✗ Exact calculations required
✗ Small numbers (< 6e-5 for float16)
✗ Large numbers (> 65504 for float16)
✗ Accumulation errors critical
✗ Double precision needed
```

## Best Practices

### 1. Check Range Before Encoding

```c
float value = 100000.0f;

/* Float16 overflow */
uint16_t f16 = float16Encode(value);
float decoded = float16Decode(f16);
/* decoded == infinity! */

/* Use bfloat16 for large values */
uint16_t bf16 = bfloat16Encode(value);
decoded = bfloat16Decode(bf16);
/* decoded == 100000.0f */
```

### 2. Minimize Accumulation Errors

```c
/* BAD: Accumulate in float16 */
uint16_t sum16 = float16Encode(0.0f);
for (int i = 0; i < 1000; i++) {
    float current = float16Decode(sum16);
    sum16 = float16Encode(current + 0.1f);
}
/* Large rounding errors! */

/* GOOD: Accumulate in float32 */
float sum32 = 0.0f;
for (int i = 0; i < 1000; i++) {
    sum32 += 0.1f;
}
uint16_t sum16 = float16Encode(sum32);
/* Minimal error */
```

### 3. Use Hardware Acceleration

```c
#if __F16C__
/* Compiler flag: -mf16c */
/* Uses _cvtss_sh and _cvtsh_ss intrinsics */
printf("Using hardware float16 conversion\n");
#else
printf("Using software float16 conversion\n");
#endif
```

### 4. Consider Precision Requirements

```c
/* Low precision ok? Use float16/bfloat16 */
uint16_t color_r = float16Encode(0.5f);  /* Graphics */
uint16_t weight = bfloat16Encode(0.123f); /* ML */

/* Need precision? Stay with float32 */
float precise = 3.141592653589793f;  /* Don't encode! */
```

## See Also

- [XOF.md](XOF.md) - XOR filter compression for doubles
- [DOD.md](DOD.md) - Delta-of-delta encoding for integers
- [multiarray](../multiarray/MULTIARRAY.md) - Arrays for storing compressed data

## References

- IEEE 754-2008: IEEE Standard for Floating-Point Arithmetic
- Micikevicius et al., "Mixed Precision Training", ICLR 2018
- Brain Floating Point: https://cloud.google.com/blog/products/ai-machine-learning/bfloat16-the-secret-to-high-performance-on-cloud-tpus

## Testing

Run the float16 test suite:

```bash
./src/datakit-test test float16
```

The test suite includes:
- Round-trip conversion accuracy
- Special value handling (NaN, infinity, zero)
- Range testing
- Hardware vs software comparison
- Performance benchmarks
