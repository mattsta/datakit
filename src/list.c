/* list.c - A generic doubly linked list implementation
 *
 * NOTE: this file is only used for some legacy tests. The list structure here
 *       is not used by any of the actual data structures in the library.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "list.h"

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
list *listCreate(void) {
    struct list *l;

    if ((l = zmalloc(sizeof(*l))) == NULL) {
        return NULL;
    }

    l->head = l->tail = NULL;
    l->len = 0;
    l->dup = NULL;
    l->free = NULL;
    l->match = NULL;
    return l;
}

/* Free the whole list.
 *
 * This function can't fail. */
void listRelease(list *l) {
    signed long len;
    listNode *current, *next;

    current = l->head;
    len = l->len;
    while (len--) {
        next = current->next;
        if (l->free) {
            l->free(current->value);
        }

        zfree(current);
        current = next;
    }

    zfree(l);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
list *listAddNodeHead(list *l, void *value) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) {
        return NULL;
    }

    node->value = value;
    if (l->len == 0) {
        l->head = l->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = l->head;
        l->head->prev = node;
        l->head = node;
    }

    l->len++;
    return l;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
list *listAddNodeTail(list *l, void *value) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) {
        return NULL;
    }

    node->value = value;
    if (l->len == 0) {
        l->head = l->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = l->tail;
        node->next = NULL;
        l->tail->next = node;
        l->tail = node;
    }

    l->len++;
    return l;
}

list *lInsertNode(list *l, listNode *oldNode, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) {
        return NULL;
    }

    node->value = value;
    if (after) {
        node->prev = oldNode;
        node->next = oldNode->next;
        if (l->tail == oldNode) {
            l->tail = node;
        }
    } else {
        node->next = oldNode;
        node->prev = oldNode->prev;
        if (l->head == oldNode) {
            l->head = node;
        }
    }
    if (node->prev != NULL) {
        node->prev->next = node;
    }

    if (node->next != NULL) {
        node->next->prev = node;
    }

    l->len++;
    return l;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
void listDelNode(list *l, listNode *node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        l->head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        l->tail = node->prev;
    }

    if (l->free) {
        l->free(node->value);
    }

    zfree(node);
    l->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
listIter *listGetIterator(list *l, bool headToTail) {
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) {
        return NULL;
    }

    if (headToTail) {
        iter->next = l->head;
    } else {
        iter->next = l->tail;
    }

    iter->headToTail = headToTail;
    return iter;
}

/* Release the iterator memory */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
void listRewind(list *l, listIter *li) {
    li->next = l->head;
    li->headToTail = true;
}

void listRewindTail(list *l, listIter *li) {
    li->next = l->tail;
    li->headToTail = false;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }

 *
 * */
