/*-------------------------------------------------------------------------
 *
 * lsm.c
 *      An LSM-tree index access method for PostgreSQL, built out of B-trees.
 *
 * WHY THIS EXISTS
 *
 * A B-tree is a wonderful read structure and a mediocre random-write one.
 * Inserting keys in random order into a large B-tree means each insert dirties
 * a leaf page chosen at random from a working set far bigger than shared
 * buffers, so almost every insert costs a read I/O to fetch the leaf and a
 * later write I/O to flush it.  Throughput falls off a cliff once the index
 * stops fitting in cache.
 *
 * An LSM tree fixes this by never doing random writes to the big structure.
 * Writes land in a small, hot, in-cache structure; when it fills up, it is
 * merged into the big one *sequentially*, in key order, which is orders of
 * magnitude cheaper per key.
 *
 * THE DESIGN HERE
 *
 * Rather than inventing a new on-disk format, each level of the LSM *is* a
 * B-tree.  For every `lsm` index we maintain three auxiliary btree relations:
 *
 *      base      - a large B-tree holding the bulk of the data
 *      top[0]    - a small B-tree
 *      top[1]    - a second small B-tree
 *
 * The lsm index relation itself stores nothing; it is a handle that owns the
 * three levels and routes to them.  The first draft had the lsm relation *be*
 * the base btree, which is tidier -- until nbtree internals reject it:
 * _bt_allequalimage() errors with "unexpected non-btree AM" the moment it sees
 * a relation whose relam is not btree.  Giving every level its own real btree
 * relation removes that entire class of problem.
 *
 * Inserts go to whichever top is active.  When the active top exceeds
 * lsm.top_index_size, we flip `active` to the other top and a background
 * worker merges the full one into base with a sequential in-key-order scan,
 * then truncates it.  Two tops (not one) so that inserts never have to block
 * on a merge.  Searches read base and both tops and union the results.
 *
 * THE TRICK THAT MAKES IT WORK
 *
 * The top indexes are ordinary B-tree indexes on the same table, so VACUUM
 * cleans their dead TIDs for free.  But the executor inserts into every index
 * of a relation, which would double-write every tuple.  So the tops are
 * created as *partial indexes with the predicate `false`*: ExecInsertIndexTuples
 * evaluates the predicate, gets false, and skips them; the planner sees a
 * predicate that can never be satisfied and never picks them on their own.
 * Only lsm_insert() writes to them, explicitly, into the active one.
 *
 * WHAT IS NOT HERE
 *
 * See BLUEPRINT.md.  The short version: no ordered scans (amcanorder = false),
 * no index-only scans, and no bloom filter on the tops.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "funcapi.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

/* ---------------------------------------------------------------- GUCs */
static int  lsm_top_index_size_kb = 1024;   /* flip the active top above this */
static int  lsm_max_indexes       = 128;    /* shmem dictionary capacity      */
static int  lsm_merge_interval_ms = 500;    /* how often the worker looks     */
static bool lsm_enabled           = true;
static char *lsm_merge_database   = NULL;

/* ------------------------------------------------- shared memory state */
typedef struct LsmEntry
{
    Oid         base;                   /* hash key: the lsm index's relid    */
    Oid         heap;
    Oid         basex;                  /* auxiliary btree holding the bulk    */
    Oid         top[2];
    int         active;                 /* which top is taking inserts        */
    bool        merge_pending;          /* full top waiting for the worker    */
    int         merging;                /* which top is being merged, or -1   */
    BlockNumber base_blocks;            /* cached sizes, for cost estimation  */
    BlockNumber top_blocks;
    double      base_tuples;
    uint64      n_merges;
    uint64      n_merged_tuples;
    uint64      n_inserts;
    double      last_merge_ms;
} LsmEntry;

typedef struct LsmShared
{
    LWLock     *lock;
    int         n_entries;
} LsmShared;

static LsmShared  *lsm_shared = NULL;
static HTAB       *lsm_dict   = NULL;

static shmem_request_hook_type  prev_shmem_request_hook  = NULL;
static shmem_startup_hook_type  prev_shmem_startup_hook  = NULL;

/* ------------------------------------------------------- declarations */
PG_FUNCTION_INFO_V1(lsm_handler);
PG_FUNCTION_INFO_V1(lsm_merge);
PG_FUNCTION_INFO_V1(lsm_stats);
PG_FUNCTION_INFO_V1(lsm_top_sizes);

void _PG_init(void);
PGDLLEXPORT void lsm_merger_main(Datum arg);

