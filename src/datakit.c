#define IKNOWWHATIMDOING /* cancel warning for default allocation functions */
#include "datakit.h"

/* The platform allocator's usable-size query, where it has one */
#if defined(__APPLE__)
#include <malloc/malloc.h>
static size_t datakitUsableSize_(const void *ptr) {
    return malloc_size(ptr);
}
#define DATAKIT_USABLE_SIZE datakitUsableSize_
#elifdef __linux__
#include <malloc.h>
static size_t datakitUsableSize_(const void *ptr) {
    return malloc_usable_size((void *)ptr);
}
#define DATAKIT_USABLE_SIZE datakitUsableSize_
#elifdef __FreeBSD__
#include <malloc_np.h>
static size_t datakitUsableSize_(const void *ptr) {
    return malloc_usable_size(ptr);
}
#define DATAKIT_USABLE_SIZE datakitUsableSize_
#else
#define DATAKIT_USABLE_SIZE nullptr
#endif

/***** Global datakit configuration *****/
datakitConfig datakitConfigMemory__ = {.localMalloc = malloc,
                                       .localCalloc = calloc,
                                       .localMemalign = posix_memalign,
                                       .localRealloc = realloc,
                                       .localReallocSlate = realloc,
                                       .localFree = free,
                                       .localUsableSize = DATAKIT_USABLE_SIZE};

bool datakitConfigSet(const datakitConfig *conf) {
    /* Every allocation path calls through these: a missing one would
     * crash at its first use, so the config is refused whole instead */
    if (!conf || !conf->localCalloc || !conf->localRealloc ||
        !conf->localReallocSlate || !conf->localFree || !conf->localMalloc ||
        !conf->localMemalign) {
        return false;
    }

    datakitConfigMemory__ = *conf;
    return true;
}

#ifdef DATAKIT_TEST
#include "ctest.h"

int datakitTest(int, char *[]) {
    int err = 0;

    TEST("datakitConfigSet refuses a config missing an allocator, changing "
         "nothing; the usable-size query is optional") {
        const datakitConfig saved = datakitConfigMemory__;
#define DK_CONFIG_REFUSED_WITHOUT_(field)                                      \
    do {                                                                       \
        datakitConfig c = saved;                                               \
        c.field = nullptr;                                                     \
        if (datakitConfigSet(&c) ||                                            \
            memcmp(&datakitConfigMemory__, &saved, sizeof(saved)) != 0) {      \
            ERR("a config without " #field " was taken");                      \
        }                                                                      \
    } while (0)
        DK_CONFIG_REFUSED_WITHOUT_(localCalloc);
        DK_CONFIG_REFUSED_WITHOUT_(localRealloc);
        DK_CONFIG_REFUSED_WITHOUT_(localReallocSlate);
        DK_CONFIG_REFUSED_WITHOUT_(localFree);
        DK_CONFIG_REFUSED_WITHOUT_(localMalloc);
        DK_CONFIG_REFUSED_WITHOUT_(localMemalign);
#undef DK_CONFIG_REFUSED_WITHOUT_
        if (datakitConfigSet(nullptr)) {
            ERR("a NULL config was taken");
        }

        datakitConfig noUsable = saved;
        noUsable.localUsableSize = nullptr;
        const bool took = datakitConfigSet(&noUsable);
        const bool reports = zusablesize(&err) != 0;
        datakitConfigSet(&saved);
        if (!took || reports) {
            ERR("without a usable-size query: taken %d, a size reported %d",
                took, reports);
        }
    }

    TEST_FINAL_RESULT;
}
#endif
