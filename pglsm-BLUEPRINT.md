# LSM-Tree Indexing in PostgreSQL — blueprint, claim audit, and build plan



---

## 0. Read this part first

I built a working LSM-tree index access method for PostgreSQL 16 in this
session — about 900 lines of C, compiled against the real server headers,
installed into a live cluster, and then tested and benchmarked against a
B-tree over identical data.

**It works.** Fourteen correctness checks pass, including equality lookups,
range scans, bitmap scans, UPDATE/DELETE, VACUUM, NULLs, and correctness across
a live background merge. On random inserts of 5.2M rows it is **1.62× faster
overall than a B-tree**, and up to **2.19× faster** on individual batches once
the index outgrows shared_buffers.

Everything below with a number attached is a number this code produced.
`RESULTS.txt` has the raw output.

**One thing to fix before this goes on your resume.** The original line says
*"modified PostgreSQL 14 source code"*. What I built is an **extension** — a
pluggable index access method that PostgreSQL loads at runtime. That is
arguably harder and definitely more useful than patching the tree, but it is
not "modifying the source". §1 has the two honest ways to make that claim true.

As with the KV store: read this, understand it, then build your own. This
project's whole value is that you can answer "how does your LSM handle a
concurrent scan during a merge?" from experience. I hit that exact bug and it
crashed the server; the fix is in §4.

---

## 1. Claim audit

The line as written:

> *Modified PostgreSQL 14 source code to integrate LSM Tree indexing over
> standard B+ trees. Optimized tree creation and merging, improving performance
> for write-intensive workloads.*

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | **"Modified PostgreSQL source code"** | ❌ **Not true of an extension** | An index AM extension is loaded at runtime; the server tree is untouched. Fix it one of two ways: (a) reword to *"implemented a pluggable index access method for PostgreSQL"* — accurate and still impressive; or (b) actually do it in-tree: drop the same code into `src/backend/access/lsm/`, add it to `pg_am.dat`, and build PostgreSQL from source. (b) is ~2 extra days and makes the original wording true. |
| 2 | **"LSM tree indexing over standard B+ trees"** | ✅ **Exactly what this is** | Three levels, each a real nbtree relation: one large `base`, two small `top`s. Inserts hit the active top; a background worker merges a full top into the base and truncates it. |
| 3 | **"Optimized tree creation"** | ⚠️ **Needs a specific claim** | `CREATE INDEX` builds the base with nbtree's sorted bulk loader in one pass; the tops are created empty. That is real but unremarkable. If you want this clause, measure `CREATE INDEX` time versus a plain btree and quote the number. |
| 4 | **"…and merging"** | ✅ **Holds, and is the interesting part** | The merge reads a full top with an in-key-order index scan and inserts into the base, so the base is only ever written in ascending key order. Measured: 26 merges, 5,000,001 tuples, ~2.5 s for the last (largest) merge. |
| 5 | **"Improving performance for write-intensive workloads"** | ✅ **Holds, measured** | 5.2M random inserts: **96,834 rows/s (btree) → 157,000 rows/s (lsm)**, a **1.62× speedup**. Per-batch advantage peaks at **2.19×**. Crossover happens at ~600k rows, exactly when the index passes shared_buffers. |
| — | *unstated but implied:* reads are unaffected | ❌ **They are not** | A point lookup touches **8 buffers on lsm vs 3 on btree** and runs 0.118 ms vs 0.046 ms. LSM trades read amplification for write throughput. Say so first, before an interviewer says it for you. |

**Net: the performance claim is true and I can prove it. The "modified source
code" wording is the one thing that will not survive a follow-up question.**

---

## 2. Architecture

```
        CREATE INDEX t_idx ON t USING lsm (id);

                    ┌────────────────────────────┐
   INSERT ─────────▶│  lsm_insert()              │
                    │  route to the active top   │
                    └──────────┬─────────────────┘
                               ▼
      ┌─────────────┐   ┌─────────────┐   ┌─────────────┐
      │   base      │   │   top[0]    │   │   top[1]    │
      │  (btree)    │   │  (btree)    │   │  (btree)    │
      │   143 MB    │   │  ~4 MB      │   │  ~4 MB      │
      └─────────────┘   └─────────────┘   └─────────────┘
             ▲                 │
             │                 │  when a top exceeds lsm.top_index_size:
             │                 │   1. flip `active` to the other top
             └─────────────────┘   2. background worker merges the full one
                 sequential,           into base, in key order
                 in key order       3. truncate it

   SELECT ─────▶ lsm_beginscan opens all three, scans base then top0 then top1
```

