#pragma once

/* multiarrayLargeNode is a 16 byte struct.
 * 'data' is an array of entrires each with width multiarrayLarge.len
 * 'prevNext' is an xor doubly linked list reference.
 * 'count' is the length of the 'data' array.
 *         (allocation of 'data' is exactly 'count' * multiarrayLarge.len) */
typedef struct multiarrayLargeNode {
    uint8_t *data;
    uint64_t prevNext : 48; /* xor of addrs is likely much less than 48 bits */
    uint64_t count : 16;    /* we should only have max 32k entries per node. */
} multiarrayLargeNode;

/* multiarrayLarge is a 24 byte struct.
 * 'head' is a direct pointer to the head of the doubly linked xor node list.
 * 'tail' is a direct pointer to the tail of the doubly linked xor node list.
 * 'len' is the width of every data entry inside every node.
 * 'rowMax' is the maximum number of entires in each node before we create
 *          a new node. */
struct multiarrayLarge {
    multiarrayLargeNode *head;
    multiarrayLargeNode *tail;
    uint16_t len;    /* width of individual node->data entires */
    uint16_t rowMax; /* maximum entries per node before creating new node */
    /* 4 bytes padding */
};
