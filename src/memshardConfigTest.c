/* Validates that datakit's global z* allocator can be routed through memshard
 * via the opt-in adapter. Standalone target; build under the dev preset (ASAN).
 *
 * Proof that z* actually hits memshard: mi_usable_size() only returns a sane
 * value for pointers owned by memshard, so a zmalloc'd pointer passing
 * mi_usable_size(p) >= requested confirms the routing. */
#define IKNOWWHATIMDOING
#include "memshardConfig.h"
#include "datakit.h"
#include "mimalloc.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    assert(datakitUseMemshard());

    /* zmalloc (zero-filled by datakit convention) routed via memshard */
    void *p = zmalloc(123);
    assert(p);
    assert(mi_usable_size(p) >= 123); /* owned by memshard */
    zfree(p);

    /* zcalloc -> mi_calloc, zeroed */
    uint64_t *c = (uint64_t *)zcalloc(100, sizeof(uint64_t));
    assert(c);
    for (int i = 0; i < 100; i++) {
        assert(c[i] == 0);
    }

    /* zrealloc -> mi_realloc, preserves + still memshard-owned */
    c[0] = 0xDEADBEEF;
    c = (uint64_t *)zrealloc(c, 200 * sizeof(uint64_t));
    assert(c && c[0] == 0xDEADBEEF);
    assert(mi_usable_size(c) >= 200 * sizeof(uint64_t));
    zfree(c);

    /* zmemalign -> mi_posix_memalign */
    void *a = NULL;
    assert(zmemalign(&a, 64, 1000) == 0);
    assert(a && ((uintptr_t)a % 64) == 0);
    assert(mi_usable_size(a) >= 1000);
    zfree(a);

    printf("memshard-as-datakit-allocator: ALL PASSED!\n");
    return 0;
}
