#define IKNOWWHATIMDOING /* cancel warning for default allocation functions */
#include "datakit.h"

/***** Global datakit configuration *****/
datakitConfig datakitConfigMemory__ = {.localMalloc = malloc,
                                       .localCalloc = calloc,
                                       .localMemalign = posix_memalign,
                                       .localRealloc = realloc,
                                       .localReallocSlate = realloc,
                                       .localFree = free};

bool datakitConfigSet(datakitConfig *conf) {
    datakitConfigMemory__ = *conf;
    return true;
}
