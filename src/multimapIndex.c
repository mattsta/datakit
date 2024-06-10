#include "multimapIndex.h"
#include "flexCapacityManagement.h"

struct multimapIndex {
    multimap *forward;
    multimap *reverse;
    multimap *keyDictionary;
    bool releaseDictionary;
    bool uniqueKeys;
    bool uniqueValues;
};

multimapIndex *multimapIndexNew(const bool compress, const bool uniqueKeys,
                                const bool uniqueValues,
                                const flexCapSizeLimit sizeLimit) {
    multimapIndex *mi = zmalloc(sizeof(*mi));

    mi->uniqueKeys = uniqueKeys;
    mi->uniqueValues = uniqueValues;

    mi->forward = multimapNewConfigure(2, uniqueKeys, compress, sizeLimit);
    mi->reverse = multimapNewConfigure(2, uniqueValues, compress, sizeLimit);
    return mi;
}

void multimapIndexFree(multimapIndex *const mi) {
    if (mi) {
        multimapFree(mi->forward);
        multimapFree(mi->reverse);
        if (mi->releaseDictionary) {
            multimapFree(mi->keyDictionary);
        }
        zfree(mi);
    }
}

bool multimapIndexInsert(multimapIndex *mi, const databox *scorebox,
                         const databox *valbox) {
    /* Steps:
     *   - If 'valbox' exists in 'mi->reverse'
     *     - update score in 'mi->reverse'
     *     - delete score+value in 'mi->forward'
     *     - insert into 'mi->forward'
     *     - return true
     *   - If 'valbox' doesn't exists
     *     - insert into 'mi->reverse'
     *     - insert into 'mi->forward'
     *     - return false
     */

    /* NEED:
     *   - Lookup with dictionary
     *   - Insert with dictionary
     *   - Get range with dictionary
     *   - Delete range with dictionary */

    databox foundscore = {{0}};
    databox *foundscorebox[1] = {&foundscore};
    if (multimapLookup(mi->reverse, valbox, foundscorebox)) {
        const databox *currentScoreVal[2] = {&foundscore, valbox};
        const databox *newScoreVal[2] = {scorebox, valbox};

        /* Update reverse map to point to new score */
        multimapEntryReplace(&mi->reverse, valbox, &scorebox);

        /* Remove current score -> memeber mapping */
        multimapDeleteFullWidth(&mi->forward, currentScoreVal);

        /* Add new score -> member mapping */
        multimapInsert(&mi->forward, newScoreVal);
    } else {
    }

    return false;
}
