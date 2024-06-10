#pragma once

/* Byte ordering detection */
/* Determines endianness at compile time */
static bool endianIsLittle(void) {
    /* Populate uint32_t with 1, then access
     * the first byte of four byte quantity, which
     * in little endian context is 1. */
    /* Compiler should know this at compile time
     * to select proper branches everywhere
     * we end up using _isLittleEndian() */
    const union {
        uint32_t i;
        uint8_t c[4];
    } one = {.i = 1};
    return one.c[0];
}
