# PostgreSQL LSM-Tree Index Access Method (`pglsm`)

A high-performance, pluggable Log-Structured Merge-Tree (`lsm`) Index Access Method extension for PostgreSQL 16.

`pglsm` optimizes write-heavy database workloads by buffering random inserts in small, cache-resident memory levels and sequentially merging them into a backing base B-tree via a dedicated background worker.

---

## Architectural Design

```
            CREATE INDEX events_idx ON events USING lsm (id);

                           ┌────────────────────────────┐
          INSERT ─────────▶│        lsm_insert()        │
                           │  Routes to active top tree │
                           └─────────────┬──────────────┘
                                         ▼
            ┌──────────────┐      ┌──────────────┐      ┌──────────────┐
            │     base     │      │    top[0]    │      │    top[1]    │
            │  (nbtree)    │      │  (nbtree)    │      │  (nbtree)    │
            │  e.g. 143 MB │      │  e.g. ~4 MB  │      │  e.g. ~4 MB  │
            └──────────────┘      └──────────────┘      └──────────────┘
                   ▲                     │
                   │                     │  When top exceeds lsm.top_index_size:
                   │                     │   1. Flip active index to sibling top
                   └─────────────────────┘   2. BgWorker performs ordered merge
                        Sequential,             into base tree
                       in key order          3. Truncate merged top level

          SELECT ─────────▶ lsm_beginscan() coordinates multi-level search across
                            base, top[0], and top[1]
```

### Why It Accelerates Writes
- **Random Insert Degradation in Standard B-Trees**: When a table's B-tree index exceeds PostgreSQL's `shared_buffers`, every random insert causes a cache miss, forcing an expensive random disk read to fetch the leaf page, followed by a dirty page write.
- **LSM Buffering Advantage**: Inserts always target a small, cache-hot top B-tree (`~4 MB`), which remains $100\%$ memory-resident. When full, the background worker streams records from the top tree in strict key order into the base tree. Because writes to the base tree are sorted, leaf page updates are sequential and page locality is maximized.

### The Partial Index Trick
To prevent PostgreSQL's query executor from multiplying writes across all internal levels, `top[0]`, `top[1]`, and `base` are registered internally as **partial indexes with a constant-false predicate**:
```c
idxinfo->ii_Predicate = list_make1(makeBoolConst(false, false));
```
This guarantees:
1. Standard table inserts skip writing to child trees automatically (only `lsm_insert` routes tuples to the active top).
2. The query planner never erroneously selects an individual child level.
3. PostgreSQL's background `VACUUM` worker automatically scans and purges dead tuple IDs (TIDs) across all levels with zero custom vacuum code required.

---

## Measured Performance & Benchmark Results

*Environment: PostgreSQL 16, 2 vCPU Linux VM, `shared_buffers = 16MB`, `lsm.top_index_size = 4MB`, `synchronous_commit = off`.*

### 1. Random Insert Throughput (5.2 Million Rows in 26 Batches of 200k)

| Total Rows | btree Throughput | pglsm Throughput | Speedup | Index Size |
|:---:|:---:|:---:|:---:|:---:|
| **200k** | 304,959 rows/s | 236,138 rows/s | 0.77× *(in cache)* | 5 MB |
| **600k** | 224,424 rows/s | 225,171 rows/s | 1.00× *(crossover)* | 17 MB |
| **1.0M** | 119,199 rows/s | 218,126 rows/s | **1.83×** | 30 MB |
| **2.0M** | 98,326 rows/s | 207,854 rows/s | **2.11×** | 59 MB |
| **3.0M** | 71,440 rows/s | 156,720 rows/s | **2.19×** | 88 MB |
| **5.2M (Total)** | **96,834 rows/s** | **157,000 rows/s** | **1.62× Overall** | 143 MB |

> **Key Observation**: Once the index outgrows `shared_buffers` (16 MB at ~600k rows), standard B-tree throughput collapses due to random disk I/O, while `pglsm` sustains high throughput.

### 2. Read Amplification Trade-off
- **Point Lookup Buffer Hits**: `8 buffers (lsm)` vs `3 buffers (btree)`.
- **Point Lookup Latency**: `0.118 ms (lsm)` vs `0.046 ms (btree)`.
- *Trade-off*: As with all LSM designs, high write throughput is balanced against probing multiple levels on point lookups.

---

## Correctness & Regression Testing

The test suite validates `pglsm` against a canonical PostgreSQL `btree` on identical data across **14 correctness assertions**:
- Exact equality searches (`=`)
- Inclusive and exclusive range scans (`<`, `<=`, `>`, `>=`, `BETWEEN`)
- Bitmap index scans (`BitmapIndexScan`)
- `UPDATE` and `DELETE` TID visibility
- Correct handling of `NULL` values
- `VACUUM` and dead tuple reclamation
- Read consistency across active background merges

---

## Project Structure

```
.
├── Makefile                     # PGXS extension build specification
├── lsm.control                  # PostgreSQL extension metadata
├── lsm.c                        # Core C implementation: Index AM, Scan, Merge, BgWorker
├── lsm--1.0.sql                 # SQL schema, CREATE ACCESS METHOD, and opclasses
├── BLUEPRINT.md                 # Design audit, technical paper & trade-off analysis
├── RESULTS.txt                  # Full captured benchmark logs and regression runs
├── test/
│   └── regress.sql              # 14-point functional validation suite
└── bench/
    └── bench.sql                # Batched random-insert benchmark harness
```

---

## Installation & Setup

### Prerequisites
- PostgreSQL 16+ (Server and Development Headers)
  ```bash
  sudo apt install postgresql-16 postgresql-server-dev-16
  ```

### 1. Build and Install Extension
```bash
make
sudo make install
```

### 2. Configure PostgreSQL (`postgresql.conf`)
Add `lsm` to shared preload libraries:
```ini
shared_preload_libraries = 'lsm'
shared_buffers = 16MB          # Set to realistic size to observe cache boundaries
lsm.top_index_size = 4096      # Flush threshold in kB (4 MB)
lsm.merge_interval = 200       # Background merge polling interval in ms
```
Restart PostgreSQL service:
```bash
sudo systemctl restart postgresql
```

### 3. Usage Example in SQL
```sql
-- Load extension
CREATE EXTENSION lsm;

-- Create table and LSM index
CREATE TABLE sensor_telemetry (
    id bigint PRIMARY KEY,
    device_id int,
    reading double precision,
    recorded_at timestamp
);

CREATE INDEX telemetry_lsm_idx ON sensor_telemetry USING lsm (id);

-- Insert high-volume telemetry stream
INSERT INTO sensor_telemetry
SELECT g, (random()*1000)::int, random()*100.0, now()
FROM generate_series(1, 2000000) g;

-- Query performance stats
SELECT * FROM lsm_stats();

-- Check sizes of individual tiers
SELECT pg_size_pretty(lsm_base_size('telemetry_lsm_idx'))  AS base_size,
       pg_size_pretty(lsm_top_sizes('telemetry_lsm_idx'))  AS tops_size,
       pg_size_pretty(lsm_index_size('telemetry_lsm_idx')) AS total_size;

-- Trigger explicit manual merge
SELECT lsm_merge('telemetry_lsm_idx');
```

---

## Running Tests & Benchmarks

```bash
# Run 14-point regression suite
psql -d postgres -f test/regress.sql

# Run 5.2M row comparative benchmark vs standard B-tree
sed -e 's/%BATCHES%/26/' -e 's/%PER%/200000/' bench/bench.sql | psql -d postgres
```
