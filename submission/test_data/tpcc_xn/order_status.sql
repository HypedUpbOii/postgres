-- TPC-C OrderStatus (simplified): read-only.
-- One query intentionally omits the warehouse/district from the WHERE
-- clause, simulating an app that searches "globally" by last name.
-- That forces a SeqScan that auto_index can actually observe.
\set wh    random(1, :scale)
\set dist  random(1, 10)
\set cust  random(1, 3000)

-- Global customer lookup by last name — SEQSCAN target for auto_index.
SELECT c_w_id, c_d_id, c_id, c_balance
  FROM customer
 WHERE c_last = (ARRAY['BARBAR','OUGHTBAR','ABLEBAR','PRIBAR','PRESBAR'])
                  [random(1, 5)];

-- Find recent orders for this customer across the district — SEQSCAN
-- because o_c_id is not part of the orders PK.
SELECT max(o_id)
  FROM orders
 WHERE o_c_id = :cust;