static void lsm_shmem_request(void);
static void lsm_shmem_startup(void);
static LsmEntry *lsm_get_entry(Oid base, Oid heap, bool create);
static void lsm_do_merge(LsmEntry *entry, int which);
static void lsm_truncate_index(Relation index);
static Oid  lsm_create_level(Relation heap, Relation base, int level, bool populate);
static void lsm_lookup_levels(Relation parent, Oid *out);

/* ------------------------------------------------------------ helpers */

/*
 * Level names are derived from the parent index OID, so after a server restart
 * the whole mapping can be rebuilt by name lookup with no extra catalog.
 * level 0 = base, levels 1 and 2 = the two tops.
 */
static char *
lsm_level_name(Oid parent, int level)
{
    return (level == 0) ? psprintf("lsm_base_%u", parent)
                        : psprintf("lsm_top_%u_%d", parent, level - 1);
}

static Oid
lsm_level_oid(Relation parent, int level)
{
    return get_relname_relid(lsm_level_name(RelationGetRelid(parent), level),
                             RelationGetNamespace(parent));
}

/* out[0] = base, out[1] = top0, out[2] = top1 */
static void
lsm_lookup_levels(Relation parent, Oid *out)
{
    int i;
    for (i = 0; i < 3; i++)
        out[i] = lsm_level_oid(parent, i);
}

/*
 * Find (or create) the shared-memory entry for an lsm index.  On a cold cache
 * -- e.g. right after a server restart -- the top index OIDs are recovered by
 * name, which is why lsm_level_name() is deterministic.
 */
static LsmEntry *
lsm_get_entry(Oid base, Oid heap, bool create)
{
    LsmEntry *entry;
    bool      found;

    if (!lsm_dict)
        elog(ERROR, "lsm: shared memory not initialised; add 'lsm' to shared_preload_libraries");

    LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
    entry = (LsmEntry *) hash_search(lsm_dict, &base,
                                     create ? HASH_ENTER : HASH_FIND, &found);
    if (entry && !found)
    {
        entry->heap          = heap;
        entry->basex         = InvalidOid;
        entry->top[0]        = InvalidOid;
        entry->top[1]        = InvalidOid;
        entry->active        = 0;
        entry->merge_pending = false;
        entry->merging       = -1;
        entry->base_blocks   = 0;
        entry->top_blocks    = 0;
        entry->base_tuples   = 0;
        entry->n_merges      = 0;
        entry->n_merged_tuples = 0;
        entry->n_inserts     = 0;
        entry->last_merge_ms = 0;
        lsm_shared->n_entries++;
    }
    LWLockRelease(lsm_shared->lock);
    return entry;
}

/* ------------------------------------------------------------- ambuild */

/*
 * Create one level: a real B-tree on the same heap and columns, but with the
 * constant-false predicate that keeps the executor from writing to it.
 *
 * `populate` controls the initial build.  The base level is built with the
 * predicate stripped, so CREATE INDEX on an already-populated table sorts the
 * existing rows straight into it; the tops are built with the predicate in
 * place and therefore come out empty.
 */
static Oid
lsm_create_level(Relation heap, Relation base, int level, bool populate)
{
    IndexInfo  *idxinfo;
    List       *colnames = NIL;
    Oid        *opclasses;
    Oid        *collations;
    int16      *coloptions;
    int         natts = IndexRelationGetNumberOfKeyAttributes(base);
    Oid         levoid;
    char       *name = lsm_level_name(RelationGetRelid(base), level);
    int         i;

    idxinfo = BuildIndexInfo(base);
    /* THE trick: predicate `false` => ExecInsertIndexTuples skips this index */
    idxinfo->ii_Predicate = list_make1(makeBoolConst(false, false));
    idxinfo->ii_PredicateState = NULL;
    idxinfo->ii_Unique = false;
    idxinfo->ii_ReadyForInserts = false;

    opclasses  = (Oid *) palloc(natts * sizeof(Oid));
    collations = (Oid *) palloc(natts * sizeof(Oid));
    coloptions = (int16 *) palloc(natts * sizeof(int16));

    for (i = 0; i < natts; i++)
    {
        /* Each level is a B-tree, so the top needs the *btree* opclass for the
         * column type, not the lsm one the parent index was declared with. */
        opclasses[i]  = GetDefaultOpClass(TupleDescAttr(RelationGetDescr(base), i)->atttypid,
                                          BTREE_AM_OID);
        collations[i] = base->rd_indcollation[i];
        coloptions[i] = base->rd_indoption[i];
        colnames = lappend(colnames,
                           makeString(pstrdup(NameStr(TupleDescAttr(RelationGetDescr(base), i)->attname))));
    }

    levoid = index_create(heap,
                          name,
                          InvalidOid, InvalidOid, InvalidOid, InvalidRelFileNumber,
                          idxinfo,
                          colnames,
                          BTREE_AM_OID,
                          base->rd_rel->reltablespace,
                          collations,
                          opclasses,
                          coloptions,
                          (Datum) 0,
                          INDEX_CREATE_SKIP_BUILD,
                          0,
                          true,
                          true,
                          NULL);

    {
        Relation lev = index_open(levoid, AccessExclusiveLock);
        IndexInfo *buildinfo = BuildIndexInfo(base);

        buildinfo->ii_Unique = false;
        buildinfo->ii_ReadyForInserts = false;
        if (!populate)
        {
            /* keep the false predicate -> the build inserts nothing */
            buildinfo->ii_Predicate = list_make1(makeBoolConst(false, false));
            buildinfo->ii_PredicateState = NULL;
        }
        index_build(heap, lev, buildinfo, false, false);
        index_close(lev, NoLock);
    }
    return levoid;
}

