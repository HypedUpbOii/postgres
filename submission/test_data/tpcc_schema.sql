-- TPC-C-lite schema for auto_index OLTP testing.
-- Simplified — keeps only the tables/columns needed to exercise:
--   * mixed read/write workload (write-dominated like real TPC-C)
--   * a clear secondary-index opportunity (customer.c_last)
--   * a clear "don't index this" trap (frequent UPDATEs)
--
-- Primary keys only.  No secondary indexes — auto_index decides what to add.

DROP TABLE IF EXISTS order_line CASCADE;
DROP TABLE IF EXISTS orders     CASCADE;
DROP TABLE IF EXISTS new_order  CASCADE;
DROP TABLE IF EXISTS customer   CASCADE;
DROP TABLE IF EXISTS district   CASCADE;
DROP TABLE IF EXISTS warehouse  CASCADE;

CREATE TABLE warehouse (
    w_id      smallint        NOT NULL,
    w_name    varchar(10),
    w_ytd     numeric(12, 2),
    w_tax     numeric(4, 4),
    PRIMARY KEY (w_id)
);

CREATE TABLE district (
    d_w_id        smallint    NOT NULL,
    d_id          smallint    NOT NULL,
    d_name        varchar(10),
    d_ytd         numeric(12, 2),
    d_tax         numeric(4, 4),
    d_next_o_id   integer,
    PRIMARY KEY (d_w_id, d_id)
);

CREATE TABLE customer (
    c_w_id      smallint        NOT NULL,
    c_d_id      smallint        NOT NULL,
    c_id        integer         NOT NULL,
    c_first     varchar(16),
    c_last      varchar(16),
    c_balance   numeric(12, 2),
    c_credit    char(2),
    c_ytd_payment numeric(12, 2),
    c_payment_cnt smallint,
    PRIMARY KEY (c_w_id, c_d_id, c_id)
);

CREATE TABLE orders (
    o_w_id        smallint    NOT NULL,
    o_d_id        smallint    NOT NULL,
    o_id          integer     NOT NULL,
    o_c_id        integer     NOT NULL,
    o_entry_d     timestamp,
    o_carrier_id  smallint,
    o_ol_cnt      smallint,
    PRIMARY KEY (o_w_id, o_d_id, o_id)
);

CREATE TABLE order_line (
    ol_w_id       smallint    NOT NULL,
    ol_d_id       smallint    NOT NULL,
    ol_o_id       integer     NOT NULL,
    ol_number     smallint    NOT NULL,
    ol_i_id       integer,
    ol_quantity   smallint,
    ol_amount     numeric(6, 2),
    PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
);
