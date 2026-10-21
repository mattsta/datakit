#define IKNOWWHATIMDOING /* cancel warning for default allocation functions */
#include "datakit.h"

/* The platform allocator's usable-size query, where it has one */
#if defined(__APPLE__)
#include <malloc/malloc.h>
static size_t datakitUsableSize_(const void *ptr) {
    return malloc_size(ptr);
}
#define DATAKIT_USABLE_SIZE datakitUsableSize_
#elif defined(__linux__)
#include <malloc.h>
static size_t datakitUsableSize_(const void *ptr) {
    return malloc_usable_size((void *)ptr);
}
#define DATAKIT_USABLE_SIZE datakitUsableSize_
#elif defined(__FreeBSD__)
#include <malloc_np.h>
static size_t datakitUsableSize_(const void *ptr) {
    return malloc_usable_size(ptr);
}
#define DATAKIT_USABLE_SIZE datakitUsableSize_
#else
#define DATAKIT_USABLE_SIZE NULL
#endif

/***** Global datakit configuration *****/
datakitConfig datakitConfigMemory__ = {.localMalloc = malloc,
                                       .localCalloc = calloc,
                                       .localMemalign = posix_memalign,
                                       .localRealloc = realloc,
                                       .localReallocSlate = realloc,
                                       .localFree = free,
                                       .localUsableSize = DATAKIT_USABLE_SIZE};

bool datakitConfigSet(datakitConfig *conf) {
    datakitConfigMemory__ = *conf;
    return true;
}
