#pragma once

/* multiarraySmall is a 16 byte struct.
 * 'data' is an array of elements.
 * 'len' is the width of each element in the array 'data'.
 * 'count' is the number of elements in 'data'
 * 'rowMax' is the number of elements to store before 'multiarray' grows
 *          this multiarraySmall into a multiarrayMedium. */
struct multiarraySmall {
    uint8_t *data;
    uint16_t len;    /* width of each entry in 'data' */
    uint16_t count;  /* number of entries (i.e. size of data is len * count) */
    uint16_t rowMax; /* not used here, but used for upgrading. */
    /* 2 bytes padding */
};