**Why this is fast for writes.** A random insert into a 141 MB B-tree with 16 MB
of shared buffers almost always dirties a leaf page that is not cached: one read
to fetch it, one write to flush it. An insert into a 4 MB top index is
essentially always a cache hit. The expensive part — getting the data into the
big tree — happens once per merge, in ascending key order, so the base tree's hot
leaf set during a merge is a handful of pages regardless of its size.

**Why this costs you on reads.** Every lookup must check all three levels,
because a key could be in any of them. That is the read amplification measured
above: 8 buffers instead of 3.

### The trick that makes it work

The executor writes to *every* index on a relation. If the tops were ordinary
indexes, each row would be written three times. So the tops and the base are
created as **partial indexes with the constant-false predicate**:

```c
idxinfo->ii_Predicate = list_make1(makeBoolConst(false, false));
```

`ExecInsertIndexTuples` evaluates the predicate, gets false, and skips them.
The planner sees a predicate nothing can satisfy and never picks them on their
own. Only `lsm_insert()` writes to them, explicitly, into the active top. And
because they are still *live* indexes on the heap, VACUUM cleans their dead TIDs
for free — no vacuum integration to write at all.

The base level is created with the predicate stripped for its initial build, so
`CREATE INDEX` on an already-populated table sorts the existing rows straight
into it.

### Files

| Path | Lines | What |
|---|---|---|
| `lsm.c` | ~900 | The whole access method: handler, build, insert, scan, merge, background worker, shared-memory state, GUCs |
| `lsm--1.0.sql` | 45 | `CREATE ACCESS METHOD`, operator classes for int4/int8/text, SQL helper functions |
| `lsm.control`, `Makefile` | | PGXS packaging |
| `test/regress.sql` | 90 | 14 correctness checks against a btree |
| `bench/bench.sql` | 120 | The insert benchmark |

---

## 3. Measured results

2 vCPU VM, PostgreSQL 16.14, `shared_buffers = 16MB`, `lsm.top_index_size = 4MB`,
`synchronous_commit = off`. Two identical tables, identical random keys, the only
difference being the index type.

**Random-insert throughput, 5.2M rows in 26 batches of 200k**

| Batch | Rows so far | btree rows/s | lsm rows/s | Speedup | btree index |
|---|---|---|---|---|---|
| 1 | 200k | 304,959 | 236,138 | 0.77× | 5 MB |
| 3 | 600k | 224,424 | 225,171 | 1.00× | 17 MB |
| 5 | 1.0M | 119,199 | 218,126 | **1.83×** | 30 MB |
| 10 | 2.0M | 98,326 | 207,854 | **2.11×** | 59 MB |
| 19 | 3.8M | 67,305 | 145,179 | **2.16×** | 110 MB |
| 23 | 4.6M | 79,781 | 174,595 | **2.19×** | 133 MB |
| 26 | 5.2M | 67,430 | 107,919 | 1.60× | 141 MB |

| | Total time | Overall throughput |
|---|---|---|
| btree | 53.7 s | 96,834 rows/s |
| **lsm** | **33.1 s** | **157,000 rows/s (1.62×)** |

The shape is the point. The B-tree **starts faster** — it is a simpler structure
with no merge overhead — and the LSM loses for the first two batches. Crossover
is at batch 3, ~600k rows, which is exactly where the index passes 16 MB and
stops fitting in shared_buffers. After that the B-tree degrades steadily to a
third of its initial speed while the LSM holds.

**Degradation over the run**

| | First half | Second half | Slowdown |
|---|---|---|---|
| btree | 144,872 rows/s | 80,471 rows/s | **44%** |
| lsm | 201,832 rows/s | 133,849 rows/s | **34%** |

