-- lsm 1.0
\echo Use "CREATE EXTENSION lsm" to load this file. \quit

CREATE FUNCTION lsm_handler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE ACCESS METHOD lsm TYPE INDEX HANDLER lsm_handler;
COMMENT ON ACCESS METHOD lsm IS 'LSM tree over B-tree levels';

-- Reuse every btree operator class, since each level is a btree.
CREATE OPERATOR CLASS int4_ops DEFAULT FOR TYPE int4 USING lsm
    AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
       FUNCTION 1 btint4cmp(int4,int4), FUNCTION 2 btint4sortsupport(internal),
       FUNCTION 4 btequalimage(oid);

CREATE OPERATOR CLASS int8_ops DEFAULT FOR TYPE int8 USING lsm
    AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
       FUNCTION 1 btint8cmp(int8,int8), FUNCTION 2 btint8sortsupport(internal),
       FUNCTION 4 btequalimage(oid);

CREATE OPERATOR CLASS text_ops DEFAULT FOR TYPE text USING lsm
    AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
       FUNCTION 1 bttextcmp(text,text), FUNCTION 2 bttextsortsupport(internal),
       FUNCTION 4 btvarstrequalimage(oid);

CREATE FUNCTION lsm_merge(index regclass) RETURNS bigint
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION lsm_top_sizes(index regclass) RETURNS bigint
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION lsm_base_size(index regclass) RETURNS bigint
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION lsm_index_size(index regclass) RETURNS bigint
    LANGUAGE sql AS $$ SELECT lsm_base_size($1) + lsm_top_sizes($1) $$;

CREATE FUNCTION lsm_stats(
    OUT index_oid oid, OUT heap_oid oid, OUT active_top int,
    OUT merge_pending bool, OUT n_inserts bigint, OUT n_merges bigint,
    OUT n_merged_tuples bigint, OUT last_merge_ms float8)
RETURNS SETOF record AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
