# list - Generic Doubly-Linked List

## Overview

`list` is a **generic doubly-linked list implementation** providing O(1) insertion/deletion and bidirectional iteration. This is a legacy implementation from Redis, used only for some tests and not by the main data structures.

**Key Features:**

- Generic value storage via void pointers
- O(1) head/tail insertion and deletion
- Bidirectional iteration
- Optional custom memory management callbacks
- Optional custom comparison callbacks
- List rotation and searching
- List duplication

**Header**: `list.h`

**Source**: `list.c`

**Origin**: Redis adlist (Salvatore Sanfilippo)

**Note**: This is a legacy test utility. Production data structures in datakit use more specialized implementations.

## Data Structures

### List Node

```c
typedef struct listNode {
    struct listNode *prev;  /* Previous node */
    struct listNode *next;  /* Next node */
    void *value;            /* Generic value pointer */
} listNode;
```

### List

```c
typedef struct list {
    listNode *head;                  /* First node */
    listNode *tail;                  /* Last node */
    void *(*dup)(void *ptr);         /* Optional: duplicate value */
    void (*free)(void *ptr);         /* Optional: free value */
    int (*match)(void *ptr, void *key); /* Optional: match value */
    unsigned long len;               /* Number of nodes */
} list;
```

### Iterator

```c
typedef struct listIter {
    listNode *next;     /* Next node to return */
    bool headToTail;    /* true = forward, false = backward */
} listIter;
```

## API Reference

### Creation and Destruction

```c
/* Create new empty list
 * Returns: new list or NULL on allocation failure
 */
list *listCreate(void);

/* Free entire list (calls free callback for each value if set)
 * list: list to free
 */
void listRelease(list *list);

/* Example */
list *mylist = listCreate();
// ... use list ...
listRelease(mylist);
```

### Adding Nodes

```c
/* Add node to head of list
 * list: list to modify
 * value: value pointer to store
 * Returns: list pointer on success, NULL on allocation failure
 */
list *listAddNodeHead(list *list, void *value);

/* Add node to tail of list
 * list: list to modify
 * value: value pointer to store
 * Returns: list pointer on success, NULL on allocation failure
 */
list *listAddNodeTail(list *list, void *value);

/* Insert node before or after existing node
 * list: list to modify
 * oldNode: reference node
 * value: value pointer to store
 * after: true to insert after oldNode, false to insert before
 * Returns: list pointer on success, NULL on allocation failure
 */
list *listInsertNode(list *list, listNode *oldNode,
                     void *value, int after);

/* Example: Build a list */
list *l = listCreate();

listAddNodeHead(l, "first");   // ["first"]
listAddNodeTail(l, "last");    // ["first", "last"]
listAddNodeHead(l, "new first"); // ["new first", "first", "last"]
```

### Removing Nodes

```c
/* Delete node from list
 * list: list to modify
 * node: node to delete (calls free callback if set)
 */
void listDelNode(list *list, listNode *node);

/* Example */
list *l = listCreate();
listNode *n = listAddNodeHead(l, "data");

// Later...
listDelNode(l, n);  // Removes node
```

### Accessing Nodes

```c
/* Get node at specific index
 * list: list to search
 * index: index (0 = head, -1 = tail, -2 = second to last, etc.)
 * Returns: node or NULL if out of range
 */
listNode *listIndex(list *list, long index);

/* Example */
list *l = listCreate();
listAddNodeTail(l, "first");
listAddNodeTail(l, "second");
listAddNodeTail(l, "third");

listNode *n = listIndex(l, 0);   // Gets "first"
n = listIndex(l, 1);              // Gets "second"
n = listIndex(l, -1);             // Gets "third" (last)
n = listIndex(l, -2);             // Gets "second" (second to last)
```

### Searching

```c
/* Search for node with matching key
 * list: list to search
 * key: key to match
 * Returns: first matching node or NULL
 * Uses match callback if set, otherwise compares pointers
 */
listNode *listSearchKey(list *list, void *key);

/* Example with pointer comparison */
list *l = listCreate();
char *str = "find me";
listAddNodeTail(l, str);
listAddNodeTail(l, "other");

listNode *found = listSearchKey(l, str);
if (found) {
    printf("Found: %s\n", (char *)found->value);
}
```

