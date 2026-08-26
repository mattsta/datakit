/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// ------------------------------------------------------------------------
// mi prefixed publi definitions of various Posix, Unix, and C++ functions
// for convenience and used when overriding these functions.
// ------------------------------------------------------------------------
#include "mimalloc.h"
#include "mimalloc/internal.h"

// ------------------------------------------------------
// Posix & Unix functions definitions
// ------------------------------------------------------

#include <errno.h>
#include <stdlib.h> // getenv
#include <string.h> // memset

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

mi_decl_nodiscard size_t mi_malloc_size(const void *p) mi_attr_noexcept {
    // if (!mi_is_in_heap_region(p)) return 0;
    return mi_usable_size(p);
}

mi_decl_nodiscard size_t mi_malloc_usable_size(const void *p) mi_attr_noexcept {
    // if (!mi_is_in_heap_region(p)) return 0;
    return mi_usable_size(p);
}

mi_decl_nodiscard size_t mi_malloc_good_size(size_t size) mi_attr_noexcept {
    return mi_good_size(size);
}

void mi_cfree(void *p) mi_attr_noexcept {
    if (mi_is_in_heap_region(p)) {
        mi_free(p);
    }
}

int mi_posix_memalign(void **p, size_t alignment,
                      size_t size) mi_attr_noexcept {
    // Note: The spec dictates we should not modify `*p` on an error. (issue#27)
    // <http://man7.org/linux/man-pages/man3/posix_memalign.3.html>
    if (p == NULL) {
        return EINVAL;
    }
    // it is required that alignment is a power of 2 and a multiple of
    // sizeof(void*)
    if (alignment < sizeof(void *) || !_mi_is_power_of_two(alignment)) {
        return EINVAL; // not a valid alignment
    }
    void *q = mi_malloc_aligned(size, alignment);
    if (q == NULL && size != 0) {
        return ENOMEM;
    }
    mi_assert_internal(_mi_is_aligned(q, alignment));
    *p = q;
    return 0;
}

mi_decl_nodiscard mi_decl_restrict void *
mi_memalign(size_t alignment, size_t size) mi_attr_noexcept {
    void *p = mi_malloc_aligned(size, alignment);
    mi_assert_internal(_mi_is_aligned(p, alignment));
    return p;
}

mi_decl_nodiscard mi_decl_restrict void *
mi_valloc(size_t size) mi_attr_noexcept {
    return mi_memalign(_mi_os_page_size(), size);
}

mi_decl_nodiscard mi_decl_restrict void *
mi_pvalloc(size_t size) mi_attr_noexcept {
    size_t psize = _mi_os_page_size();
    if (size >= SIZE_MAX - psize) {
        return NULL; // overflow
    }
    size_t asize = _mi_align_up(size, psize);
    return mi_malloc_aligned(asize, psize);
}

mi_decl_nodiscard mi_decl_restrict void *
mi_aligned_alloc(size_t alignment, size_t size) mi_attr_noexcept {
    // C11 requires the size to be an integral multiple of the alignment, see
    // <https://en.cppreference.com/w/c/memory/aligned_alloc>. unfortunately, it
    // turns out quite some programs pass a size that is not an integral
    // multiple so skip this check..
    /* if mi_unlikely((size & (alignment - 1)) != 0) { // C11 requires
      alignment>0 && integral multiple, see
      <https://en.cppreference.com/w/c/memory/aligned_alloc> #if MI_DEBUG > 0
        _mi_error_message(EOVERFLOW, "(mi_)aligned_alloc requires the size to be
      an integral multiple of the alignment (size %zu, alignment %zu)\n", size,
      alignment); #endif return NULL;
      }
    */
    // C11 also requires alignment to be a power-of-two (and > 0) which is
    // checked in mi_malloc_aligned
    void *p = mi_malloc_aligned(size, alignment);
    mi_assert_internal(_mi_is_aligned(p, alignment));
    return p;
}

mi_decl_nodiscard void *mi_reallocarray(void *p, size_t count, size_t size)
    mi_attr_noexcept { // BSD
                       // <https://man.freebsd.org/cgi/man.cgi?query=reallocarray>
    size_t total;
    if mi_unlikely (mi_count_size_overflow(count, size, &total)) {
        errno = EOVERFLOW;
        return NULL;
    }
    void *newp = mi_realloc(p, total);
    if (newp == NULL) {
        errno = ENOMEM;
    }
    return newp;
}

mi_decl_nodiscard int mi_reallocarr(void *ptrp, size_t count, size_t size)
    mi_attr_noexcept { // NetBSD <https://man.netbsd.org/reallocarr.3>
    mi_assert(size != 0);
    mi_assert(ptrp != NULL);
    if (ptrp == NULL || size == 0) {
        return (errno = EINVAL);
    }
    size_t total;
    if mi_unlikely (mi_count_size_overflow(count, size, &total)) {
        return (errno = EOVERFLOW);
    }
    void **op = (void **)ptrp;
    if (total == 0) {
        mi_free(*op);
        *op = NULL;
        return 0;
    } else {
        void *newp = mi_realloc(*op, total);
        if (newp == NULL) {
            return (errno = ENOMEM);
        }
        *op = newp;
        return 0;
    }
}
