-- bench.sql -- random-insert throughput, lsm vs btree, as the index outgrows cache.
--
-- The experiment: same table, same rows, same random key order; the only
-- difference is the index type.  Batches are timed individually so you can see
-- the moment the btree stops fitting in shared_buffers and falls off a cliff,
-- which is the entire reason LSM trees exist.
\set ON_ERROR_STOP on
\timing off
SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS lsm;

DROP TABLE IF EXISTS bench_lsm, bench_bt, bench_log;
CREATE TABLE bench_lsm (id bigint, pad text);
CREATE TABLE bench_bt  (id bigint, pad text);
CREATE INDEX bench_lsm_idx ON bench_lsm USING lsm   (id);
CREATE INDEX bench_bt_idx  ON bench_bt  USING btree (id);

CREATE TABLE bench_log (
    which text, batch int, rows_total bigint,
    ms double precision, rows_per_sec double precision,
    index_bytes bigint, base_bytes bigint, top_bytes bigint
);

DO $$
DECLARE
    batches int := %BATCHES%;
    per     int := %PER%;
    b       int;
    t0      timestamptz;
    ms      double precision;
    tot     bigint := 0;
BEGIN
    FOR b IN 1..batches LOOP
        -- btree
        t0 := clock_timestamp();
        INSERT INTO bench_bt
            SELECT (random()*9223372036854775806)::bigint, 'p'
            FROM generate_series(1, per);
        ms := extract(epoch FROM clock_timestamp()-t0)*1000;
        tot := b::bigint * per;
        INSERT INTO bench_log VALUES ('btree', b, tot, ms, per/(ms/1000.0),
                                      pg_relation_size('bench_bt_idx'), NULL, NULL);

        -- lsm
        t0 := clock_timestamp();
        INSERT INTO bench_lsm
            SELECT (random()*9223372036854775806)::bigint, 'p'
            FROM generate_series(1, per);
        ms := extract(epoch FROM clock_timestamp()-t0)*1000;
        INSERT INTO bench_log VALUES ('lsm', b, tot, ms, per/(ms/1000.0),
                                      lsm_index_size('bench_lsm_idx'),
                                      lsm_base_size('bench_lsm_idx'),
                                      lsm_top_sizes('bench_lsm_idx'));
        RAISE NOTICE 'batch % of %  (% rows each)', b, batches, per;
    END LOOP;
END $$;

\echo ''
\echo '=== insert throughput per batch (rows/sec) ==='
SELECT b.batch,
       b.rows_total,
       round(bt.rows_per_sec)::int  AS btree_rows_sec,
       round(b.rows_per_sec)::int   AS lsm_rows_sec,
       round((b.rows_per_sec / bt.rows_per_sec)::numeric, 2) AS speedup,
       pg_size_pretty(bt.index_bytes) AS btree_index,
       pg_size_pretty(b.index_bytes)  AS lsm_index
FROM bench_log b JOIN bench_log bt USING (batch)
WHERE b.which='lsm' AND bt.which='btree'
ORDER BY b.batch;

\echo ''
\echo '=== totals ==='
SELECT which,
       round((sum(ms)/1000.0)::numeric, 1) AS total_seconds,
       max(rows_total) AS rows_inserted,
       round((max(rows_total)/(sum(ms)/1000.0))::numeric) AS overall_rows_sec
FROM bench_log GROUP BY which ORDER BY which;

\echo ''
\echo '=== second half vs first half (the cache-pressure effect) ==='
SELECT which,
       round((avg(rows_per_sec) FILTER (WHERE batch <= (%BATCHES%/2)))::numeric)  AS first_half_rows_sec,
       round((avg(rows_per_sec) FILTER (WHERE batch >  (%BATCHES%/2)))::numeric)  AS second_half_rows_sec,
       round((100.0*(1 - avg(rows_per_sec) FILTER (WHERE batch > (%BATCHES%/2))
                      / avg(rows_per_sec) FILTER (WHERE batch <= (%BATCHES%/2))))::numeric) AS pct_slowdown
FROM bench_log GROUP BY which ORDER BY which;

\echo ''
\echo '=== index sizes and merge stats ==='
SELECT pg_size_pretty(pg_relation_size('bench_bt_idx'))  AS btree_index,
       pg_size_pretty(lsm_index_size('bench_lsm_idx'))   AS lsm_total,
       pg_size_pretty(lsm_base_size('bench_lsm_idx'))    AS lsm_base,
       pg_size_pretty(lsm_top_sizes('bench_lsm_idx'))    AS lsm_tops;

SELECT index_oid::regclass AS idx, n_inserts, n_merges, n_merged_tuples,
       round(last_merge_ms::numeric,1) AS last_merge_ms
FROM lsm_stats() WHERE index_oid = 'bench_lsm_idx'::regclass;

\echo ''
\echo '=== point-lookup latency (reads pay for the extra levels) ==='
ANALYZE bench_lsm; ANALYZE bench_bt;
SET enable_seqscan = off;
DO $$
DECLARE t0 timestamptz; n int; i int; ms double precision;
BEGIN
    t0 := clock_timestamp();
    FOR i IN 1..3000 LOOP
        SELECT count(*) INTO n FROM bench_bt WHERE id = (random()*9223372036854775806)::bigint;
    END LOOP;
    ms := extract(epoch FROM clock_timestamp()-t0)*1000;
    RAISE NOTICE 'btree: 3000 point lookups in % ms (% us each)', round(ms::numeric,1), round((ms*1000/3000)::numeric,1);

    t0 := clock_timestamp();
    FOR i IN 1..3000 LOOP
        SELECT count(*) INTO n FROM bench_lsm WHERE id = (random()*9223372036854775806)::bigint;
    END LOOP;
    ms := extract(epoch FROM clock_timestamp()-t0)*1000;
    RAISE NOTICE 'lsm:   3000 point lookups in % ms (% us each)', round(ms::numeric,1), round((ms*1000/3000)::numeric,1);
END $$;
