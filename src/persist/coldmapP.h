#pragma once

/* coldmapP - a coldmap whose changes are logged to a persistCtx
 *
 * Every change is logged before it is made, so a refused log changes
 * nothing (PERSIST_LOG_REFUSED); one that would change nothing is not
 * logged (PERSIST_LOG_UNCHANGED).  Reads go through coldmapGetRawP and
 * the coldmap API, with the caller's mflexState.  Open replays the WAL
 * over the last snapshot; compaction writes a new snapshot. */

#include "../coldmap.h"
#include "persistCtx.h"

/* @handle: fixed: its address holds from creation to free */
typedef struct coldmapP coldmapP;

/* A new, empty persistent coldmap (config NULL: the defaults) */
coldmapP *coldmapNewP(persistCtx *ctx, multimapElements elementsPerEntry,
                      const coldmapConfig *config);
/* A new persistent coldmap of 'rows' (as coldmapNewFromDump, consumed) */
coldmapP *coldmapNewFromDumpP(persistCtx *ctx,
                              multimapElements elementsPerEntry,
                              const coldmapConfig *config, flex *rows);
/* The coldmap the context's files hold */
coldmapP *coldmapOpenP(persistCtx *ctx);
void coldmapCloseP(coldmapP *c);

/* The coldmap itself, for reads (changes through it are not logged) */
const coldmap *coldmapGetRawP(const coldmapP *c);
size_t coldmapCountP(const coldmapP *c);

/* As coldmapInsert; 'replaced' (may be NULL): whether a row was replaced */
persistLogResult coldmapInsertP(coldmapP *c, const databox *elements[],
                                bool *replaced);
/* As coldmapDelete: a key with no row logs nothing */
persistLogResult coldmapDeleteP(coldmapP *c, const databox *key);
/* As coldmapInsertMany, one op: rows that do not ascend are refused;
 * 'replaced' (may be NULL): rows that replaced one */
persistLogResult coldmapInsertManyP(coldmapP *c, const flex *rows,
                                    size_t *replaced);
/* As coldmapDeleteMany, one op: keys that do not ascend are refused; the
 * batch is logged as given (a key with no row is skipped on replay), and
 * an empty batch or coldmap logs nothing.  'deleted' (may be NULL). */
persistLogResult coldmapDeleteManyP(coldmapP *c, const flex *keys,
                                    size_t *deleted);
/* As coldmapDeleteRange, one op (a range holding no row logs nothing);
 * 'deleted' (may be NULL): rows deleted */
persistLogResult coldmapDeleteRangeP(coldmapP *c, const databox *lo,
                                     const databox *hi, size_t *deleted);

bool coldmapSyncP(coldmapP *c);
bool coldmapCompactP(coldmapP *c);
void coldmapGetStatsP(const coldmapP *c, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int coldmapPTest(int argc, char *argv[]);
#endif