static IndexBuildResult *
lsm_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result;
    LsmEntry   *entry;
    Oid         levels[3];
    Relation    basex;
    double      ntuples;
    BlockNumber nblocks;

    /* base level: built from the existing heap rows, sorted, in one pass */
    levels[0] = lsm_create_level(heap, index, 0, true);
    levels[1] = lsm_create_level(heap, index, 1, false);
    levels[2] = lsm_create_level(heap, index, 2, false);

    basex = index_open(levels[0], AccessShareLock);
    ntuples = basex->rd_rel->reltuples;
    nblocks = RelationGetNumberOfBlocks(basex);
    index_close(basex, AccessShareLock);
    if (ntuples < 0)
        ntuples = 0;

    entry = lsm_get_entry(RelationGetRelid(index), RelationGetRelid(heap), true);
    LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
    entry->basex       = levels[0];
    entry->top[0]      = levels[1];
    entry->top[1]      = levels[2];
    entry->active      = 0;
    entry->base_blocks = nblocks;
    entry->base_tuples = ntuples;
    entry->top_blocks  = 0;
    LWLockRelease(lsm_shared->lock);

    elog(DEBUG1, "lsm: built index %u -> base %u, tops %u/%u",
         RelationGetRelid(index), levels[0], levels[1], levels[2]);

    /* The lsm relation itself stays empty; report the base's counts. */
    result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
    result->heap_tuples  = ntuples;
    result->index_tuples = ntuples;
    return result;
}

static void
lsm_buildempty(Relation index)
{
    /* nothing to initialise: the lsm relation never stores tuples */
}

/* ------------------------------------------------------------ aminsert */

static bool
lsm_insert(Relation index, Datum *values, bool *isnull,
           ItemPointer ht_ctid, Relation heap,
           IndexUniqueCheck checkUnique, bool indexUnchanged,
           IndexInfo *indexInfo)
{
    LsmEntry   *entry;
    Oid         topoid;
    Relation    top;
    bool        res;
    int         active;
    BlockNumber nblocks;

    entry = lsm_get_entry(RelationGetRelid(index), RelationGetRelid(heap), true);

    LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
    if (!OidIsValid(entry->basex))
    {
        Oid levels[3];
        LWLockRelease(lsm_shared->lock);
        lsm_lookup_levels(index, levels);   /* recover mapping after restart */
        LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
        entry->basex  = levels[0];
        entry->top[0] = levels[1];
        entry->top[1] = levels[2];
    }
    /*
     * lsm.enabled = off routes every insert straight into the base level, so
     * the same index degrades into an ordinary B-tree.  That is the A/B switch
     * the benchmarks use: identical schema, identical data, one GUC.
     */
    active = lsm_enabled ? entry->active : -1;
    topoid = (active >= 0) ? entry->top[active] : entry->basex;
    entry->n_inserts++;
    LWLockRelease(lsm_shared->lock);

    if (!OidIsValid(topoid))
        return false;

    top = index_open(topoid, RowExclusiveLock);
    res = index_insert(top, values, isnull, ht_ctid, heap,
                       UNIQUE_CHECK_NO, false, BuildIndexInfo(top));
    nblocks = RelationGetNumberOfBlocks(top);
    index_close(top, RowExclusiveLock);

    /*
     * Flip to the other top once this one is big enough, and leave a flag for
     * the merger.  The flip is what keeps inserts off the critical path of a
     * merge: by the time the worker starts reading top[old], every new insert
     * is already landing in top[new].
     */
    entry->top_blocks = nblocks;

    if (active >= 0 &&
        (uint64) nblocks * BLCKSZ > (uint64) lsm_top_index_size_kb * 1024)
    {
        LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
        if (entry->active == active && !entry->merge_pending)
        {
            entry->active        = 1 - active;
            entry->merging       = active;
            entry->merge_pending = true;
            elog(DEBUG1, "lsm: top[%d] of index %u full (%u blocks), flipping to top[%d]",
                 active, RelationGetRelid(index), nblocks, 1 - active);
        }
        LWLockRelease(lsm_shared->lock);
    }
    return res;
}