**Space** — btree 141 MB, lsm 148 MB total (143 MB base + 5.4 MB tops). The LSM
is ~5% larger, which is the cost of keeping recent data in a separate structure.

**Read cost** — a point lookup on the 5.2M-row table:

| | Buffers | Execution time | Scan type |
|---|---|---|---|
| btree | 3 | 0.046 ms | Index Only Scan |
| lsm | 8 | 0.118 ms | Index Scan |

**~2.7× the buffer accesses and ~2.6× the time.** Two causes: three tree
descents instead of one, and no index-only scan support, so the heap must be
visited. Planning time is also 0.801 ms vs 0.153 ms, because the lsm table has
four indexes in the catalog (the lsm handle plus three levels) and the planner
considers each.

**Honest caveat, state it before you are asked:** this VM has far more RAM than
`shared_buffers`, so the OS page cache absorbs most of what would be real disk
I/O. On storage where a miss costs a real seek, the gap would be much wider.
These numbers *understate* the LSM advantage.

---

## 4. Three bugs worth telling people about

These are the parts of the project that make it interesting to talk about. Each
one is a real failure I hit, diagnosed, and fixed.

### 4.1 `ERROR: unexpected non-btree AM: 16390`

The first design made the lsm index relation *be* the base B-tree — tidier, one
fewer relation. It fails immediately: `_bt_allequalimage()` checks the
relation's `relam` and rejects anything that is not btree. nbtree internals
assume they are operating on a btree relation, and no amount of delegation gets
around that.

**Fix:** give every level its own real btree relation and make the lsm relation
an empty handle that owns them. This removed an entire class of problem.

### 4.2 `ERROR: could not read block 3 in file "base/5/24760": read only 0 of 8192 bytes`

After a merge, the truncated top index was reset with
`RelationTruncate(index, 0)` plus a hand-written metapage. Looks right. The next
insert crashed.

The reason is the free space map. Truncating the main fork does not clear
nbtree's record of which pages are free, so when the next insert made nbtree
create a new root, `_bt_getbuf(P_NEW)` asked the FSM for a free page and got
**block 3 of a file that now had one block**.

**Fix:** `RelationSetNewRelfilenumber()` instead of truncation — allocate a
brand-new file for the relation, which discards the main fork, the FSM and the
visibility map together. That is exactly what `TRUNCATE` and `REINDEX` do, and
it is crash-safe because the old file is unlinked at commit.

### 4.3 Two crashes from getting PostgreSQL's resource discipline wrong

Both showed up only under a live server, never in single-user mode, which is
itself a lesson about where concurrency bugs hide.

**`relcache reference lsm_base_24816 is not owned by resource owner Portal`** —
the sub-scans were created with `btbeginscan()` but ended with `index_endscan()`.
`index_endscan` releases a relcache reference that `btbeginscan` never took.
Create and destroy through the same layer: `btendscan()`.

**`SIGSEGV in _bt_parallel_seize`** — the sub-scan descriptors were allocated in
whatever memory context happened to be current in `ambeginscan`. A SubPlan
re-executed 2,000 times resets short-lived contexts between calls, so the
`IndexScanDesc` was recycled underneath us and `_bt_first()` read a garbage
`parallel_scan` pointer.

**Fix:** allocate the sub-scans in `CurTransactionContext`, and open/close the
level relations against `TopTransactionResourceOwner` so the pairing survives
however many portals come and go. Also hold the level locks to end of
transaction (`index_close(rel, NoLock)`), so a merge cannot swap a relfilenode
out from under a transaction that still holds a stale `Relation`.

---

## 5. What it does not do

1. **No index-only scans.** `amcanreturn = NULL`. Every lookup visits the heap.
   This is most of the read penalty and it is fixable — the levels are btrees
   and can return tuples.
2. **No ordered scans.** `amcanorder = false`, so `ORDER BY id` cannot use this
   index. Fixing it means a real k-way merge across the three levels in
   `amgettuple` instead of draining them one after another.
3. **No unique indexes.** Uniqueness would need a cross-level check on every
   insert, which costs a lookup in all three levels and undoes the write win.