### Iteration

```c
/* Get iterator for list
 * list: list to iterate
 * headToTail: true for forward, false for backward
 * Returns: iterator or NULL on allocation failure
 */
listIter *listGetIterator(list *list, bool headToTail);

/* Get next node from iterator
 * iter: iterator
 * Returns: next node or NULL when exhausted
 */
listNode *listNext(listIter *iter);

/* Release iterator */
void listReleaseIterator(listIter *iter);

/* Rewind iterator to head (forward iteration) */
void listRewind(list *list, listIter *li);

/* Rewind iterator to tail (backward iteration) */
void listRewindTail(list *list, listIter *li);

/* Example: Forward iteration */
list *l = listCreate();
listAddNodeTail(l, "one");
listAddNodeTail(l, "two");
listAddNodeTail(l, "three");

listIter *iter = listGetIterator(l, true);
listNode *node;
while ((node = listNext(iter)) != NULL) {
    printf("%s\n", (char *)node->value);
}
listReleaseIterator(iter);

/* Example: Backward iteration */
iter = listGetIterator(l, false);
while ((node = listNext(iter)) != NULL) {
    printf("%s\n", (char *)node->value);
}
listReleaseIterator(iter);
```

### List Operations

```c
/* Duplicate list
 * orig: list to copy
 * Returns: new list copy or NULL on failure
 * Uses dup callback if set, otherwise copies pointers
 */
list *listDup(list *orig);

/* Rotate list (move tail to head)
 * list: list to rotate
 */
void listRotate(list *list);

/* Example: Rotation */
list *l = listCreate();
listAddNodeTail(l, "1");
listAddNodeTail(l, "2");
listAddNodeTail(l, "3");
// List: ["1", "2", "3"]

listRotate(l);
// List: ["3", "1", "2"]

listRotate(l);
// List: ["2", "3", "1"]
```

## Callback Functions

### Dup Callback

```c
/* Set custom duplication function
 * list: list to configure
 * dup: function to duplicate values
 */
#define listSetDupMethod(l, m)

/* Example */
void *myDup(void *ptr) {
    char *str = (char *)ptr;
    return strdup(str);
}

list *l = listCreate();
listSetDupMethod(l, myDup);

listAddNodeTail(l, "original");
list *copy = listDup(l);  // Uses myDup to copy values
```

### Free Callback

```c
/* Set custom free function
 * list: list to configure
 * free: function to free values
 */
#define listSetFreeMethod(l, m)

/* Example */
void myFree(void *ptr) {
    free(ptr);
}

list *l = listCreate();
listSetFreeMethod(l, myFree);

listAddNodeTail(l, strdup("data"));
listRelease(l);  // Calls myFree on each value before freeing list
```

### Match Callback

```c
/* Set custom match function
 * list: list to configure
 * match: function to compare values (returns non-zero if match)
 */
#define listSetMatchMethod(l, m)

/* Example */
int myMatch(void *ptr, void *key) {
    return strcmp((char *)ptr, (char *)key) == 0;
}

list *l = listCreate();
listSetMatchMethod(l, myMatch);

listAddNodeTail(l, "apple");
listAddNodeTail(l, "banana");

listNode *found = listSearchKey(l, "banana");
// Uses myMatch instead of pointer comparison
```

## Accessor Macros

```c
/* Get list length */
#define listLength(l)

/* Get first node */
#define listFirst(l)

/* Get last node */
#define listLast(l)

/* Get previous node */
#define listPrevNode(n)

/* Get next node */
#define listNextNode(n)

/* Get node value */
#define listNodeValue(n)

/* Example */
list *l = listCreate();
listAddNodeTail(l, "data");

printf("Length: %lu\n", listLength(l));
listNode *first = listFirst(l);
printf("First value: %s\n", (char *)listNodeValue(first));
```

## Real-World Examples

### Example 1: Task Queue

