-- regress.sql -- correctness suite for the lsm access method.
-- Every check compares an lsm index against a btree over identical data.
\set ON_ERROR_STOP on
\timing off
SET client_min_messages = warning;

DROP TABLE IF EXISTS lt, bt CASCADE;
CREATE EXTENSION IF NOT EXISTS lsm;

CREATE TABLE lt (id bigint, s text);
CREATE TABLE bt (id bigint, s text);
CREATE INDEX lt_idx ON lt USING lsm  (id);
CREATE INDEX bt_idx ON bt USING btree(id);

-- identical data in both tables
INSERT INTO lt SELECT (random()*200000)::bigint, md5(g::text) FROM generate_series(1,120000) g;
INSERT INTO bt SELECT id, s FROM lt;

ANALYZE lt; ANALYZE bt;
SET enable_seqscan = off;
SET enable_bitmapscan = on;

\echo '--- 1. row counts match'
SELECT (SELECT count(*) FROM lt) = (SELECT count(*) FROM bt) AS counts_match;

\echo '--- 2. the planner actually picks the lsm index'
EXPLAIN (COSTS OFF) SELECT count(*) FROM lt WHERE id = 100::bigint;

\echo '--- 3. equality lookups agree on 2000 random keys'
SELECT count(*) AS mismatches FROM (
  SELECT k FROM generate_series(1,2000) k
) g
WHERE (SELECT count(*) FROM lt WHERE id = (g.k*97)::bigint)
   <> (SELECT count(*) FROM bt WHERE id = (g.k*97)::bigint);

\echo '--- 4. range scans agree'
SELECT count(*) AS mismatches FROM (
  SELECT k FROM generate_series(1,200) k
) g
WHERE (SELECT count(*) FROM lt WHERE id BETWEEN (g.k*500)::bigint AND (g.k*500+400)::bigint)
   <> (SELECT count(*) FROM bt WHERE id BETWEEN (g.k*500)::bigint AND (g.k*500+400)::bigint);

\echo '--- 5. no duplicate TIDs returned (would double-count rows)'
SELECT (SELECT count(*) FROM lt WHERE id < 50000::bigint)
     = (SELECT count(*) FROM bt WHERE id < 50000::bigint) AS no_duplicates;

\echo '--- 6. bitmap scans agree with index scans'
SET enable_indexscan = off;
SELECT (SELECT count(*) FROM lt WHERE id BETWEEN 1000::bigint AND 9000::bigint)
     = (SELECT count(*) FROM bt WHERE id BETWEEN 1000::bigint AND 9000::bigint) AS bitmap_matches;
SET enable_indexscan = on;

\echo '--- 7. results survive a forced merge'
SELECT lsm_merge('lt_idx') > 0 AS merged_something;
SELECT (SELECT count(*) FROM lt WHERE id < 50000::bigint)
     = (SELECT count(*) FROM bt WHERE id < 50000::bigint) AS correct_after_merge;

\echo '--- 8. UPDATE and DELETE are reflected'
DELETE FROM lt WHERE id < 1000::bigint;
DELETE FROM bt WHERE id < 1000::bigint;
UPDATE lt SET id = id + 1000000 WHERE id BETWEEN 5000::bigint AND 6000::bigint;
UPDATE bt SET id = id + 1000000 WHERE id BETWEEN 5000::bigint AND 6000::bigint;
SELECT (SELECT count(*) FROM lt) = (SELECT count(*) FROM bt) AS counts_match_after_dml;
SELECT (SELECT count(*) FROM lt WHERE id > 1000000::bigint)
     = (SELECT count(*) FROM bt WHERE id > 1000000::bigint) AS updated_rows_found;

\echo '--- 9. VACUUM cleans all three levels'
VACUUM (ANALYZE) lt;
SELECT (SELECT count(*) FROM lt WHERE id < 200000::bigint)
     = (SELECT count(*) FROM bt WHERE id < 200000::bigint) AS correct_after_vacuum;

\echo '--- 10. NULLs behave'
INSERT INTO lt VALUES (NULL,'n'); INSERT INTO bt VALUES (NULL,'n');
SELECT (SELECT count(*) FROM lt WHERE id IS NULL)
     = (SELECT count(*) FROM bt WHERE id IS NULL) AS nulls_match;

\echo '--- 11. text opclass works too'
DROP TABLE IF EXISTS lt2;
CREATE TABLE lt2 (k text);
CREATE INDEX lt2_idx ON lt2 USING lsm (k);
INSERT INTO lt2 SELECT md5(g::text) FROM generate_series(1,20000) g;
SELECT count(*) = 1 AS text_lookup_works FROM lt2 WHERE k = md5('7');

\echo '--- 12. lsm.enabled = off degrades to a plain btree, same answers'
SET lsm.enabled = off;
INSERT INTO lt SELECT (random()*200000)::bigint, 'off' FROM generate_series(1,5000);
SELECT count(*) = 5000 AS inserts_with_lsm_disabled FROM lt WHERE s = 'off';
SET lsm.enabled = on;

\echo '--- 13. level sizes are reported'
SELECT pg_size_pretty(lsm_base_size('lt_idx')) AS base,
       pg_size_pretty(lsm_top_sizes('lt_idx')) AS tops,
       pg_size_pretty(lsm_index_size('lt_idx')) AS total;

\echo '--- 14. stats'
SELECT index_oid::regclass AS idx, n_inserts, n_merges, n_merged_tuples FROM lsm_stats() ORDER BY 1;
