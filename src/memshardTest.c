/* memshardTest - correctness tests for the vendored/adapted memshard allocator.
 *
 * Standalone target (like memboundBench); build under the `dev` preset so it
 * runs with ASAN/UBSAN. Exercises malloc/free/calloc/realloc/zalloc, usable
 * size, aligned alloc, first-class heaps, and — most importantly for the
 * lock-free design — multithreaded local and CROSS-THREAD free. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mimalloc.h"

#define T(name) (printf("test — %s\n", name), fflush(stdout))

/* ---- single-thread correctness ---- */
static void test_basic(void) {
    T("memshard: malloc/free + write/read");
    for (size_t sz = 1; sz <= (1u << 20); sz <<= 1) {
        unsigned char *p = (unsigned char *)mi_malloc(sz);
        assert(p);
        memset(p, 0xA5, sz);
        assert(p[0] == 0xA5 && p[sz - 1] == 0xA5);
        assert(mi_usable_size(p) >= sz);
        mi_free(p);
    }
}

static void test_calloc_zero(void) {
    T("memshard: calloc is zeroed + overflow-safe");
    size_t n = 1000;
    uint64_t *p = (uint64_t *)mi_calloc(n, sizeof(uint64_t));
    assert(p);
    for (size_t i = 0; i < n; i++) {
        assert(p[i] == 0);
    }
    mi_free(p);
    /* overflow must return NULL, not a tiny buffer */
    assert(mi_calloc((size_t)-1, 2) == NULL);
}

static void test_realloc(void) {
    T("memshard: realloc grows preserving data, shrinks");
    size_t sz = 64;
    unsigned char *p = (unsigned char *)mi_malloc(sz);
    assert(p);
    for (size_t i = 0; i < sz; i++) {
        p[i] = (unsigned char)i;
    }
    p = (unsigned char *)mi_realloc(p, sz * 64);
    assert(p);
    for (size_t i = 0; i < sz; i++) {
        assert(p[i] == (unsigned char)i); /* preserved across grow */
    }
    p = (unsigned char *)mi_realloc(p, 32);
    assert(p);
    assert(p[0] == 0 && p[31] == 31);
    mi_free(p);
}

static void test_zalloc(void) {
    T("memshard: zalloc is zeroed");
    size_t sz = 4096;
    unsigned char *p = (unsigned char *)mi_zalloc(sz);
    assert(p);
    for (size_t i = 0; i < sz; i++) {
        assert(p[i] == 0);
    }
    mi_free(p);
}

static void test_aligned(void) {
    T("memshard: aligned allocation honors alignment");
    for (size_t align = 16; align <= 4096; align <<= 1) {
        void *p = mi_malloc_aligned(1234, align);
        assert(p);
        assert(((uintptr_t)p % align) == 0);
        mi_free(p);
    }
}

static void test_heap(void) {
    T("memshard: first-class heap create/alloc/destroy");
    mi_heap_t *h = mi_heap_new();
    assert(h);
    void *ptrs[256];
    for (int i = 0; i < 256; i++) {
        ptrs[i] = mi_heap_malloc(h, (size_t)(i + 1) * 8);
        assert(ptrs[i]);
        memset(ptrs[i], i & 0xff, (size_t)(i + 1) * 8);
    }
    /* destroy frees everything at once (no per-block free needed) */
    mi_heap_destroy(h);
}

/* ---- multithread: local churn ---- */
#define MT_THREADS 8
#define MT_ITERS 20000

static void *mt_local(void *arg) {
    (void)arg;
    void *keep[64] = {0};
    for (int i = 0; i < MT_ITERS; i++) {
        int slot = i % 64;
        if (keep[slot]) {
            mi_free(keep[slot]);
        }
        size_t sz = (size_t)((i * 37) % 4096) + 1;
        keep[slot] = mi_malloc(sz);
        assert(keep[slot]);
        ((unsigned char *)keep[slot])[0] = (unsigned char)i;
    }
    for (int s = 0; s < 64; s++) {
        if (keep[s]) {
            mi_free(keep[s]);
        }
    }
    return NULL;
}

static void test_mt_local(void) {
    T("memshard: MT local alloc/free churn (8 threads)");
    pthread_t th[MT_THREADS];
    for (int i = 0; i < MT_THREADS; i++) {
        assert(pthread_create(&th[i], NULL, mt_local, NULL) == 0);
    }
    for (int i = 0; i < MT_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
}

/* ---- multithread: CROSS-THREAD free (the lock-free xthread path) ---- */
#define XQ 4096
static void **xq;            /* producer fills, consumers free */
static _Atomic int xq_head;  /* next slot to free */
static _Atomic int xq_count; /* produced so far */

static void *mt_producer(void *arg) {
    (void)arg;
    for (int i = 0; i < XQ; i++) {
        void *p = mi_malloc((size_t)((i * 53) % 2048) + 1);
        assert(p);
        xq[i] = p;
        /* @sync: release memshardTest.xqSlot */
        atomic_store_explicit(&xq_count, i + 1, memory_order_release);
    }
    return NULL;
}

static void *mt_consumer(void *arg) {
    (void)arg;
    for (;;) {
        /* @sync: relaxed counter */
        int idx = atomic_fetch_add_explicit(&xq_head, 1, memory_order_relaxed);
        if (idx >= XQ) {
            break;
        }
        /* @sync: acquire memshardTest.xqSlot */
        while (atomic_load_explicit(&xq_count, memory_order_acquire) <= idx) {
            /* spin until producer has published this slot */
        }
        mi_free(xq[idx]); /* freed by a thread that did NOT allocate it */
    }
    return NULL;
}

static void test_mt_crossfree(void) {
    T("memshard: MT cross-thread free (lock-free xthread path)");
    xq = (void **)mi_malloc(sizeof(void *) * XQ);
    assert(xq);
    /* @sync: relaxed init */
    atomic_store_explicit(&xq_head, 0, memory_order_relaxed);
    /* @sync: relaxed init */
    atomic_store_explicit(&xq_count, 0, memory_order_relaxed);
    pthread_t prod, cons[3];
    pthread_create(&prod, NULL, mt_producer, NULL);
    for (int i = 0; i < 3; i++) {
        pthread_create(&cons[i], NULL, mt_consumer, NULL);
    }
    pthread_join(prod, NULL);
    for (int i = 0; i < 3; i++) {
        pthread_join(cons[i], NULL);
    }
    mi_free(xq);
}

int main(void) {
    test_basic();
    test_calloc_zero();
    test_realloc();
    test_zalloc();
    test_aligned();
    test_heap();
    test_mt_local();
    test_mt_crossfree();
    printf("ALL MEMSHARD TESTS PASSED!\n");
    return 0;
}
