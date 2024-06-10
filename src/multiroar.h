#pragma once

#include "databox.h"
#include "multimap.h"

typedef struct multiroar multiroar;

multiroar *multiroarBitNew(void);
multiroar *multiroarValueNew(uint8_t bitWidth, size_t rows, size_t cols);
void multiroarFree(multiroar *r);

multiroar *multiroarDuplicate(multiroar *r);

/* Modify 'r' in place */
bool multiroarBitSet(multiroar *r, size_t position);
bool multiroarBitSetGetPrevious(multiroar *r, size_t position);
void multiroarBitSetRange(multiroar *r, size_t start, size_t extent);
bool multiroarRemove(multiroar *r, size_t position);

bool multiroarValueSet(multiroar *r, size_t position, databox *value);
bool multiroarValueSetGetPrevious(multiroar *r, size_t position, databox *value,
                                  databox *overwritten);
bool multiroarValueRemoveGetRemoved(multiroar *r, size_t position,
                                    databox *removed);

void multiroarXor(multiroar *r, multiroar *b);
void multiroarAnd(multiroar *r, multiroar *b);
void multiroarOr(multiroar *r, multiroar *b);
void multiroarNot(multiroar *r);

/* Return a new multiroar */
multiroar *multiroarNewXor(multiroar *r, multiroar *b);
multiroar *multiroarNewAnd(multiroar *r, multiroar *b);
multiroar *multiroarNewOr(multiroar *r, multiroar *b);
multiroar *multiroarNewNot(multiroar *r);

#ifdef DATAKIT_TEST
int multiroarTest(int argc, char *argv[]);
#endif /* DATAKIT_TEST */
