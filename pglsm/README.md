# pglsm — an LSM-tree index access method for PostgreSQL

A working PostgreSQL 16 extension that adds an `lsm` index type: writes land in a
small, cache-resident B-tree and a background worker merges it, in key order,
into a large base B-tree. Random inserts get **1.62× faster** than a stock
B-tree once the index outgrows `shared_buffers`; point lookups cost **~2.7×**
the buffer accesses. Both numbers are measured — see `RESULTS.txt`.

**~900 lines of C. No dependencies beyond the PostgreSQL server headers.**

```bash
sudo apt install postgresql-server-dev-16
make && sudo make install

# postgresql.conf
#   shared_preload_libraries = 'lsm'
#   shared_buffers = 16MB          # so the index actually outgrows cache
#   lsm.top_index_size = 4096      # kB; flip + merge above this
#   lsm.merge_interval = 200       # ms

psql -c "CREATE EXTENSION lsm;"
psql -f test/regress.sql                                       # 14 checks vs btree
sed -e 's/%BATCHES%/26/' -e 's/%PER%/200000/' bench/bench.sql | psql
```

## Use it

```sql
CREATE TABLE events (id bigint, payload text);
CREATE INDEX events_idx ON events USING lsm (id);

INSERT INTO events SELECT (random()*1e12)::bigint, 'x'
FROM generate_series(1,1000000);

SELECT * FROM lsm_stats();
--  index_oid | active_top | merge_pending | n_inserts | n_merges | n_merged_tuples | last_merge_ms

SELECT pg_size_pretty(lsm_base_size('events_idx')),   -- the big level
       pg_size_pretty(lsm_top_sizes('events_idx')),   -- the two small ones
       pg_size_pretty(lsm_index_size('events_idx'));  -- total

SELECT lsm_merge('events_idx');   -- force a merge now
SET lsm.enabled = off;            -- route inserts straight to base: it is now a btree
```

Supported opclasses: `int4`, `int8`, `text`. Adding more is three lines of SQL
each — every level is a B-tree, so they reuse btree's comparison functions.

## How it works

```
INSERT ──▶ lsm_insert() ──▶ active top (small btree, stays in cache)
                                 │
              top exceeds lsm.top_index_size
                                 │
                    flip active to the other top
                                 ▼
              background worker merges the full top into
              base in ascending key order, then truncates it
```

All three levels are ordinary btree relations. They are created as **partial
indexes with the predicate `false`**, so the executor skips them on INSERT (only
`lsm_insert` writes to them) while VACUUM still cleans their dead TIDs for free.
The `lsm` relation itself stores nothing — it is a handle that owns the levels.

See `BLUEPRINT.md` for the full design, the measured results, three instructive
bugs, and a 5-week plan to build it yourself.

## Layout

| Path | What |
|---|---|
| `lsm.c` | The access method: handler, build, insert, scan, merge, bgworker, shmem, GUCs |
| `lsm--1.0.sql` | `CREATE ACCESS METHOD`, opclasses, SQL helpers |
| `test/regress.sql` | 14 correctness checks against a btree over identical data |
| `bench/bench.sql` | Batched random-insert benchmark, lsm vs btree |
| `RESULTS.txt` | Raw output of every test and benchmark run |
| `BLUEPRINT.md` | Design, claim audit, build plan, interview prep |

## Known limitations

No index-only scans, no ordered scans (`amcanorder = false`), no unique indexes,
two levels rather than a full L0…Ln hierarchy, no bloom filters on the tops, and
merges block readers of the level being merged. `BLUEPRINT.md` §5 explains each
and what fixing it would take.
