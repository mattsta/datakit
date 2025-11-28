/* Minimal test file to include all struct definitions for pahole analysis */
#include "perf.h"
#include "varint.h"
#include "varintAdaptive.h"
#include "varintBitmap.h"
#include "varintDelta.h"
#include "varintDict.h"
#include "varintFOR.h"
#include "varintFloat.h"
#include "varintPFOR.h"

int main(void) {
    /* Just reference the structs so compiler includes them */
    sizeof(varintFORMeta);
    sizeof(varintPFORMeta);
    sizeof(varintFloatMeta);
    sizeof(varintAdaptiveDataStats);
    sizeof(varintAdaptiveMeta);
    sizeof(varintBitmapStats);
    sizeof(varintBitmap);
    sizeof(varintBitmapIterator);
    sizeof(varintDict);
    sizeof(varintDictStats);
    sizeof(perfStateGlobal);
    sizeof(perfStateStat);
    sizeof(perfState);
    return 0;
}