/* -------------------------------------------------------------- scans */

/*
 * Open/close a level relation against the *transaction* resource owner.
 *
 * index_open() charges the relcache reference to CurrentResourceOwner, and the
 * executor may run amendscan under a different owner than ambeginscan -- which
 * fails with "relcache reference lsm_base_NNNNN is not owned by resource owner
 * Portal".  Pinning both ends to TopTransactionResourceOwner makes the pairing
 * hold for the whole transaction regardless of how many portals come and go.
 */
static Relation
lsm_level_open(Oid oid)
{
    ResourceOwner saved = CurrentResourceOwner;
    Relation rel;

    CurrentResourceOwner = TopTransactionResourceOwner;
    rel = index_open(oid, AccessShareLock);
    CurrentResourceOwner = saved;
    return rel;
}

static void
lsm_level_close(Relation rel)
{
    ResourceOwner saved = CurrentResourceOwner;

    CurrentResourceOwner = TopTransactionResourceOwner;
    /*
     * NoLock: keep the AccessShareLock until end of transaction.
     *
     * A merge swaps the top level's relfilenode.  If a scan dropped its lock at
     * endscan, a merge could swap the file out from under a later statement in
     * the same transaction that still holds a stale Relation pointer -- which
     * showed up as intermittent backend crashes under concurrent load.  Holding
     * the lock to commit makes the merge wait instead.  The cost is that a
     * long-running transaction delays merges; that is the right trade.
     */
    index_close(rel, NoLock);
    CurrentResourceOwner = saved;
}

/*
 * A scan over an lsm index is three B-tree scans stitched together: base,
 * top[0], top[1].  Because amcanorder is false the executor imposes no
 * ordering requirement, so "stitched together" can mean "drain one, then the
 * next", which is far simpler than a k-way merge.
 */
typedef struct LsmScanState
{
    IndexScanDesc sub[3];       /* base, top0, top1 */
    Relation      subrel[3];
    int           current;
    int           nsub;
    Relation      heap;
} LsmScanState;

/*
 * The level relations are opened here, not in amrescan.
 *
 * index_open() registers the relcache reference with the *current* resource
 * owner, and amrescan can be called under a different one than amendscan --
 * which produces "relcache reference ... is not owned by resource owner Portal"
 * at the end of a re-scanned nested-loop join.  ambeginscan and amendscan are
 * guaranteed to be paired within one resource owner, so open and close there.
 */
static IndexScanDesc
lsm_beginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan;
    LsmScanState *st;
    LsmEntry     *entry;
    Oid           levels[3];
    int           i;

    MemoryContext oldcxt;

    scan = RelationGetIndexScan(index, nkeys, norderbys);
    st = (LsmScanState *) palloc0(sizeof(LsmScanState));

    entry = lsm_dict ? lsm_get_entry(RelationGetRelid(index), InvalidOid, false) : NULL;
    if (entry && OidIsValid(entry->basex))
    {
        levels[0] = entry->basex;
        levels[1] = entry->top[0];
        levels[2] = entry->top[1];
    }
    else
        lsm_lookup_levels(index, levels);

    /*
     * Build the sub-scans in the transaction context, not whatever context
     * happens to be current.  A SubPlan re-executed thousands of times resets
     * short-lived contexts between calls; an IndexScanDesc allocated in one of
     * those gets recycled underneath us and _bt_first() then reads a garbage
     * parallel_scan pointer and segfaults inside _bt_parallel_seize().
     */
    oldcxt = MemoryContextSwitchTo(CurTransactionContext);
    /* base first: it holds most of the data, so most matches are found there */
    for (i = 0; i < 3; i++)
    {
        if (!OidIsValid(levels[i]))
            continue;
        st->subrel[st->nsub] = lsm_level_open(levels[i]);
        st->sub[st->nsub] = btbeginscan(st->subrel[st->nsub], nkeys, norderbys);
        st->sub[st->nsub]->parallel_scan = NULL;   /* never parallel */
        st->nsub++;
    }
    MemoryContextSwitchTo(oldcxt);

    scan->opaque = st;
    return scan;
}

static void
lsm_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
           ScanKey orderbys, int norderbys)
{
    LsmScanState *st = (LsmScanState *) scan->opaque;
    int i;

    st->current = 0;
    st->heap = scan->heapRelation;

    for (i = 0; i < st->nsub; i++)
    {
        st->sub[i]->heapRelation = scan->heapRelation;
        st->sub[i]->xs_snapshot  = scan->xs_snapshot;
        btrescan(st->sub[i], keys, nkeys, orderbys, norderbys);
    }
}

