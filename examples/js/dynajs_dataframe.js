// dynajs_dataframe.js — dyna:dataframe, the frame-shaped verbs.
//
// A dataframe is columnar over TypedArrays: every method reduces, maps or
// selects columns, and — since round 6 — whole frames. This file walks the
// verbs that return a FRAME rather than a number or an array, because that is
// where the module's shape changed: JOIN, PIVOT, MELT, CONCAT, RESAMPLE and
// the select family, all returning a real DataFrame you can chain further.
//
// The one rule that makes all of this safe: a returned frame owns its data.
// Nothing aliases native memory, so a join's output can be fed to the next
// GROUP_BY_* without a lifetime thought.
//
// Run: dynajs examples/js/dynajs_dataframe.js
import { test, run, assert, assertEqual } from "./harness.js";
import { DataFrame } from "dyna:dataframe";

// Two tiny sales tables, the fixed cast for most of this file. Join keys must
// be INTEGER columns, so the customer is carried as an id, not a name.
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

// ---------------------------------------------------------------------------
// 1. INTEROP — asking a frame what it holds, and getting data back out.
test("interop", () => {
  const types = orders.DTYPES();
  assertEqual(types.customer_id, "i32", "an Int32Array column is i32");
  assertEqual(types.amount, "f64", "a Float64Array column is f64");

  const info = orders.INFO();
  assertEqual(info.rows, 5, "five orders");
  assertEqual(info.cols, 4, "four columns");
  assert(info.total_bytes > 0, "INFO reports a byte total");

  // TO_RECORDS is the escape hatch: an array of plain row objects. It is also
  // the natural oracle for every frame verb below.
  const rec = orders.TO_RECORDS();
  assertEqual(rec.length, 5, "one record per row");
  assertEqual(rec[0].customer_id, 1, "record fields by column name");

  // TO_JSON / TO_CSV round-trip through a text form. NaN in a JSON output is
  // null, and in CSV it is an empty field — the engine's own conventions.
  const asJson = orders.TO_JSON();
  assert(asJson.startsWith('[{'), "TO_JSON is a JSON array of records");
  const asCsv = orders.TO_CSV();
  assert(asCsv.startsWith("order_id,customer_id,amount,at_day\n"), "CSV header first");

  // A fresh frame from an array of objects: types are inferred per column.
  const rebuilt = orders.FROM_RECORDS([
    { x: 1, tag: "a" }, { x: 2, tag: "b" },
  ]);
  assertEqual(rebuilt.COLUMNS.join(","), "x,tag", "FROM_RECORDS union of keys");
  assertEqual(rebuilt.ROWS, 2, "FROM_RECORDS row count");
});

// ---------------------------------------------------------------------------
// 2. SELECT / FILTER — the verbs every analyst reaches for first.
test("select and filter", () => {
  // SELECT reorders; DROP_COLUMNS removes; RENAME renames in place.
  const slim = orders.SELECT(["customer_id", "amount"]);
  assertEqual(slim.COLUMNS.join(","), "customer_id,amount", "SELECT keeps the order given");

  const noCountry = customers.DROP_COLUMNS(["country"]);
  assertEqual(noCountry.COLUMNS.join(","), "id,name,since_day", "DROP_COLUMNS complement");

  const renamed = orders.RENAME({ amount: "total" });
  assertEqual(renamed.COLUMNS.join(","), "order_id,customer_id,total,at_day", "RENAME");

  // FILTER is the whole point of a boolean column: keep rows where the mask
  // is 1. The comparison methods produce the mask.
  const big = orders.FILTER(orders.GE("amount", 100));
  assertEqual(big.ROWS, 3, "three orders >= 100");
  assertEqual(big.TO_RECORDS()[0].order_id, 101, "the biggest first in row order");

  // SAMPLE with a seed is reproducible — the requirement that makes it useful
  // for science rather than a coin toss.
  const s1 = orders.SAMPLE(2, 7);
  const s2 = orders.SAMPLE(2, 7);
  assertEqual(s1.TO_RECORDS()[0].order_id, s2.TO_RECORDS()[0].order_id,
              "a seeded sample is reproducible");
});