listNode *listNext(listIter *iter) {
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->headToTail) {
            iter->next = current->next;
        } else {
            iter->next = current->prev;
        }
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
list *listDup(list *orig) {
    list *copy;
    listIter *iter;
    listNode *node;

    if ((copy = listCreate()) == NULL) {
        return NULL;
    }

    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    iter = listGetIterator(orig, true);
    while ((node = listNext(iter)) != NULL) {
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                listReleaseIterator(iter);
                return NULL;
            }
        } else {
            value = node->value;
        }

        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            listReleaseIterator(iter);
            return NULL;
        }
    }
    listReleaseIterator(iter);
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
listNode *listSearchKey(list *l, void *key) {
    listIter *iter;
    listNode *node;

    iter = listGetIterator(l, true);
    while ((node = listNext(iter)) != NULL) {
        if (l->match) {
            if (l->match(node->value, key)) {
                listReleaseIterator(iter);
                return node;
            }
        } else {
            if (key == node->value) {
                listReleaseIterator(iter);
                return node;
            }
        }
    }

    listReleaseIterator(iter);
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
listNode *listIndex(list *l, int64_t index) {
    listNode *n;

    if (index < 0) {
        index = (-index) - 1;
        n = l->tail;
        while (index-- && n) {
            n = n->prev;
        }
    } else {
        n = l->head;
        while (index-- && n) {
            n = n->next;
        }
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
void listRotate(list *l) {
    listNode *tail = l->tail;

    if (listLength(l) <= 1) {
        return;
    }

    /* Detach current tail */
    l->tail = tail->prev;
    l->tail->next = NULL;
    /* Move it as head */
    l->head->prev = tail;
    tail->prev = NULL;
    tail->next = l->head;
    l->head = tail;
}

#ifdef DATAKIT_TEST
#include "ctest.h"

int listTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    /* Test listCreate and listRelease */
    printf("Testing listCreate and listRelease...\n");
    {
        list *l = listCreate();
        if (l == NULL) {
            ERR("listCreate returned NULL%s", "");
        }

        if (listLength(l) != 0) {
            ERR("New list should have length 0, got %zu", listLength(l));
        }

        if (listFirst(l) != NULL) {
            ERR("New list head should be NULL%s", "");
        }

        if (listLast(l) != NULL) {
            ERR("New list tail should be NULL%s", "");
        }

        listRelease(l);
    }

    /* Test listAddNodeHead */
    printf("Testing listAddNodeHead...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3, 4, 5};

        for (int32_t i = 0; i < 5; i++) {
            list *result = listAddNodeHead(l, &values[i]);
            if (result == NULL) {
                ERR("listAddNodeHead returned NULL for value %d", i);
            }
        }

        if (listLength(l) != 5) {
            ERR("Expected length 5, got %zu", listLength(l));
        }

        /* Head should be the last added (5), tail should be first added (1) */
        if (*(int32_t *)listNodeValue(listFirst(l)) != 5) {
            ERR("Expected head value 5, got %d",
                *(int32_t *)listNodeValue(listFirst(l)));
        }

        if (*(int32_t *)listNodeValue(listLast(l)) != 1) {
            ERR("Expected tail value 1, got %d",
                *(int32_t *)listNodeValue(listLast(l)));
        }

        listRelease(l);
    }

    /* Test listAddNodeTail */
    printf("Testing listAddNodeTail...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3, 4, 5};

        for (int32_t i = 0; i < 5; i++) {
            list *result = listAddNodeTail(l, &values[i]);
            if (result == NULL) {
                ERR("listAddNodeTail returned NULL for value %d", i);
            }
        }

        if (listLength(l) != 5) {
            ERR("Expected length 5, got %zu", listLength(l));
        }

        /* Head should be first added (1), tail should be last added (5) */
        if (*(int32_t *)listNodeValue(listFirst(l)) != 1) {
            ERR("Expected head value 1, got %d",
                *(int32_t *)listNodeValue(listFirst(l)));
        }

        if (*(int32_t *)listNodeValue(listLast(l)) != 5) {
            ERR("Expected tail value 5, got %d",
                *(int32_t *)listNodeValue(listLast(l)));
        }

        listRelease(l);
    }

    /* Test listDelNode */
    printf("Testing listDelNode...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3};

        for (int32_t i = 0; i < 3; i++) {
            listAddNodeTail(l, &values[i]);
        }

        /* Delete middle node */
        listNode *middle = listFirst(l)->next;
        listDelNode(l, middle);

        if (listLength(l) != 2) {
            ERR("Expected length 2 after delete, got %zu", listLength(l));
        }

        /* Verify structure: head -> tail */
        if (*(int32_t *)listNodeValue(listFirst(l)) != 1) {
            ERR("Expected head value 1 after delete, got %d",
                *(int32_t *)listNodeValue(listFirst(l)));
        }

        if (*(int32_t *)listNodeValue(listLast(l)) != 3) {
            ERR("Expected tail value 3 after delete, got %d",
                *(int32_t *)listNodeValue(listLast(l)));
        }

        /* Verify linkage */
        if (listFirst(l)->next != listLast(l)) {
            ERR("Head->next should point to tail%s", "");
        }

        if (listLast(l)->prev != listFirst(l)) {
            ERR("Tail->prev should point to head%s", "");
        }

        listRelease(l);
    }

    /* Test listGetIterator and listNext (head to tail) */
    printf("Testing listGetIterator head to tail...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3, 4, 5};

        for (int32_t i = 0; i < 5; i++) {
            listAddNodeTail(l, &values[i]);
        }

        listIter *iter = listGetIterator(l, true);
        if (iter == NULL) {
            ERR("listGetIterator returned NULL%s", "");
        }

        listNode *node;
        int32_t expected = 1;
        while ((node = listNext(iter)) != NULL) {
            int32_t val = *(int32_t *)listNodeValue(node);
            if (val != expected) {
                ERR("Iterator expected %d, got %d", expected, val);
            }
            expected++;
        }

        if (expected != 6) {
            ERR("Iterator didn't visit all nodes, expected count 6, got %d",
                expected);
        }

        listReleaseIterator(iter);
        listRelease(l);
    }

    /* Test listGetIterator and listNext (tail to head) */
    printf("Testing listGetIterator tail to head...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3, 4, 5};

        for (int32_t i = 0; i < 5; i++) {
            listAddNodeTail(l, &values[i]);
        }

        listIter *iter = listGetIterator(l, false);
        if (iter == NULL) {
            ERR("listGetIterator returned NULL%s", "");
        }

        listNode *node;
        int32_t expected = 5;
        while ((node = listNext(iter)) != NULL) {
            int32_t val = *(int32_t *)listNodeValue(node);
            if (val != expected) {
                ERR("Iterator expected %d, got %d", expected, val);
            }
            expected--;
        }

        if (expected != 0) {
            ERR("Iterator didn't visit all nodes, expected count 0, got %d",
                expected);
        }

        listReleaseIterator(iter);
        listRelease(l);
    }

    /* Test listRewind and listRewindTail */
    printf("Testing listRewind and listRewindTail...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3};

        for (int32_t i = 0; i < 3; i++) {
            listAddNodeTail(l, &values[i]);
        }

        listIter li;
        listRewind(l, &li);

        listNode *node = listNext(&li);
        if (*(int32_t *)listNodeValue(node) != 1) {
            ERR("listRewind should start at head (1), got %d",
                *(int32_t *)listNodeValue(node));
        }

        listRewindTail(l, &li);
        node = listNext(&li);
        if (*(int32_t *)listNodeValue(node) != 3) {
            ERR("listRewindTail should start at tail (3), got %d",
                *(int32_t *)listNodeValue(node));
        }

        listRelease(l);
    }

    /* Test listDup */
    printf("Testing listDup...\n");
    {
        list *orig = listCreate();
        int32_t values[] = {10, 20, 30, 40, 50};

        for (int32_t i = 0; i < 5; i++) {
            listAddNodeTail(orig, &values[i]);
        }

        list *copy = listDup(orig);
        if (copy == NULL) {
            ERR("listDup returned NULL%s", "");
        }

        if (listLength(copy) != listLength(orig)) {
            ERR("Copy length %lu != orig length %zu", listLength(copy),
                listLength(orig));
        }

        /* Verify values match */
        listIter *origIter = listGetIterator(orig, true);
        listIter *copyIter = listGetIterator(copy, true);
        listNode *origNode, *copyNode;

        while ((origNode = listNext(origIter)) != NULL) {
            copyNode = listNext(copyIter);
            if (copyNode == NULL) {
                ERR("Copy has fewer nodes than original%s", "");
                break;
            }

            /* Without dup method, pointers should be the same */
            if (listNodeValue(origNode) != listNodeValue(copyNode)) {
                ERR("Value pointers should be identical without dup method%s",
                    "");
            }
        }

        listReleaseIterator(origIter);
        listReleaseIterator(copyIter);
        listRelease(orig);
        listRelease(copy);
    }

    /* Test listSearchKey */
    printf("Testing listSearchKey...\n");
    {
        list *l = listCreate();
        int32_t values[] = {100, 200, 300, 400, 500};

        for (int32_t i = 0; i < 5; i++) {
            listAddNodeTail(l, &values[i]);
        }

        /* Search for existing value (by pointer) */
        listNode *found = listSearchKey(l, &values[2]);
        if (found == NULL) {
            ERR("listSearchKey didn't find existing key%s", "");
        } else if (*(int32_t *)listNodeValue(found) != 300) {
            ERR("listSearchKey found wrong value: %d",
                *(int32_t *)listNodeValue(found));
        }

        /* Search for non-existing pointer */
        int32_t notInList = 300; /* Same value but different pointer */
        found = listSearchKey(l, &notInList);
        if (found != NULL) {
            ERR("listSearchKey should not find different pointer%s", "");
        }

        listRelease(l);
    }

    /* Test listIndex */
    printf("Testing listIndex...\n");
    {
        list *l = listCreate();
        int32_t values[] = {10, 20, 30, 40, 50};

        for (int32_t i = 0; i < 5; i++) {
            listAddNodeTail(l, &values[i]);
        }

        /* Positive indices */
        listNode *node = listIndex(l, 0);
        if (node == NULL || *(int32_t *)listNodeValue(node) != 10) {
            ERR("listIndex(0) should return 10%s", "");
        }

        node = listIndex(l, 2);
        if (node == NULL || *(int32_t *)listNodeValue(node) != 30) {
            ERR("listIndex(2) should return 30%s", "");
        }

        node = listIndex(l, 4);
        if (node == NULL || *(int32_t *)listNodeValue(node) != 50) {
            ERR("listIndex(4) should return 50%s", "");
        }

        /* Negative indices */
        node = listIndex(l, -1);
        if (node == NULL || *(int32_t *)listNodeValue(node) != 50) {
            ERR("listIndex(-1) should return 50%s", "");
        }

        node = listIndex(l, -3);
        if (node == NULL || *(int32_t *)listNodeValue(node) != 30) {
            ERR("listIndex(-3) should return 30%s", "");
        }

        node = listIndex(l, -5);
        if (node == NULL || *(int32_t *)listNodeValue(node) != 10) {
            ERR("listIndex(-5) should return 10%s", "");
        }

        /* Out of bounds */
        node = listIndex(l, 5);
        if (node != NULL) {
            ERR("listIndex(5) should return NULL for 5-element list%s", "");
        }

        node = listIndex(l, -6);
        if (node != NULL) {
            ERR("listIndex(-6) should return NULL for 5-element list%s", "");
        }

        listRelease(l);
    }

    /* Test listRotate */
    printf("Testing listRotate...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3, 4, 5};

        for (int32_t i = 0; i < 5; i++) {
            listAddNodeTail(l, &values[i]);
        }

        /* Initial: 1 -> 2 -> 3 -> 4 -> 5 */
        /* After rotate: 5 -> 1 -> 2 -> 3 -> 4 */
        listRotate(l);

        if (*(int32_t *)listNodeValue(listFirst(l)) != 5) {
            ERR("After rotate, head should be 5, got %d",
                *(int32_t *)listNodeValue(listFirst(l)));
        }

        if (*(int32_t *)listNodeValue(listLast(l)) != 4) {
            ERR("After rotate, tail should be 4, got %d",
                *(int32_t *)listNodeValue(listLast(l)));
        }

        /* Verify full order: 5, 1, 2, 3, 4 */
        int32_t expected[] = {5, 1, 2, 3, 4};
        listIter *iter = listGetIterator(l, true);
        listNode *node;
        int32_t idx = 0;
        while ((node = listNext(iter)) != NULL) {
            if (*(int32_t *)listNodeValue(node) != expected[idx]) {
                ERR("After rotate, index %d expected %d, got %d", idx,
                    expected[idx], *(int32_t *)listNodeValue(node));
            }
            idx++;
        }
        listReleaseIterator(iter);

        listRelease(l);
    }

    /* Test empty list rotation */
    printf("Testing listRotate on empty/single-element list...\n");
    {
        list *l = listCreate();

        /* Rotate empty list should not crash */
        listRotate(l);
        if (listLength(l) != 0) {
            ERR("Empty list rotation changed length%s", "");
        }

        /* Single element */
        int32_t val = 42;
        listAddNodeTail(l, &val);
        listRotate(l);

        if (listLength(l) != 1) {
            ERR("Single element rotation changed length%s", "");
        }

        if (*(int32_t *)listNodeValue(listFirst(l)) != 42) {
            ERR("Single element rotation changed value%s", "");
        }

        listRelease(l);
    }

    /* Test deletion during iteration */
    printf("Testing deletion during iteration...\n");
    {
        list *l = listCreate();
        int32_t values[] = {1, 2, 3, 4, 5};

        for (int32_t i = 0; i < 5; i++) {
            listAddNodeTail(l, &values[i]);
        }

        /* Delete every other node */
        listIter *iter = listGetIterator(l, true);
        listNode *node;
        int32_t count = 0;
        while ((node = listNext(iter)) != NULL) {
            if (count % 2 == 1) {
                listDelNode(l, node);
            }
            count++;
        }
        listReleaseIterator(iter);

        /* Should have 1, 3, 5 remaining */
        if (listLength(l) != 3) {
            ERR("Expected 3 nodes after deletion, got %zu", listLength(l));
        }

        int32_t expectedVals[] = {1, 3, 5};
        iter = listGetIterator(l, true);
        int32_t idx = 0;
        while ((node = listNext(iter)) != NULL) {
            if (*(int32_t *)listNodeValue(node) != expectedVals[idx]) {
                ERR("Expected value %d at index %d, got %d", expectedVals[idx],
                    idx, *(int32_t *)listNodeValue(node));
            }
            idx++;
        }
        listReleaseIterator(iter);

        listRelease(l);
    }

    TEST_FINAL_RESULT;
}
#endif
