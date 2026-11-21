// covers: dyna:dataframe.DataFrame
// M1/M3: a column given as {codes: Int32Array, values: string[]} (the shape of
// dyna:csv CsvIndex.column(c, "dict")) becomes a string column without a JS
// string per row: codes are copied, each value is stored once, and group-by
// keys come back as the value strings. values must be distinct strings and
// every code must index one of them.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_dict_columns");
import { DataFrame } from "dyna:dataframe";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const J = JSON.stringify;

__kitS.section("a dictionary column is a str column", () => {
    const codes = new Int32Array([1, 0, 1, 2, 1]);
    const d = new DataFrame({ city: { codes, values: ["Oslo", "Lima", "Pune"] }, v: new Float64Array([1, 2, 3, 4, 5]) });
    codes[0] = 2;
    check(J(d.DTYPES()) === J({ city: "str", v: "f64" }), "a dictionary column is a str column");
    const s = d.GROUP_BY_SUM("city", "v");
    check(J(s.keys) === J(["Oslo", "Lima", "Pune"]) && J(Array.from(s.values)) === J([2, 9, 4]), "group keys are the dictionary strings: " + J(s));
    const plain = new DataFrame({ city: ["Lima", "Oslo", "Lima", "Pune", "Lima"], v: new Float64Array([1, 2, 3, 4, 5]) });
    const p = plain.GROUP_BY_SUM("city", "v");
    const m = new Map(p.keys.map((k, i) => [k, p.values[i]]));
    check(s.keys.every((k, i) => m.get(k) === s.values[i]), "same sums as the string[] column");
    check(d.GROUP_BY_COUNT("city").keys.length === 3, "codes are copied at construction (later writes are not seen)");
});
__kitS.section("a value no row uses is not a group key", () => {
    const d = new DataFrame({ k: { codes: new Int32Array([0, 0]), values: ["a", "unused"] }, v: new Float64Array([1, 2]) });
    check(J(d.GROUP_BY_SUM("k", "v").keys) === J(["a"]), "a value no row uses is not a group key");
    check(new DataFrame({ k: { codes: new Int32Array(0), values: [] } }).ROWS === 0, "an empty dictionary column");
});
__kitS.end();
check(err(() => new DataFrame({ a: { codes: new Int32Array([0, 2]), values: ["x", "y"] } })) === "RangeError: column 'a': codes[1] = 2 is outside the 2 dictionary values", "a code past the values is a RangeError");
check(err(() => new DataFrame({ a: { codes: new Int32Array([-1]), values: ["x"] } })).startsWith("RangeError"), "a negative code is a RangeError");
check(err(() => new DataFrame({ a: { codes: new Int32Array([0, 1]), values: ["x", "x"] } })) === "TypeError: column 'a': dictionary values[1] repeats values[0]", "repeated values are a TypeError");
check(err(() => new DataFrame({ a: { codes: new Uint32Array([0]), values: ["x"] } })).startsWith("TypeError"), "codes must be an Int32Array");
check(err(() => new DataFrame({ a: { codes: new Int32Array([0]), values: [7] } })).startsWith("TypeError"), "values must be strings");
check(err(() => new DataFrame({ a: { values: ["x"] } })).startsWith("TypeError"), "a missing codes array is a TypeError");
check(err(() => new DataFrame({ a: { codes: new Int32Array([0, 0]), values: ["x"] }, b: new Float64Array(3) })).startsWith("RangeError"), "length mismatch with another column");

print("test_dataframe_data_frame_dict_columns: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_dataframe_data_frame_dict_columns: " + failures + " failures");
