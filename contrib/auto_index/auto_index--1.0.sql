\echo Use "CREATE EXTENSION auto_index" to load this file. \quit

CREATE TABLE auto_index_catalog (
    relid                   oid             NOT NULL,
    indexrelid              oid             NOT NULL PRIMARY KEY,
    attnos                  smallint[]      NOT NULL,    -- length 1 = singleton, 2-3 = composite
    created_at              timestamptz     NOT NULL DEFAULT now(),
    last_checked_idx_scan   bigint          NOT NULL DEFAULT 0,
    last_checked_at         timestamptz,
    idle_write_cost         bigint          NOT NULL DEFAULT 0
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

-- Helper: turn an attnos[] array into a comma-separated column-name list
-- for the relation, preserving the array order.  Used by the test scripts
-- to display singletons and composites uniformly.
CREATE FUNCTION auto_index_attnames(rel oid, attnos smallint[])
RETURNS text
LANGUAGE sql STABLE AS $$
    SELECT string_agg(a.attname, ', ' ORDER BY u.ord)
    FROM unnest(attnos) WITH ORDINALITY AS u(attno, ord)
    JOIN pg_attribute a ON a.attrelid = rel AND a.attnum = u.attno
$$;
