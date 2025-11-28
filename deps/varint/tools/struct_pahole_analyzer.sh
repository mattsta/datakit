#!/bin/bash
# Comprehensive Struct Analysis using pahole (DWARF Debug Information)
# This script analyzes all structs in the varint library for:
#   - Memory layout and padding
#   - Cache line alignment (64-byte boundaries on most modern CPUs)
#   - Field locality of reference
#   - Optimization opportunities

set -e

# Colors for output
C_RED='\033[1;31m'
C_YELLOW='\033[1;33m'
C_GREEN='\033[1;32m'
C_BLUE='\033[1;34m'
C_CYAN='\033[1;36m'
C_MAGENTA='\033[1;35m'
C_BOLD='\033[1m'
C_RESET='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo -e "${C_BOLD}╔══════════════════════════════════════════════════════════════════════════════╗${C_RESET}"
echo -e "${C_BOLD}║           Varint Library Struct Analysis with pahole (DWARF)                ║${C_RESET}"
echo -e "${C_BOLD}║          Cache Line Awareness & Memory Layout Optimization                  ║${C_RESET}"
echo -e "${C_BOLD}╚══════════════════════════════════════════════════════════════════════════════╝${C_RESET}"
echo

# Check if pahole is installed
if ! command -v pahole &> /dev/null; then
    echo -e "${C_RED}Error: pahole not found. Install with:${C_RESET}"
    echo -e "${C_CYAN}  sudo apt-get install dwarves${C_RESET}"
    echo -e "${C_CYAN}  # or on Red Hat/CentOS/Fedora:${C_RESET}"
    echo -e "${C_CYAN}  sudo yum install dwarves${C_RESET}"
    exit 1
fi

echo -e "${C_BLUE}pahole version: $(pahole --version)${C_RESET}"
echo

# Compile test binary with debug symbols if needed
if [ ! -f "$SCRIPT_DIR/struct_audit_dwarf" ] || [ "$SCRIPT_DIR/struct_audit.c" -nt "$SCRIPT_DIR/struct_audit_dwarf" ]; then
    echo -e "${C_YELLOW}Compiling struct_audit with debug symbols (-g3 -O0)...${C_RESET}"
    gcc -g3 -O0 -I"$PROJECT_ROOT/src" -o "$SCRIPT_DIR/struct_audit_dwarf" "$SCRIPT_DIR/struct_audit.c" -lm
    echo -e "${C_GREEN}✓ Compilation successful${C_RESET}"
    echo
fi

# List of all metadata structs to analyze
STRUCTS=(
    "varintFORMeta"
    "varintPFORMeta"
    "varintFloatMeta"
    "varintAdaptiveDataStats"
    "varintAdaptiveMeta"
    "varintDictStats"
    "varintBitmapStats"
    "varintDict"
    "varintBitmap"
    "varintBitmapIterator"
)

echo -e "${C_BOLD}════════════════════════════════════════════════════════════════════════════════${C_RESET}"
echo -e "${C_BOLD}                        DETAILED STRUCT ANALYSIS${C_RESET}"
echo -e "${C_BOLD}════════════════════════════════════════════════════════════════════════════════${C_RESET}"
echo

CACHE_LINE_SIZE=64  # Modern x86_64 CPUs use 64-byte cache lines

