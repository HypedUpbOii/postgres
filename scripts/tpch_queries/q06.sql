-- TPC-H Q6: Forecasting Revenue Change
-- Single-table aggregate; multiple range filters on lineitem.
-- Predicate columns: l_shipdate (range), l_discount (range), l_quantity (range).
SELECT
    sum(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= date '1994-01-01'
  AND l_shipdate <  date '1994-01-01' + interval '1 year'
  AND l_discount BETWEEN 0.06 - 0.01 AND 0.06 + 0.01
  AND l_quantity < 24;