// ---------------------------------------------------------------------------
// 3. JOIN — the row multiplier. This is where the frame contract earns its keep.
test("join", () => {
  // An inner join on the customer id. A duplicate key on the right multiplies
  // rows: the result is still a frame, still chainable.
  const joined = orders.JOIN(customers, "customer_id", "id");
  assertEqual(joined.ROWS, 5, "every order matched a customer");
  assertEqual(joined.COLUMNS.join(","),
              "order_id,customer_id,amount,at_day,id,name,country,since_day",
              "right columns carried; the colliding 'id' gets _right");

  // A left join keeps every order even if a customer is missing from the table,
  // and NaN fills the missing right side. The right side must be numeric: a
  // string column there would have no value for a missing row, so it is refused.
  const countries = new DataFrame({
    id: Int32Array.from([1, 2, 3, 9]),
    code: Int32Array.from([44, 1, 49, 0]),
  });
  const left = orders.JOIN(countries, "customer_id", "id", "left");
  assertEqual(left.ROWS, 5, "left join keeps every left row");
  const known = left.FILTER(left.NOT_NA("code"));
  assertEqual(known.ROWS, 5, "every order has a known customer (ids 1,2,3 all present)");
  assertEqual(left.TO_RECORDS()[4].code, 1, "order 105 (customer 2) has country code 1");

  // ASOF_JOIN matches the nearest PRECEDING time, not equality — a temporal
  // join that a plain equality join cannot express. Both sides must be sorted
  // ascending, and the carried right side numeric.
  const events = new DataFrame({
    day: Int32Array.from([2, 4, 6]),
    sales: Float64Array.from([50, 60, 70]),
  });
  const asof = customers.ASOF_JOIN(events, "since_day", "day");
  assertEqual(asof.ROWS, 3, "one asof row per customer");
  assertEqual(asof.TO_RECORDS()[0].sales, NaN, "customer since day 0 precedes the first event");
  assertEqual(asof.TO_RECORDS()[2].sales, 50, "customer since day 2 gets the nearest PRECEDING event (day 2)");
});

// ---------------------------------------------------------------------------
// 4. PIVOT / MELT — the wide/long inverse pair.
test("pivot and melt", () => {
  // PIVOT turns (index, column) pairs into a matrix. The output width is the
  // number of DISTINCT pivot values — data-dependent, and bounded internally.
  const wide = orders.PIVOT("at_day", "customer_id", "amount", "sum");
  assertEqual(wide.COLUMNS.join(","), "at_day,1,2,3", "one column per customer id");
  assertEqual(wide.ROWS, 4, "one row per distinct day (1,2,3,5)");

  // MELT reverses it: a matrix back into (id, variable, value) triples.
  const mat = new DataFrame({
    day: Int32Array.from([1, 2]),
    ada: Float64Array.from([120, 80]),
    bob: Float64Array.from([340, 95]),
  });
  const long = mat.MELT(["day"], ["ada", "bob"]);
  assertEqual(long.ROWS, 4, "rows x value-vars");
  assertEqual(long.COLUMNS.join(","), "day,variable,value", "long form column names");
});

// ---------------------------------------------------------------------------
// 5. CONCAT — stacking frames with an identical schema.
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
  // A mismatched schema is refused, not filled.
  try {
    orders.CONCAT(new DataFrame({ order_id: Int32Array.from([1]), nope: Float64Array.from([1]) }));
    assert(false, "CONCAT must refuse a different column set");
  } catch (e) {
    assert(String(e.message).includes("match"), "CONCAT refuses mismatched columns");
  }
});

// ---------------------------------------------------------------------------
// 6. RESAMPLE — time into buckets.
test("resample", () => {
  const daily = new DataFrame({
    day: Float64Array.from([1, 2, 11, 12, 21]),
    sales: Float64Array.from([5, 6, 7, 8, 9]),
  });
  // Buckets of width 10 over the day column; only occupied buckets appear.
  const byDecade = daily.RESAMPLE("day", 10, "sum");
  assertEqual(byDecade.COLUMNS.join(","), "bucket,value", "bucket start + aggregate");
  assertEqual(byDecade.ROWS, 3, "three occupied decades");
  assertEqual(byDecade.TO_RECORDS()[1].bucket, 10, "the decade start, not the row day");
});

// ---------------------------------------------------------------------------
// 7. JSON_AGG — a grouped column straight to a JSON string.
test("json_agg", () => {
  // Per customer, their amounts as a JSON array.
  const byCustomer = orders.JSON_AGG("customer_id", "amount");
  const parsed = JSON.parse(byCustomer);
  assertEqual(parsed["1"].join(","), "120,80", "customer 1's orders in row order");
  assertEqual(parsed["2"].join(","), "340,95", "customer 2's orders");

  // An object form keeps the LAST value of a duplicate key, and a "__proto__"
  // key is defined as an OWN property (no prototype-chain surprise).
  const obj = orders.JSON_OBJECT_AGG("customer_id", "amount");
  assertEqual(JSON.parse(obj)["2"], 95, "duplicate key keeps the last value");
});

run();
