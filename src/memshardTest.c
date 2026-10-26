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

static void *mt_local(void *) {
    void *keep[64] = {};
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

static void *mt_producer(void *) {
    for (int i = 0; i < XQ; i++) {
        void *p = mi_malloc((size_t)((i * 53) % 2048) + 1);
        assert(p);
        xq[i] = p;
        /* @sync: release memshardTest.xqSlot */
        atomic_store_explicit(&xq_count, i + 1, memory_order_release);
    }
    return NULL;
}

static void *mt_consumer(void *) {
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

/* ---- heap delete while another thread frees its blocks ----
 * mi_heap_delete moves the heap's pages to the thread's default heap
 * while a freeing thread may be pushing a block onto the deleted heap's
 * delayed-free list (full pages use it).  The move must either wait the
 * freer out or be seen by it; a freer that pushed onto the freed heap is
 * a use-after-free ASan reports. */
#define HD_ROUNDS 40
#define HD_BLOCKS 2048
static void *hd_blocks[HD_BLOCKS];
static _Atomic int hd_go;

static void *hd_freer(void *) {
    /* @sync: acquire memshardTest.hdGo */
    while (!atomic_load_explicit(&hd_go, memory_order_acquire)) {
        /* spin until the blocks are published */
    }
    for (int i = 0; i < HD_BLOCKS; i++) {
        mi_free(hd_blocks[i]); /* cross-thread: the owner is deleting */
    }
    return NULL;
}

static void test_mt_heap_delete_crossfree(void) {
    T("memshard: heap delete during cross-thread frees");
    for (int r = 0; r < HD_ROUNDS; r++) {
        mi_heap_t *h = mi_heap_new();
        assert(h);
        /* One size, so the pages fill and sit in the full queue, whose
         * blocks are freed through the heap's delayed-free list. */
        for (int i = 0; i < HD_BLOCKS; i++) {
            hd_blocks[i] = mi_heap_malloc(h, 64);
            assert(hd_blocks[i]);
            memset(hd_blocks[i], r & 0xff, 64);
        }
        /* @sync: relaxed init */
        atomic_store_explicit(&hd_go, 0, memory_order_relaxed);
        pthread_t th;
        assert(pthread_create(&th, NULL, hd_freer, NULL) == 0);
        /* @sync: release memshardTest.hdGo */
        atomic_store_explicit(&hd_go, 1, memory_order_release);
        mi_heap_delete(h); /* pages move to this thread's default heap */
        pthread_join(th, NULL);
        mi_collect(true); /* process every delayed free */
    }
}

/* ---- usable size from a non-owner while the owner moves pages ----
 * The owner pins blocks of one size, then churns that size so their
 * pages move in and out of the full queue (a write to the page flags);
 * another thread reads mi_usable_size of the pinned blocks, which reads
 * the same flags (TSan reports a plain read racing that write). */
#define US_PINNED 256
#define US_ROUNDS 200
static void *us_pinned[US_PINNED];
static _Atomic int us_state; /* 1: pinned published, 2: owner done */

static void *us_reader(void *arg) {
    size_t *bad = arg;
    /* @sync: acquire memshardTest.usState */
    while (atomic_load_explicit(&us_state, memory_order_acquire) == 0) {
        /* spin until the pinned blocks are published */
    }
    /* @sync: relaxed hint */
    while (atomic_load_explicit(&us_state, memory_order_relaxed) != 2) {
        for (int i = 0; i < US_PINNED; i++) {
            if (mi_usable_size(us_pinned[i]) < 48) {
                (*bad)++;
            }
        }
    }
    return NULL;
}

static void test_mt_usable_size_remote(void) {
    T("memshard: usable size from another thread while pages move");
    for (int i = 0; i < US_PINNED; i++) {
        us_pinned[i] = mi_malloc(48);
        assert(us_pinned[i]);
    }
    /* @sync: relaxed init */
    atomic_store_explicit(&us_state, 0, memory_order_relaxed);
    size_t bad = 0;
    pthread_t th;
    assert(pthread_create(&th, NULL, us_reader, &bad) == 0);
    /* @sync: release memshardTest.usState */
    atomic_store_explicit(&us_state, 1, memory_order_release);
    void *churn[1024];
    for (int r = 0; r < US_ROUNDS; r++) {
        for (int i = 0; i < 1024; i++) {
            churn[i] = mi_malloc(48);
            assert(churn[i]);
        }
        for (int i = 0; i < 1024; i++) {
            mi_free(churn[i]);
        }
    }
    /* @sync: relaxed hint */
    atomic_store_explicit(&us_state, 2, memory_order_relaxed);
    pthread_join(th, NULL);
    assert(bad == 0);
    for (int i = 0; i < US_PINNED; i++) {
        mi_free(us_pinned[i]);
    }
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
    test_mt_heap_delete_crossfree();
    test_mt_usable_size_remote();
    printf("ALL MEMSHARD TESTS PASSED!\n");
    return 0;
}
