// flags: --std
import { test, run, assert, assertEqual } from "./harness.js";
import { DataFrame } from "dyna:dataframe";

const orders = new DataFrame({
  order_id: Int32Array.from([101, 102, 103, 104, 105]),
  customer_id: Int32Array.from([1, 2, 1, 3, 2]),
  amount: Float64Array.from([120, 340, 80, 200, 95]),
  at_day: Int32Array.from([1, 1, 2, 3, 5]),
});

const customers = new DataFrame({
  id: Int32Array.from([1, 2, 3]),
  name: ["ada", "bob", "cyn"],
  country: ["uk", "us", "de"],
  since_day: Int32Array.from([0, 0, 2]),
});

test("interop", () => {
  const types = orders.DTYPES();
  assertEqual(types.customer_id, "i32", "an Int32Array column is i32");
  assertEqual(types.amount, "f64", "a Float64Array column is f64");

  const info = orders.INFO();
  assertEqual(info.rows, 5, "five orders");
  assertEqual(info.cols, 4, "four columns");
  assert(info.total_bytes > 0, "INFO reports a byte total");

  const rec = orders.TO_RECORDS();
  assertEqual(rec.length, 5, "one record per row");
  assertEqual(rec[0].customer_id, 1, "record fields by column name");

  const asJson = orders.TO_JSON();
  assert(asJson.startsWith('[{'), "TO_JSON is a JSON array of records");
  const asCsv = orders.TO_CSV();
  assert(asCsv.startsWith("order_id,customer_id,amount,at_day\n"), "CSV header first");

  const rebuilt = orders.FROM_RECORDS([
    { x: 1, tag: "a" }, { x: 2, tag: "b" },
  ]);
  assertEqual(rebuilt.COLUMNS.join(","), "x,tag", "FROM_RECORDS union of keys");
  assertEqual(rebuilt.ROWS, 2, "FROM_RECORDS row count");
});

test("select and filter", () => {
  const slim = orders.SELECT(["customer_id", "amount"]);
  assertEqual(slim.COLUMNS.join(","), "customer_id,amount", "SELECT keeps the order given");

  const noCountry = customers.DROP_COLUMNS(["country"]);
  assertEqual(noCountry.COLUMNS.join(","), "id,name,since_day", "DROP_COLUMNS complement");

  const renamed = orders.RENAME({ amount: "total" });
  assertEqual(renamed.COLUMNS.join(","), "order_id,customer_id,total,at_day", "RENAME");

  const big = orders.FILTER(orders.GE("amount", 100));
  assertEqual(big.ROWS, 3, "three orders >= 100");
  assertEqual(big.TO_RECORDS()[0].order_id, 101, "the biggest first in row order");

  const s1 = orders.SAMPLE(2, 7);
  const s2 = orders.SAMPLE(2, 7);
  assertEqual(s1.TO_RECORDS()[0].order_id, s2.TO_RECORDS()[0].order_id,
              "a seeded sample is reproducible");
});

test("join", () => {
  const joined = orders.JOIN(customers, "customer_id", "id");
  assertEqual(joined.ROWS, 5, "every order matched a customer");
  assertEqual(joined.COLUMNS.join(","),
              "order_id,customer_id,amount,at_day,id,name,country,since_day",
              "right columns carried; the colliding 'id' gets _right");

  const countries = new DataFrame({
    id: Int32Array.from([1, 2, 3, 9]),
    code: Int32Array.from([44, 1, 49, 0]),
  });
  const left = orders.JOIN(countries, "customer_id", "id", "left");
  assertEqual(left.ROWS, 5, "left join keeps every left row");
  const known = left.FILTER(left.NOT_NA("code"));
  assertEqual(known.ROWS, 5, "every order has a known customer (ids 1,2,3 all present)");
  assertEqual(left.TO_RECORDS()[4].code, 1, "order 105 (customer 2) has country code 1");

  const events = new DataFrame({
    day: Int32Array.from([2, 4, 6]),
    sales: Float64Array.from([50, 60, 70]),
  });
  const asof = customers.ASOF_JOIN(events, "since_day", "day");
  assertEqual(asof.ROWS, 3, "one asof row per customer");
  assertEqual(asof.TO_RECORDS()[0].sales, NaN, "customer since day 0 precedes the first event");
  assertEqual(asof.TO_RECORDS()[2].sales, 50, "customer since day 2 gets the nearest PRECEDING event (day 2)");
});

test("pivot and melt", () => {
  const wide = orders.PIVOT("at_day", "customer_id", "amount", "sum");
  assertEqual(wide.COLUMNS.join(","), "at_day,1,2,3", "one column per customer id");
  assertEqual(wide.ROWS, 4, "one row per distinct day (1,2,3,5)");

  const mat = new DataFrame({
    day: Int32Array.from([1, 2]),
    ada: Float64Array.from([120, 80]),
    bob: Float64Array.from([340, 95]),
  });
  const long = mat.MELT(["day"], ["ada", "bob"]);
  assertEqual(long.ROWS, 4, "rows x value-vars");
  assertEqual(long.COLUMNS.join(","), "day,variable,value", "long form column names");
});

test("concat", () => {
  const more = new DataFrame({
    order_id: Int32Array.from([106]),
    customer_id: Int32Array.from([1]),
    amount: Float64Array.from([60]),
    at_day: Int32Array.from([6]),
  });
  const all = orders.CONCAT(more);
  assertEqual(all.ROWS, 6, "CONCAT stacks rows");
  assertEqual(all.TO_RECORDS()[5].amount, 60, "the appended row is last");
  try {
    orders.CONCAT(new DataFrame({ order_id: Int32Array.from([1]), nope: Float64Array.from([1]) }));
    assert(false, "CONCAT must refuse a different column set");
  } catch (e) {
    assert(String(e.message).includes("match"), "CONCAT refuses mismatched columns");
  }
});

test("resample", () => {
  const daily = new DataFrame({
    day: Float64Array.from([1, 2, 11, 12, 21]),
    sales: Float64Array.from([5, 6, 7, 8, 9]),
  });
  const byDecade = daily.RESAMPLE("day", 10, "sum");
  assertEqual(byDecade.COLUMNS.join(","), "bucket,value", "bucket start + aggregate");
  assertEqual(byDecade.ROWS, 3, "three occupied decades");
  assertEqual(byDecade.TO_RECORDS()[1].bucket, 10, "the decade start, not the row day");
});

test("json_agg", () => {
  const byCustomer = orders.JSON_AGG("customer_id", "amount");
  const parsed = JSON.parse(byCustomer);
  assertEqual(parsed["1"].join(","), "120,80", "customer 1's orders in row order");
  assertEqual(parsed["2"].join(","), "340,95", "customer 2's orders");

  const obj = orders.JSON_OBJECT_AGG("customer_id", "amount");
  assertEqual(JSON.parse(obj)["2"], 95, "duplicate key keeps the last value");
});

run();