static bool
lsm_gettuple(IndexScanDesc scan, ScanDirection dir)
{
    LsmScanState *st = (LsmScanState *) scan->opaque;

    /* amcanbackward is false, so dir is always forward here */
    while (st->current < st->nsub)
    {
        if (btgettuple(st->sub[st->current], ForwardScanDirection))
        {
            scan->xs_heaptid = st->sub[st->current]->xs_heaptid;
            scan->xs_recheck = st->sub[st->current]->xs_recheck;
            return true;
        }
        st->current++;
    }
    return false;
}

static int64
lsm_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    LsmScanState *st = (LsmScanState *) scan->opaque;
    int64 total = 0;
    int   i;

    for (i = 0; i < st->nsub; i++)
        total += btgetbitmap(st->sub[i], tbm);
    return total;
}

static void
lsm_endscan(IndexScanDesc scan)
{
    LsmScanState *st = (LsmScanState *) scan->opaque;
    int i;

    for (i = 0; i < st->nsub; i++)
    {
        /*
         * btendscan, not index_endscan.  The sub-scans were created with
         * btbeginscan(), which does not take a relcache reference; index_endscan
         * would drop one that was never taken and trip the resource-owner
         * accounting.  Create and destroy through the same layer.
         */
        if (st->sub[i])
            btendscan(st->sub[i]);
        if (st->subrel[i])
            lsm_level_close(st->subrel[i]);
        st->sub[i] = NULL;
        st->subrel[i] = NULL;
    }
    st->nsub = 0;
}

/* ------------------------------------------------------------- vacuum */

/*
 * All three levels are ordinary live btree indexes on the same heap, so VACUUM
 * visits each of them on its own and removes dead TIDs there.  The lsm
 * relation holds nothing, so there is nothing here to clean.
 */
static IndexBulkDeleteResult *
lsm_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
               IndexBulkDeleteCallback callback, void *callback_state)
{
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
    return stats;
}

static IndexBulkDeleteResult *
lsm_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    return stats;
}

/*
 * Cost estimate.
 *
 * The first version delegated to btcostestimate(), which segfaulted: that
 * function reaches into btree-specific state on the IndexOptInfo and the lsm
 * relation does not have it.  genericcostestimate() is the AM-agnostic helper
 * PostgreSQL exports for exactly this situation, so use that and add what is
 * specific to an LSM:
 *
 *   - three tree descents per lookup instead of one (base + two tops)
 *   - correlation 0: an LSM is never physically clustered on the key
 *   - real page counts, since the lsm relation itself is an empty shell and
 *     pg_class would otherwise tell the planner this index costs nothing
 */
static void
lsm_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                 Cost *indexStartupCost, Cost *indexTotalCost,
                 Selectivity *indexSelectivity, double *indexCorrelation,
                 double *indexPages)
{
    IndexOptInfo *index = path->indexinfo;
    GenericCosts  costs;
    LsmEntry     *entry;

    memset(&costs, 0, sizeof(costs));

    entry = lsm_dict ? lsm_get_entry(index->indexoid, InvalidOid, false) : NULL;
    if (entry && entry->base_blocks + entry->top_blocks > 0)
    {
        index->pages  = entry->base_blocks + entry->top_blocks;
        if (entry->base_tuples > 0)
            index->tuples = entry->base_tuples + (double) entry->n_inserts;
    }
    if (index->tree_height < 0)
        index->tree_height = (index->pages > 1)
            ? (int) (log((double) index->pages) / log(100.0)) : 0;

    genericcostestimate(root, path, loop_count, &costs);

    /* two extra descents for the tops, charged as random page fetches */
    costs.indexStartupCost += 2.0 * random_page_cost;
    costs.indexTotalCost   += 2.0 * random_page_cost;

    *indexStartupCost = costs.indexStartupCost;
    *indexTotalCost   = costs.indexTotalCost;
    *indexSelectivity = costs.indexSelectivity;
    *indexCorrelation = 0.0;
    *indexPages       = costs.numIndexPages;
}

/* ------------------------------------------------------------ merging */

/*
 * Reset a B-tree relation to an empty, valid state.
 *
 * The obvious implementation -- RelationTruncate(index, 0) plus a hand-written
 * metapage -- is wrong, and wrong in an instructive way.  It leaves the index's
 * free space map intact, so the next insert makes nbtree create a new root, and
 * _bt_getbuf(P_NEW) asks the FSM for a free page, and the FSM cheerfully hands
 * back block 3 of a file that now has one block:
 *
 *      ERROR: could not read block 3 in file "base/5/24760": read only 0 of 8192
 *
 * Allocating a fresh relfilenode instead throws away the main fork, the FSM and
 * the visibility map together, which is exactly what TRUNCATE and REINDEX do.
 * The old file is unlinked at commit, so this is also crash-safe.
 */
