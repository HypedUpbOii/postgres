\echo Use "CREATE EXTENSION auto_index" to load this file. \quit

CREATE TABLE auto_index_catalog (
    relid                   oid             NOT NULL,
    indexrelid              oid             NOT NULL PRIMARY KEY,
    attno                   smallint        NOT NULL,
    created_at              timestamptz     NOT NULL DEFAULT now(),
    last_checked_idx_scan   bigint          NOT NULL DEFAULT 0,
    last_checked_at         timestamptz
);

CREATE FUNCTION auto_index_stats(
    OUT relid           oid,
    OUT attno           smallint,
    OUT equality_hits   bigint,
    OUT range_hits      bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'auto_index_stats'
LANGUAGE C STRICT;

CREATE FUNCTION auto_index_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'auto_index_reset'
LANGUAGE C STRICT;
