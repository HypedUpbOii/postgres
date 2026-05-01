-- TPC-C NewOrder (simplified): inserts orders + order_lines, increments
-- district counter.  Heavy on write traffic to several tables.
\set wh    random(1, :scale)
\set dist  random(1, 10)
\set cust  random(1, 3000)

-- Get and bump the district's next order ID.
UPDATE district
   SET d_next_o_id = d_next_o_id + 1
 WHERE d_w_id = :wh AND d_id = :dist
RETURNING d_next_o_id - 1 AS oid \gset

INSERT INTO orders (o_w_id, o_d_id, o_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt)
VALUES (:wh, :dist, :oid, :cust, now(), NULL, 5);

INSERT INTO order_line (ol_w_id, ol_d_id, ol_o_id, ol_number,
                        ol_i_id, ol_quantity, ol_amount)
SELECT :wh, :dist, :oid, n,
       (random()*100000)::int + 1,
       (random()*10)::int + 1,
       (random()*100)::numeric(6,2)
FROM generate_series(1, 5) n;