static void
lsm_truncate_index(Relation index)
{
    Buffer  metabuf;
    Page    metapage;
    bool    allequalimage = _bt_allequalimage(index, false);

    RelationSetNewRelfilenumber(index, index->rd_rel->relpersistence);

    metabuf = ReadBuffer(index, P_NEW);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    Assert(BufferGetBlockNumber(metabuf) == BTREE_METAPAGE);

    START_CRIT_SECTION();
    metapage = BufferGetPage(metabuf);
    _bt_initmetapage(metapage, P_NONE, 0, allequalimage);
    MarkBufferDirty(metabuf);
    log_newpage_buffer(metabuf, true);
    END_CRIT_SECTION();

    UnlockReleaseBuffer(metabuf);
}

/*
 * Merge one full top index into the base.
 *
 * The read side is a full B-tree scan, which nbtree serves in key order, so
 * the inserts into base arrive sorted.  That is the entire performance
 * argument for an LSM: the base tree is only ever written in ascending key
 * order, which keeps its hot leaf set tiny no matter how big it gets.
 */
static void
lsm_do_merge(LsmEntry *entry, int which)
{
    Relation    heap, base, top;
    IndexScanDesc scan;
    IndexInfo  *baseinfo;
    TupleDesc   tupdesc;
    Datum       values[INDEX_MAX_KEYS];
    bool        isnull[INDEX_MAX_KEYS];
    uint64      n = 0;
    BlockNumber base_blocks;
    double      base_tuples;
    instr_time  start, duration;

    INSTR_TIME_SET_CURRENT(start);

    heap = table_open(entry->heap, RowExclusiveLock);
    base = index_open(entry->basex, RowExclusiveLock);
    top  = index_open(entry->top[which], AccessExclusiveLock);

    baseinfo = BuildIndexInfo(base);
    tupdesc  = RelationGetDescr(top);

    scan = index_beginscan(heap, top, SnapshotAny, 0, 0);
    scan->xs_want_itup = true;
    index_rescan(scan, NULL, 0, NULL, 0);

    while (index_getnext_tid(scan, ForwardScanDirection) != NULL)
    {
        if (scan->xs_itup == NULL)
            continue;
        index_deform_tuple(scan->xs_itup, tupdesc, values, isnull);
        index_insert(base, values, isnull, &scan->xs_heaptid, heap,
                     UNIQUE_CHECK_NO, false, baseinfo);
        n++;
        if ((n & 0xFFF) == 0)
            CHECK_FOR_INTERRUPTS();
    }
    index_endscan(scan);

    lsm_truncate_index(top);

    base_blocks = RelationGetNumberOfBlocks(base);
    base_tuples = base->rd_rel->reltuples;
    if (base_tuples < 0)
        base_tuples = 0;
    base_tuples += (double) n;

    index_close(top, AccessExclusiveLock);
    index_close(base, RowExclusiveLock);
    table_close(heap, RowExclusiveLock);

    INSTR_TIME_SET_CURRENT(duration);
    INSTR_TIME_SUBTRACT(duration, start);

    LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
    entry->base_blocks     = base_blocks;
    entry->base_tuples     = base_tuples;
    entry->top_blocks      = 0;
    entry->merge_pending   = false;
    entry->merging         = -1;
    entry->n_merges       += 1;
    entry->n_merged_tuples += n;
    entry->last_merge_ms   = INSTR_TIME_GET_MILLISEC(duration);
    LWLockRelease(lsm_shared->lock);

    elog(LOG, "lsm: merged " UINT64_FORMAT " tuples from top[%d] into index %u in %.1f ms",
         n, which, entry->base, INSTR_TIME_GET_MILLISEC(duration));
}

/* SQL: SELECT lsm_merge('idx'::regclass) -- force a merge, for tests. */
Datum
lsm_merge(PG_FUNCTION_ARGS)
{
    Oid        indexoid = PG_GETARG_OID(0);
    LsmEntry  *entry;
    int        which;

    entry = lsm_get_entry(indexoid, InvalidOid, false);
    if (!entry)
        ereport(ERROR, (errmsg("lsm: index %u is not a registered lsm index", indexoid)));

    LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
    if (entry->merge_pending)
        which = entry->merging;
    else
    {
        which = entry->active;              /* force-flip and merge */
        entry->active  = 1 - which;
        entry->merging = which;
        entry->merge_pending = true;
    }
    LWLockRelease(lsm_shared->lock);

    lsm_do_merge(entry, which);
    PG_RETURN_INT64((int64) entry->n_merged_tuples);
}