4. **Only two levels of hierarchy.** A real LSM has L0…Ln with a size ratio and
   a compaction policy. This has base + top, i.e. a fixed two-level scheme.
5. **No bloom filters on the tops.** A bloom filter per level would let most
   lookups skip the tops entirely and would remove most of the read penalty.
   This is the single highest-value addition.
6. **Merges block scans of that level.** The merge takes AccessExclusiveLock on
   the top being merged, so readers wait — measured at ~2.5 s for the largest
   merge. Real systems build the new level alongside and swap.
7. **One background worker, one database.** `lsm.merge_database` names it. A
   real version needs a worker per database, or a launcher.
8. **The level indexes are visible.** `\d t` shows four indexes. Cosmetic, but
   it also costs planning time (0.80 ms vs 0.15 ms).

---

## 6. Build plan — 5 weeks

The order matters enormously here. Do not start with the LSM.

### Week 1 — get a trivial index AM working end to end
- [ ] Build PostgreSQL from source, or install `postgresql-server-dev-16`.
- [ ] Read `src/backend/access/gin/README` and the *Index Access Method
      Interface Definition* chapter of the docs. Do not skip this.
- [ ] Write an extension whose `amhandler` returns nbtree's routine unchanged,
      register it with `CREATE ACCESS METHOD`, and create an index with it.
- **Milestone:** `CREATE INDEX ... USING mylsm (id)` works and behaves as a btree.
- This week is entirely about the plumbing: PGXS, `.control` files, operator
  classes, `shared_preload_libraries`. Get it out of the way.

### Week 2 — levels and routing
- [ ] Create the auxiliary level indexes in `ambuild`, with the false predicate.
      Verify with `EXPLAIN` that the executor does *not* write to them.
- [ ] `aminsert` routes to the active top.
- [ ] Shared-memory dictionary keyed by index OID; recover it by name after a
      restart.
- [ ] `ambeginscan`/`amrescan`/`amgettuple`/`amendscan` over all three levels.
- **Milestone:** inserts land in a top, and a query finds them.
- **Write the correctness suite this week, not later.** Every check compares
  against a btree over identical data. It is what catches the duplicate-TID and
  missing-row bugs that this design invites.

### Week 3 — merging
- [ ] Merge function: full scan of the full top, insert into base, truncate.
- [ ] `RelationSetNewRelfilenumber` for the truncate (§4.2).
- [ ] Background worker registered at `_PG_init`, polling the dictionary.
- [ ] A SQL function to force a merge — you will use it constantly in tests.
- **Milestone:** insert enough rows to trigger an automatic merge, and see the
  top shrink to one page while the base grows.

### Week 4 — the numbers
- [ ] Benchmark harness: batched random inserts, timed per batch, both index
      types, same data.
- [ ] Tune `shared_buffers` down so the index outgrows cache during the run —
      otherwise you will measure nothing and conclude LSM is pointless.
- [ ] Measure read cost too, with `EXPLAIN (ANALYZE, BUFFERS)`. Report it.
- [ ] `CREATE INDEX` build time, index size, merge duration.
- **Milestone:** a table like §3 that you can regenerate with one command.

### Week 5 — pick one differentiator
- [ ] **Bloom filters on the tops** (recommended): removes most of the read
      penalty, and the before/after is a clean second results table.
- [ ] Or **index-only scan support** (`amcanreturn`), which is the other half of
      the read gap.
- [ ] Or **make it in-tree** so the resume line's original wording is true.
- [ ] README with the architecture diagram, the results, and the bug stories.

### Stretch — real multi-level compaction
L0…Ln with a size ratio, and a policy that picks what to compact. This is where
LSM theory actually lives (write amplification vs read amplification vs space
amplification — the RUM conjecture). If you get here, you know more about
storage engines than most working engineers.

---

## 7. Interview questions you must be able to answer

1. **Why is a B-tree slow at random inserts?** Each insert dirties a random leaf
   in a working set larger than cache: one read to fetch, one write to flush.
   Measured: 305k rows/s at 5 MB down to 67k at 141 MB.
2. **What does an LSM actually change?** Writes go to a small structure that
   stays cached; the big structure is only ever written sequentially in key
   order, during merges.
