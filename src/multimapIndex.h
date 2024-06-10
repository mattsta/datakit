#pragma once

#include "multimap.h"
#include <stdbool.h>

typedef struct multimapIndex multimapIndex;

multimapIndex *multimapIndexNew(bool compress, bool uniqueKeys,
                                bool uniqueValues, flexCapSizeLimit sizeLimit);
void multimapIndexFree(multimapIndex *mi);

bool multimapIndexInsert(multimapIndex *mi, const databox *scorebox,
                         const databox *valbox);

bool multimapIndexLookupScoreForMember(multimapIndex *mi, const databox *valbox,
                                       databox *scorebox);
bool multimapIndexLookupMembersForScoreRange(multimapIndex *mi,
                                             const databox *scoreboxLow,
                                             const databox *scoreboxHigh,
                                             flex **result);

bool multimapIndexRemoveByMember(multimapIndex *mi, const databox *valbox);
bool multimapIndexRemoveByScore(multimapIndex *mi, const databox *scorebox);
bool multimapIndexRemoveByScoreRange(multimapIndex *mi,
                                     const databox *scoreboxLow,
                                     const databox *scoreboxHigh);
