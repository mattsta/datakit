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
listNode *listIndex(list *l, long index) {
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