```c
typedef struct task {
    char name[64];
    void (*func)(void);
} task;

void freeTask(void *ptr) {
    free((task *)ptr);
}

list *taskQueue = listCreate();
listSetFreeMethod(taskQueue, freeTask);

/* Add tasks */
task *t1 = malloc(sizeof(*t1));
strcpy(t1->name, "Task 1");
t1->func = doTask1;
listAddNodeTail(taskQueue, t1);

task *t2 = malloc(sizeof(*t2));
strcpy(t2->name, "Task 2");
t2->func = doTask2;
listAddNodeTail(taskQueue, t2);

/* Process tasks */
while (listLength(taskQueue) > 0) {
    listNode *node = listFirst(taskQueue);
    task *t = listNodeValue(node);

    printf("Running: %s\n", t->name);
    t->func();

    listDelNode(taskQueue, node);  // Automatically frees task
}

listRelease(taskQueue);
```

### Example 2: LRU Cache

```c
typedef struct cacheEntry {
    char *key;
    void *data;
} cacheEntry;

list *lru = listCreate();

void cacheAccess(const char *key) {
    /* Find entry */
    listIter *iter = listGetIterator(lru, true);
    listNode *node;
    cacheEntry *entry = NULL;

    while ((node = listNext(iter)) != NULL) {
        cacheEntry *e = listNodeValue(node);
        if (strcmp(e->key, key) == 0) {
            entry = e;
            listDelNode(lru, node);
            break;
        }
    }
    listReleaseIterator(iter);

    if (entry) {
        /* Move to head (most recently used) */
        listAddNodeHead(lru, entry);
    } else {
        /* Not found, add new entry */
        entry = malloc(sizeof(*entry));
        entry->key = strdup(key);
        entry->data = loadData(key);
        listAddNodeHead(lru, entry);

        /* Evict least recently used if over limit */
        if (listLength(lru) > MAX_CACHE_SIZE) {
            listNode *last = listLast(lru);
            cacheEntry *evict = listNodeValue(last);
            free(evict->key);
            free(evict->data);
            free(evict);
            listDelNode(lru, last);
        }
    }
}
```

### Example 3: Command History

```c
list *history = listCreate();
listSetFreeMethod(history, free);

void addCommand(const char *cmd) {
    listAddNodeTail(history, strdup(cmd));

    /* Limit history size */
    while (listLength(history) > 100) {
        listNode *oldest = listFirst(history);
        listDelNode(history, oldest);
    }
}

void printHistory(void) {
    int i = 1;
    listIter *iter = listGetIterator(history, true);
    listNode *node;

    while ((node = listNext(iter)) != NULL) {
        printf("%3d  %s\n", i++, (char *)listNodeValue(node));
    }

    listReleaseIterator(iter);
}

/* Get command by index (negative = from end) */
const char *getHistoryCommand(long index) {
    listNode *node = listIndex(history, index);
    return node ? (const char *)listNodeValue(node) : NULL;
}
```

### Example 4: Event Subscribers

```c
typedef void (*eventHandler)(void *data);

int matchHandler(void *ptr, void *key) {
    return ptr == key;
}

list *subscribers = listCreate();
listSetMatchMethod(subscribers, matchHandler);

void subscribe(eventHandler handler) {
    listAddNodeTail(subscribers, (void *)handler);
}

void unsubscribe(eventHandler handler) {
    listNode *node = listSearchKey(subscribers, (void *)handler);
    if (node) {
        listDelNode(subscribers, node);
    }
}

void notifyAll(void *eventData) {
    listIter *iter = listGetIterator(subscribers, true);
    listNode *node;

    while ((node = listNext(iter)) != NULL) {
        eventHandler handler = (eventHandler)listNodeValue(node);
        handler(eventData);
    }

    listReleaseIterator(iter);
}
```

## Performance Characteristics

