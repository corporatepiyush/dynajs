// covers: dyna:dataframe.DataFrame
// {out: name} on the elementwise, fill and window verbs creates the column when the frame has none by that
// name (owner ruling, B1 follow-up): before, out had to name an existing Float64Array column, so a computed
// column (rev = qty * price) could only overwrite an input, and there was no verb that adds one. The new
// column is f64, appended after the existing ones, and nothing is added when the verb throws.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_out_new");
import { DataFrame } from "dyna:dataframe";
import { parseDataFrame } from "dyna:csv";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const err = (f) => { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } };
const fresh = () => new DataFrame({ k: ["a", "b", "a"], qty: new Int32Array([2, 3, 4]), price: new Float64Array([1.5, 2, 10]) });

__kitS.section("MUL {out} with a new name returns the frame", () => {
    const df = fresh();
    check(df.MUL("price", "qty", { out: "rev" }) === df, "MUL {out} with a new name returns the frame");
    eq(df.COLUMNS, ["k", "qty", "price", "rev"], "the new column is appended");
    eq(df.DTYPES().rev, "f64", "and is f64");
    eq(Array.from(df.TO_COLUMNS().rev), [3, 6, 40], "and holds price * qty");
    eq(Array.from(df.TO_COLUMNS().price), [1.5, 2, 10], "the inputs are untouched");
    eq(Array.from(df.GROUP_BY_SUM("k", "rev").values), [43, 6], "and the new column aggregates");
    df.ADD("rev", 1, { out: "rev" });
    eq(Array.from(df.TO_COLUMNS().rev), [4, 7, 41], "an existing f64 out column is still written in place");
});
__kitS.section("every {out} family creates the column: map, CLIP, FILL_NA, ROLLING_SUM, ROLLING_STD, RSUB", () => {
    const df = fresh();
    df.ABS("price", { out: "a1" });
    df.CLIP("price", 0, 5, { out: "a2" });
    df.FILL_NA("price", 0, { out: "a3" });
    df.ROLLING_SUM("price", 2, { out: "a4" });
    df.ROLLING_STD("price", 2, { out: "a5" });
    df.RSUB("price", 10, { out: "a6" });
    eq(df.COLUMNS.slice(3), ["a1", "a2", "a3", "a4", "a5", "a6"], "every {out} family creates the column: map, CLIP, FILL_NA, ROLLING_SUM, ROLLING_STD, RSUB");
    eq(Array.from(df.TO_COLUMNS().a2), [1.5, 2, 5], "CLIP into a new column");
    eq(Array.from(df.TO_COLUMNS().a6), [8.5, 8, 0], "RSUB into a new column");
});
__kitS.section("a verb that throws throws as without out", () => {
    const df = fresh();
    eq(err(() => df.MUL("k", 2, { out: "bad" })), err(() => fresh().MUL("k", 2)), "a verb that throws throws as without out");
    eq(df.COLUMNS, ["k", "qty", "price"], "and adds no column");
    eq(err(() => df.MUL("price", 2, { out: "k" })).replace(/:.*/, ""), "TypeError", "an existing non-f64 out column is still refused");
    eq(err(() => df.MUL("price", 2, { out: "a\0b" })), "RangeError: column name contains a NUL character", "a NUL in the new name is refused");
    eq(df.COLUMNS, ["k", "qty", "price"], "and adds no column");
});
__kitS.section("columns can be added up to the 1024-column cap", () => {
    let df = new DataFrame({ c0: new Float64Array(1) });
    for (let i = 1; i < 1024; i++) df.ADD("c0", i, { out: "c" + i });
    eq(df.COLS, 1024, "columns can be added up to the 1024-column cap");
    check(/^RangeError: .*1024/.test(err(() => df.ADD("c0", 1, { out: "over" }))), "and the 1025th is a RangeError naming the cap");
    eq(df.COLS, 1024, "which adds nothing");
});
__kitS.section("a frame from parseDataFrame takes a computed column", () => {
    const df = parseDataFrame("region,qty,price\nr1,2,3\nr2,1,5\nr1,4,1\n");
    df.MUL("price", "qty", { out: "rev" });
    eq(Array.from(df.GROUP_BY_SUM("region", "rev").values), [10, 5], "a frame from parseDataFrame takes a computed column");
});
__kitS.end();
print("test_dataframe_out_new: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_dataframe_out_new: " + fails + " failures");
