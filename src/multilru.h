#pragma once

typedef struct multilru multilru;
typedef size_t multilruPtr;

multilru *multilruNew(void);
multilru *multilruNewWithLevels(size_t maxLevels);
multilru *multilruNewWithLevelsCapacity(size_t maxLevels, size_t startCapacity);
void multilruFree(multilru *mlru);
size_t multilruBytes(const multilru *mlru);
size_t multilruCount(const multilru *mlru);

multilruPtr multilruInsert(multilru *mlru);
void multilruIncrease(multilru *mlru, const multilruPtr currentPtr);
bool multilruRemoveMinimum(multilru *mlru, multilruPtr *atomRef);
void multilruDelete(multilru *mlru, const multilruPtr ptr);

void multilruGetNLowest(multilru *mlru, multilruPtr N[], size_t n);
void multilruGetNHighest(multilru *mlru, multilruPtr N[], size_t n);

#ifdef DATAKIT_TEST
void multilruRepr(const multilru *mlru);
int multilruTest(int argc, char *argv[]);
#endif