| Operation   | Complexity | Notes                              |
| ----------- | ---------- | ---------------------------------- |
| Create      | O(1)       | Single allocation                  |
| AddNodeHead | O(1)       | Direct insertion                   |
| AddNodeTail | O(1)       | Direct insertion with tail pointer |
| InsertNode  | O(1)       | Direct insertion at known position |
| DelNode     | O(1)       | Direct deletion with known node    |
| Index       | O(n)       | Must traverse from head or tail    |
| SearchKey   | O(n)       | Linear search                      |
| Length      | O(1)       | Cached in structure                |
| Rotate      | O(1)       | Just pointer updates               |
| Dup         | O(n)       | Must copy all nodes                |
| Release     | O(n)       | Must free all nodes                |

### Memory Usage

```
Per list: 40 bytes (head, tail, callbacks, length)
Per node: 24 bytes (prev, next, value pointers)

For 1000 nodes:
- List overhead: 40 bytes
- Node overhead: 24,000 bytes
- Value pointers: 8,000 bytes (stored separately)
- Total: 32,040 bytes
```

## Best Practices

### 1. Always Free Lists

```c
/* GOOD */
list *l = listCreate();
// ... use list ...
listRelease(l);

/* BAD */
list *l = listCreate();
// ... use list ...
// Never freed - memory leak!
```

### 2. Set Free Callback for Allocated Values

```c
/* GOOD */
list *l = listCreate();
listSetFreeMethod(l, free);
listAddNodeTail(l, strdup("data"));
listRelease(l);  // Automatically frees "data"

/* BAD */
list *l = listCreate();
listAddNodeTail(l, strdup("data"));
listRelease(l);  // Memory leak - "data" not freed!
```

### 3. Don't Modify During Iteration

```c
/* WRONG - modifying while iterating */
listIter *iter = listGetIterator(l, true);
listNode *node;
while ((node = listNext(iter)) != NULL) {
    listDelNode(l, node);  // Breaks iterator!
}

/* RIGHT - safe deletion */
listIter *iter = listGetIterator(l, true);
listNode *node;
while ((node = listNext(iter)) != NULL) {
    listNode *toDelete = node;
    node = listNext(iter);  // Advance first
    listDelNode(l, toDelete);  // Safe
}
```

### 4. Check Return Values

```c
/* GOOD */
listNode *node = listIndex(l, 5);
if (node) {
    void *value = listNodeValue(node);
} else {
    printf("Index out of range\n");
}

/* BAD */
listNode *node = listIndex(l, 5);
void *value = listNodeValue(node);  // May crash if node is NULL!
```

## Common Pitfalls

### 1. Double Free

```c
/* WRONG */
list *l = listCreate();
char *str = strdup("data");
listAddNodeTail(l, str);
free(str);  // Freed manually
listRelease(l);  // Tries to free again if free callback set!

/* RIGHT */
list *l = listCreate();
listSetFreeMethod(l, free);
listAddNodeTail(l, strdup("data"));
listRelease(l);  // Frees automatically
```

### 2. Using Freed Node

```c
/* WRONG */
listNode *node = listFirst(l);
listDelNode(l, node);
void *value = listNodeValue(node);  // node is freed!

/* RIGHT */
listNode *node = listFirst(l);
void *value = listNodeValue(node);  // Get value first
listDelNode(l, node);
```

### 3. Iterator Leaks

```c
/* BAD */
listIter *iter = listGetIterator(l, true);
listNode *node = listNext(iter);
return;  // Iterator leaked!

/* GOOD */
listIter *iter = listGetIterator(l, true);
listNode *node = listNext(iter);
listReleaseIterator(iter);
```

## Comparison with Other Structures

| Feature       | list   | array | hash table |
| ------------- | ------ | ----- | ---------- |
| Ordered       | Yes    | Yes   | No         |
| Random access | O(n)   | O(1)  | N/A        |
| Insert/Delete | O(1)\* | O(n)  | O(1) avg   |
| Search        | O(n)   | O(n)  | O(1) avg   |
| Memory        | Medium | Low   | High       |

\*O(1) when node is known, O(n) for search

## See Also

- [ptrPrevNext](../memory/PTR_PREV_NEXT.md) - Compact linked list pointer storage
- [flex](../flex/FLEX.md) - Alternative flexible array structure

## Testing

This is a legacy module used only in tests. The implementation is stable and well-tested through Redis's extensive test suite.