/* SQL: SELECT * FROM lsm_stats() */
Datum
lsm_stats(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    HASH_SEQ_STATUS seq;
    LsmEntry   *entry;
    TupleDesc   tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext oldcx;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "lsm_stats: return type must be a row type");

    oldcx = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;
    MemoryContextSwitchTo(oldcx);

    LWLockAcquire(lsm_shared->lock, LW_SHARED);
    hash_seq_init(&seq, lsm_dict);
    while ((entry = (LsmEntry *) hash_seq_search(&seq)) != NULL)
    {
        Datum vals[8];
        bool  nulls[8] = {false, false, false, false, false, false, false, false};

        vals[0] = ObjectIdGetDatum(entry->base);
        vals[1] = ObjectIdGetDatum(entry->heap);
        vals[2] = Int32GetDatum(entry->active);
        vals[3] = BoolGetDatum(entry->merge_pending);
        vals[4] = Int64GetDatum((int64) entry->n_inserts);
        vals[5] = Int64GetDatum((int64) entry->n_merges);
        vals[6] = Int64GetDatum((int64) entry->n_merged_tuples);
        vals[7] = Float8GetDatum(entry->last_merge_ms);
        tuplestore_putvalues(tupstore, tupdesc, vals, nulls);
    }
    LWLockRelease(lsm_shared->lock);
    return (Datum) 0;
}

/* SQL: SELECT lsm_top_sizes('idx'::regclass) -- bytes in each top level */
Datum
lsm_top_sizes(PG_FUNCTION_ARGS)
{
    Oid        indexoid = PG_GETARG_OID(0);
    LsmEntry  *entry = lsm_get_entry(indexoid, InvalidOid, false);
    int64      total = 0;
    int        i;

    if (!entry)
        PG_RETURN_NULL();
    for (i = 0; i < 2; i++)
    {
        if (OidIsValid(entry->top[i]))
        {
            Relation r = index_open(entry->top[i], AccessShareLock);
            total += (int64) RelationGetNumberOfBlocks(r) * BLCKSZ;
            index_close(r, AccessShareLock);
        }
    }
    PG_RETURN_INT64(total);
}

/* SQL: SELECT lsm_base_size('idx'::regclass) -- bytes in the base level */
PG_FUNCTION_INFO_V1(lsm_base_size);
Datum
lsm_base_size(PG_FUNCTION_ARGS)
{
    Oid        indexoid = PG_GETARG_OID(0);
    LsmEntry  *entry = lsm_get_entry(indexoid, InvalidOid, false);
    Relation   r;
    int64      sz;

    if (!entry || !OidIsValid(entry->basex))
        PG_RETURN_NULL();
    r = index_open(entry->basex, AccessShareLock);
    sz = (int64) RelationGetNumberOfBlocks(r) * BLCKSZ;
    index_close(r, AccessShareLock);
    PG_RETURN_INT64(sz);
}

/* ------------------------------------------------- background merger */

static volatile sig_atomic_t got_sigterm = false;
static void
lsm_sigterm(SIGNAL_ARGS)
{
    got_sigterm = true;
    SetLatch(MyLatch);
}

