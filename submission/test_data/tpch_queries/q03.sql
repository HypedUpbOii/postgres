-- TPC-H Q3: Shipping Priority
-- 3-way join (customer, orders, lineitem); date predicates on both sides of orders.
-- Predicate columns: c_mktsegment, o_orderdate, l_shipdate, c_custkey, o_orderkey, l_orderkey
SELECT
    l_orderkey,
    sum(l_extendedprice * (1 - l_discount)) AS revenue,
    o_orderdate,
    o_shippriority
FROM customer, orders, lineitem
WHERE c_mktsegment = 'BUILDING'
  AND c_custkey   = o_custkey
  AND l_orderkey  = o_orderkey
  AND o_orderdate < date '1995-03-15'
  AND l_shipdate  > date '1995-03-15'
GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate
LIMIT 10;