for struct in "${STRUCTS[@]}"; do
    echo -e "${C_MAGENTA}┌──────────────────────────────────────────────────────────────────────────────┐${C_RESET}"
    echo -e "${C_MAGENTA}│ $struct${C_RESET}"
    echo -e "${C_MAGENTA}└──────────────────────────────────────────────────────────────────────────────┘${C_RESET}"

    # Run pahole and capture output
    pahole_output=$(pahole -C "$struct" "$SCRIPT_DIR/struct_audit_dwarf" 2>/dev/null || echo "NOT_FOUND")

    if [ "$pahole_output" = "NOT_FOUND" ]; then
        echo -e "${C_YELLOW}  ⚠ Struct not found in binary (may be in .c file or optimized out)${C_RESET}"
        echo
        continue
    fi

    # Display pahole output
    echo "$pahole_output"

    # Extract size and cacheline info
    size=$(echo "$pahole_output" | grep "size:" | sed 's/.*size: \([0-9]*\).*/\1/')
    cachelines=$(echo "$pahole_output" | grep "cachelines:" | sed 's/.*cachelines: \([0-9]*\).*/\1/')
    padding=$(echo "$pahole_output" | grep "padding:" | sed 's/.*padding: \([0-9]*\).*/\1/')

    # Cache line analysis
    echo
    echo -e "${C_BOLD}  Cache Line Analysis (${CACHE_LINE_SIZE}-byte cache lines):${C_RESET}"

    if [ -n "$cachelines" ]; then
        if [ "$cachelines" -eq 1 ]; then
            echo -e "    ${C_GREEN}✓ Fits entirely in 1 cache line (${size} bytes)${C_RESET}"
            echo -e "    ${C_GREEN}  → Excellent locality of reference${C_RESET}"
        else
            echo -e "    ${C_YELLOW}⚠ Spans ${cachelines} cache lines (${size} bytes)${C_RESET}"

            # Check if there are cache line boundary markers in the output
            if echo "$pahole_output" | grep -q "cacheline.*boundary"; then
                echo -e "    ${C_YELLOW}  → May cause cache misses when accessing fields across boundaries${C_RESET}"
                echo
                echo -e "${C_CYAN}    Fields crossing cache line boundaries:${C_RESET}"
                echo "$pahole_output" | grep -B1 "cacheline.*boundary" | grep -v "boundary" | sed 's/^/      /'
            fi
        fi
    fi

    # Padding analysis
    if [ -n "$padding" ] && [ "$padding" -gt 0 ]; then
        efficiency=$(echo "scale=1; ($size - $padding) * 100 / $size" | bc)
        echo
        echo -e "  ${C_BOLD}Padding Analysis:${C_RESET}"
        echo -e "    ${C_YELLOW}Wasted: ${padding} bytes (${efficiency}% efficient)${C_RESET}"

        if [ "$padding" -ge 8 ]; then
            echo -e "    ${C_RED}→ Consider reordering fields (8+ bytes wasted)${C_RESET}"
        elif [ "$padding" -ge 4 ]; then
            echo -e "    ${C_YELLOW}→ Could be optimized (4+ bytes wasted)${C_RESET}"
        fi
    else
        echo
        echo -e "  ${C_BOLD}Padding Analysis:${C_RESET}"
        echo -e "    ${C_GREEN}✓ No internal padding (100% efficient)${C_RESET}"
    fi

    echo
    echo
done

echo -e "${C_BOLD}════════════════════════════════════════════════════════════════════════════════${C_RESET}"
echo -e "${C_BOLD}                        SUMMARY & RECOMMENDATIONS${C_RESET}"
echo -e "${C_BOLD}════════════════════════════════════════════════════════════════════════════════${C_RESET}"
echo

echo -e "${C_CYAN}Cache Line Optimization Tips:${C_RESET}"
echo "  1. Keep frequently accessed structs under 64 bytes (1 cache line)"
echo "  2. Group hot fields (frequently accessed together) at the beginning"
echo "  3. Place rarely-used fields at the end"
echo "  4. Consider splitting large structs if different parts are accessed separately"
echo

echo -e "${C_CYAN}Padding Elimination Tips:${C_RESET}"
echo "  1. Order fields by size: 8-byte → 4-byte → 2-byte → 1-byte"
echo "  2. Place pointers and 64-bit types first (8-byte aligned)"
echo "  3. Group same-sized fields together"
echo "  4. Use _Static_assert to prevent regressions"
echo

echo -e "${C_CYAN}Additional pahole Commands:${C_RESET}"
echo -e "  ${C_GREEN}pahole --reorganize struct_audit_dwarf${C_RESET}"
echo "    → Show automatically reorganized layout suggestions"
echo
echo -e "  ${C_GREEN}pahole --show_reorg_steps struct_audit_dwarf${C_RESET}"
echo "    → Show step-by-step reorganization process"
echo
echo -e "  ${C_GREEN}pahole -E -C StructName struct_audit_dwarf${C_RESET}"
echo "    → Expand nested structs/unions"
echo
echo -e "  ${C_GREEN}pahole --sizes struct_audit_dwarf | grep varint${C_RESET}"
echo "    → List all varint structs with sizes"
echo

echo -e "${C_BOLD}Analysis complete!${C_RESET}"