void
lsm_merger_main(Datum arg)
{
    pqsignal(SIGTERM, lsm_sigterm);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection(lsm_merge_database, NULL, 0);

    elog(LOG, "lsm: background merger started for database \"%s\"", lsm_merge_database);

    while (!got_sigterm)
    {
        int rc = WaitLatch(MyLatch,
                           WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                           lsm_merge_interval_ms, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        if (rc & WL_LATCH_SET)
            CHECK_FOR_INTERRUPTS();
        if (got_sigterm)
            break;

        /* Look for a top that has been flipped out and needs draining. */
        for (;;)
        {
            HASH_SEQ_STATUS seq;
            LsmEntry *entry, *target = NULL;
            int which = -1;

            StartTransactionCommand();
            PushActiveSnapshot(GetTransactionSnapshot());

            LWLockAcquire(lsm_shared->lock, LW_SHARED);
            hash_seq_init(&seq, lsm_dict);
            while ((entry = (LsmEntry *) hash_seq_search(&seq)) != NULL)
            {
                if (entry->merge_pending && entry->merging >= 0)
                {
                    target = entry;
                    which  = entry->merging;
                    hash_seq_term(&seq);
                    break;
                }
            }
            LWLockRelease(lsm_shared->lock);

            if (!target)
            {
                PopActiveSnapshot();
                CommitTransactionCommand();
                break;
            }

            PG_TRY();
            {
                lsm_do_merge(target, which);
            }
            PG_CATCH();
            {
                LWLockAcquire(lsm_shared->lock, LW_EXCLUSIVE);
                target->merge_pending = false;
                target->merging = -1;
                LWLockRelease(lsm_shared->lock);
                PG_RE_THROW();
            }
            PG_END_TRY();

            PopActiveSnapshot();
            CommitTransactionCommand();
        }
    }
    elog(LOG, "lsm: background merger exiting");
    proc_exit(0);
}

/* --------------------------------------------------------- amhandler */

Datum
lsm_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *am;

    /* Start from nbtree's routine and override only what differs.  Everything
     * we do not touch -- opclass validation, options, vacuum plumbing -- is
     * B-tree behaviour, which is correct because every level IS a B-tree. */
    am = (IndexAmRoutine *) DatumGetPointer(bthandler(fcinfo));

    am->amcanorder      = false;   /* results arrive base-then-tops, unsorted */
    am->amcanbackward   = false;
    am->amcanunique     = false;   /* uniqueness would need cross-level checks */
    am->amcanparallel   = false;
    am->amcaninclude    = false;
    am->amcanreturn     = NULL;    /* no index-only scans */
    am->amoptionalkey   = true;
    am->amsearcharray   = false;
    am->amclusterable   = false;
    am->ampredlocks     = false;

    am->ambuild         = lsm_build;
    am->ambuildempty    = lsm_buildempty;
    am->aminsert        = lsm_insert;
    am->ambulkdelete    = lsm_bulkdelete;
    am->amvacuumcleanup = lsm_vacuumcleanup;
    am->amcostestimate  = lsm_costestimate;
    am->ambeginscan     = lsm_beginscan;
    am->amrescan        = lsm_rescan;
    am->amgettuple      = lsm_gettuple;
    am->amgetbitmap     = lsm_getbitmap;
    am->amendscan       = lsm_endscan;
    am->ammarkpos       = NULL;
    am->amrestrpos      = NULL;
    am->amestimateparallelscan = NULL;
    am->aminitparallelscan     = NULL;
    am->amparallelrescan       = NULL;

    PG_RETURN_POINTER(am);
}

/* ------------------------------------------------------------- init */

static void
lsm_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
    RequestAddinShmemSpace(MAXALIGN(sizeof(LsmShared)) +
                           hash_estimate_size(lsm_max_indexes, sizeof(LsmEntry)));
    RequestNamedLWLockTranche("lsm", 1);
}

static void
lsm_shmem_startup(void)
{
    HASHCTL info;
    bool    found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    lsm_shared = ShmemInitStruct("lsm shared", sizeof(LsmShared), &found);
    if (!found)
    {
        lsm_shared->lock = &(GetNamedLWLockTranche("lsm"))->lock;
        lsm_shared->n_entries = 0;
    }

    memset(&info, 0, sizeof(info));
    info.keysize   = sizeof(Oid);
    info.entrysize = sizeof(LsmEntry);
    lsm_dict = ShmemInitHash("lsm dict", lsm_max_indexes, lsm_max_indexes,
                             &info, HASH_ELEM | HASH_BLOBS);

    LWLockRelease(AddinShmemInitLock);
}

void
_PG_init(void)
{
    BackgroundWorker worker;

    DefineCustomIntVariable("lsm.top_index_size",
        "Size in kB above which the active top index is flipped and merged.",
        NULL, &lsm_top_index_size_kb, 1024, 64, 1024 * 1024,
        PGC_POSTMASTER, GUC_UNIT_KB, NULL, NULL, NULL);

    DefineCustomIntVariable("lsm.max_indexes",
        "Maximum number of lsm indexes tracked in shared memory.",
        NULL, &lsm_max_indexes, 128, 8, 8192,
        PGC_POSTMASTER, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("lsm.merge_interval",
        "How often the background merger looks for work, in milliseconds.",
        NULL, &lsm_merge_interval_ms, 500, 10, 60000,
        PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);

    DefineCustomBoolVariable("lsm.enabled",
        "When off, lsm indexes behave exactly like B-trees (for A/B testing).",
        NULL, &lsm_enabled, true, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomStringVariable("lsm.merge_database",
        "Database the background merger connects to.",
        NULL, &lsm_merge_database, "postgres",
        PGC_POSTMASTER, 0, NULL, NULL, NULL);

    if (!process_shared_preload_libraries_in_progress)
        return;

    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = lsm_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = lsm_shmem_startup;

    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 5;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "lsm");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "lsm_merger_main");
    snprintf(worker.bgw_name, BGW_MAXLEN, "lsm background merger");
    snprintf(worker.bgw_type, BGW_MAXLEN, "lsm merger");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;
    RegisterBackgroundWorker(&worker);
}
