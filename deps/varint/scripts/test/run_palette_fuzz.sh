#!/usr/bin/env bash
# Long-running varintPalette fuzz session under ASan+UBSan.
# Self-contained fuzzer (no libFuzzer). Every failure prints its
# reproduce command (iterations + seed), so reruns are exact.
#
# Usage: ./scripts/test/run_palette_fuzz.sh [iterations] [seed]
#   iterations default: 500000
#   seed default: current epoch seconds (printed for reproducibility)

set -euo pipefail

ITERATIONS="${1:-500000}"
SEED="${2:-$(date +%s)}"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${REPO_ROOT}/build/fuzz"
mkdir -p "${OUT_DIR}"
BIN="${OUT_DIR}/varintPaletteFuzzAsan"

echo "Building fuzzer with ASan+UBSan..."
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
    -Wall -Wextra -std=c11 \
    -o "${BIN}" \
    "${REPO_ROOT}/src/varintPaletteFuzz.c" \
    "${REPO_ROOT}/src/varintPalette.c" \
    "${REPO_ROOT}/src/varintCompete.c" \
    "${REPO_ROOT}/src/varintTelemetry.c" \
    "${REPO_ROOT}/src/varintDeltaDelta.c" \
    "${REPO_ROOT}/src/varintStride.c" \
    "${REPO_ROOT}/src/varintDelta.c" \
    "${REPO_ROOT}/src/varintRLE.c" \
    "${REPO_ROOT}/src/varintFOR.c" \
    "${REPO_ROOT}/src/varintPFOR.c" \
    "${REPO_ROOT}/src/varintDict.c" \
    "${REPO_ROOT}/src/varintBP128.c" \
    "${REPO_ROOT}/src/varintAdaptive.c" \
    "${REPO_ROOT}/src/varintElias.c" \
    "${REPO_ROOT}/src/varintGroup.c" \
    "${REPO_ROOT}/src/varintBitmap.c" \
    "${REPO_ROOT}/src/varintExternal.c" \
    "${REPO_ROOT}/src/varintTagged.c"

echo "Fuzzing: ${ITERATIONS} iterations, seed ${SEED}"
echo "Reproduce any failure with: varintPaletteFuzz <iterations> <seed>"
"${BIN}" "${ITERATIONS}" "${SEED}"
echo "Fuzz session clean."