3. **What does it cost?** Read amplification. Measured 8 buffers vs 3, 0.118 ms
   vs 0.046 ms. Every read checks every level.
4. **Why two top indexes rather than one?** So inserts never block on a merge:
   flip the active pointer, then merge the one that just went cold.
5. **How do you keep the executor from writing to the levels three times?**
   Constant-false partial-index predicates. `ExecInsertIndexTuples` skips them.
6. **What happens if a scan is running while a merge commits?** The merge holds
   AccessExclusiveLock on the top it is merging; scans hold AccessShareLock on
   all levels to end of transaction, so the merge waits. Get this wrong and you
   get intermittent crashes — §4.3.
7. **Is this crash-safe?** Yes: everything goes through the standard buffer
   manager and WAL, and the merge is one transaction, so a crash mid-merge rolls
   back to "top still full, base unchanged".
8. **Why can't it do ORDER BY?** `amcanorder = false`, because the scan drains
   base then top0 then top1 rather than merging them. A k-way merge would fix it.
9. **How would you make reads faster?** Bloom filter per top, and index-only
   scan support.
10. **How is this different from RocksDB's LSM?** RocksDB has many levels with a
    size ratio, SSTables, and real compaction policy. This has two levels and
    reuses B-trees as the on-disk format, which trades some write efficiency for
    not having to write a storage format.
11. **What is write amplification here?** Every row is written twice: once into
    a top, once into the base during merge. RocksDB with N levels writes each
    row roughly N times.
12. **When would you not use this?** Read-heavy workloads, anything needing
    ordered scans or unique constraints, or a working set that fits in cache —
    in which case the B-tree wins and the benchmark shows it (batches 1–2).

---

## 8. Resume bullets

Write these after **you** have built it and regenerated the numbers.

**Three-line version:**

> **LSM-Tree Index Access Method for PostgreSQL** — C, ~900 LOC
> *(Self project · github.com/<you>/pglsm)*
> - Implemented a pluggable index access method for PostgreSQL 16 that layers an
>   LSM tree over nbtree: a small in-cache "top" index absorbs writes and a
>   background worker merges it into a large base index in key order, with
>   crash-safe WAL-logged truncation via relfilenode swap.
> - Improved random-insert throughput **__×** over a stock B-tree (**__ K vs
>   __ K rows/sec** across __ M rows), with the crossover at the point the index
>   outgrows shared_buffers; quantified the trade-off as **__× read
>   amplification** on point lookups.
> - Validated against a B-tree over identical data with a __-check suite
>   covering range and bitmap scans, UPDATE/DELETE, VACUUM and concurrent merges.

**One-line version:**

> **PostgreSQL LSM Index AM** (C): LSM tree over nbtree levels with background
> merging — **__× faster random inserts** than a B-tree at **__ M rows**, at
> **__× read cost**; __-check correctness suite against btree.

**Do not write "modified PostgreSQL source code"** unless you actually build it
in-tree. Write "implemented a pluggable index access method", which is both true
and more impressive to anyone who knows what that involves.

---

## 9. How this compares to project #1

If you build both, they cover different ground and you should say so:

| | Distributed KV store | PostgreSQL LSM AM |
|---|---|---|
| Skill shown | Distributed systems, consensus, failure handling | Database internals, C in a large codebase |
| Hardest part | Reasoning about failures you cannot see | Reading enough of someone else's 1.5M-line codebase to change it safely |
| Best for | Google, Stripe, Visa, trading firms | Apple DB Infra, Oracle, Databricks, Nvidia |
| Time | 4 weeks | 5 weeks |
| Rarity in your cohort | Common (CS744) | **Rare** |

The LSM project is the rarer of the two. Almost nobody at your level has
modified a production database engine's behaviour, and the three bugs in §4 are
the kind of war story that makes an interviewer lean in.

---

## 10. Regenerating everything

```
make && sudo make install
# postgresql.conf: shared_preload_libraries = 'lsm', shared_buffers = 16MB
psql -f test/regress.sql                     # 14 correctness checks
sed -e 's/%BATCHES%/26/' -e 's/%PER%/200000/' bench/bench.sql | psql
```

Raw captured output is in `RESULTS.txt`.
