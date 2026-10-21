#define IKNOWWHATIMDOING /* we assign allocator fns; don't warn on libc names  \
                          */
#include "memshardConfig.h"

#include "mimalloc.h"

/* memshard's mi_* signatures line up 1:1 with datakitConfig's function-pointer
 * members (notably mi_posix_memalign matches localMemalign's
 * int(void**, size_t, size_t)). memshard allocates via mmap directly and never
 * calls back into the z* allocator, so routing z* -> memshard is
 * recursion-free. */
datakitConfig datakitMemshardConfig(void) {
    datakitConfig c = {
        .localMalloc = mi_malloc,
        .localCalloc = mi_calloc,
        .localRealloc = mi_realloc,
        .localReallocSlate =
            mi_realloc, /* matches default (realloc==realloc) */
        .localFree = mi_free,
        .localMemalign = mi_posix_memalign,
        .localUsableSize = mi_usable_size,
    };
    return c;
}

bool datakitUseMemshard(void) {
    datakitConfig c = datakitMemshardConfig();
    return datakitConfigSet(&c);
}
