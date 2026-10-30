#pragma once

/* multitierP - a multitier whose changes are logged to a persistCtx
 *
 * A Set is logged before it is made (a refused log changes nothing), a
 * Delete of a present key likewise, and rows a sink takes are logged as
 * they leave (by any call: a Get or a Set can push rows out of the cold
 * level).  With the config's logUses, each Get and Flush is logged before
 * it is made too, so a replay repeats every move between levels and an
 * open after a crash restores the exact state; without, moves log
 * nothing, and an open after a crash has every row on the last snapshot's
 * levels and use history plus the logged writes.  Close writes a
 * snapshot of the whole state (use history included), so an open after a
 * close is exact either way.  The caller's sink receives rows before their
 * log record is written: a crash between the two, or a refused record
 * (multitierSinkLogRefusedP counts them), leaves the rows in the files
 * too, so they come back on the next open - held twice, never lost. */

#include "../multitier.h"
#include "persistCtx.h"

/* @handle: fixed: its address holds from creation to free */
typedef struct multitierP multitierP;

/* config NULL: the defaults; its sink receives the rows leaving */
multitierP *multitierNewP(persistCtx *ctx, const multitierConfig *config);
/* The multitier the context's files hold; 'sink' as the config's */
multitierP *multitierOpenP(persistCtx *ctx, multitierSinkFn *sink,
                           void *sinkCtx);
void multitierCloseP(multitierP *t);
/* The caller's sink (NULL: rows leaving are logged and dropped) */
void multitierSetSinkP(multitierP *t, multitierSinkFn *sink, void *sinkCtx);

/* The multitier itself, for its stats and counts */
const multitier *multitierGetRawP(const multitierP *t);
size_t multitierCountP(const multitierP *t);
/* Sink records the WAL refused (each leaves its rows in the files) */
uint64_t multitierSinkLogRefusedP(const multitierP *t);

/* As multitierSet; 'isNew' (may be NULL): whether the key was new */
persistLogResult multitierSetP(multitierP *t, const databox *key,
                               const databox *value, bool *isNew);
/* As multitierDelete: a key with no row logs nothing */
persistLogResult multitierDeleteP(multitierP *t, const databox *key);
/* Reads: as multitierGet / multitierPeek (a Get logs the rows it pushes
 * out to the sink, and itself first with logUses: a refused record leaves
 * it a Peek) */
bool multitierGetP(multitierP *t, const databox *key, databox *value);
bool multitierPeekP(multitierP *t, const databox *key, databox *value);
void multitierFlushP(multitierP *t);

bool multitierSyncP(multitierP *t);
bool multitierCompactP(multitierP *t);
void multitierGetStatsP(const multitierP *t, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int multitierPTest(int argc, char *argv[]);
#endif
