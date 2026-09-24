/* churn_df_all.js -- exhaustive DataFrame churn: every method on the
 * prototype is called with a battery of argument shapes (valid, boundary and
 * invalid), 10k+ operations per run, then exit. Under ASan+LSan a missing
 * free on ANY path -- success or refusal -- shows up as an exit leak.
 *
 * Exceptions are EXPECTED (this drives the error paths on purpose); what is
 * never acceptable is a leak, a crash or a hang. */
import { DataFrame } from "dyna:dataframe";

const f64 = (...a) => new Float64Array(a);
const f32 = (...a) => new Float32Array(a);
const i32 = (...a) => new Int32Array(a);
const u32 = (...a) => new Uint32Array(a);
const i16 = (...a) => new Int16Array(a);
const u16 = (...a) => new Uint16Array(a);
const i8 = (...a) => new Int8Array(a);
const u8 = (...a) => new Uint8Array(a);

const N = 8;
const df = new DataFrame({
    a: f64(1, 2, 3, 4, 5, 6, 7, 8),
    b: f64(2, 1, 4, 3, 6, 5, 8, 7),
    t: f64(0, 1, 2, 3, 10, 11, 12, 13),   // resample-time (sorted)
    tu: f64(0, 5, 1, 2, 3, 4, 6, 7),      // unsorted
    s: ["x", "y", "x", "z", "x", "y", "z", "x"],
    f32c: f32(1, 2, 3, 4, 5, 6, 7, 8),
    i32c: i32(1, -2, 3, -4, 5, -6, 7, -8),
    u32c: u32(1, 2, 3, 4, 5, 6, 7, 8),
    i16c: i16(1, -2, 3, -4, 5, -6, 7, -8),
    u16c: u16(1, 2, 3, 4, 5, 6, 7, 8),
    i8c: i8(1, -2, 3, -4, 5, -6, 7, -8),
    u8c: u8(1, 2, 3, 4, 5, 6, 7, 8),
    nan: f64(1, NaN, 3, Infinity, -Infinity, 6, 7, 8),
});
const df2 = new DataFrame({
    a: f64(1, 2, 3), b: f64(9, 8, 7), s: ["x", "q", "y"], k: f64(0, 1, 2),
});
const dfEmpty = new DataFrame({ a: f64(), t: f64(), s: [] });
const mask8 = u8(1, 0, 1, 0, 1, 0, 1, 0);
const mask3 = u8(1, 0, 1);
const srt = new DataFrame({ t: f64(0, 1, 2, 3, 10, 11, 12, 13),
                            a: f64(1, 2, 3, 4, 5, 6, 7, 8) });

const frames = [df, df2, dfEmpty, srt];
const cols = ["a", "b", "t", "tu", "s", "f32c", "i32c", "u32c", "i16c", "u16c",
              "i8c", "u8c", "nan", "nope", 0, 1, 2, -1, 99, "", null, undefined];
const vals = [0, 1, 2.5, -1, NaN, Infinity, 1e308, "x", "", true, false, null,
              undefined, {}, [], [1, 2], [1, 2, 3], f64(1, 0, 1), f64(7, 7, 7),
              f32(1, 2), i32(3), 100, -100, 0.5];
const masks = [mask8, mask3, u8(), null, undefined, f64(1), "m"];
const bags = [{ out: "a" }, { out: "nope" }, { out: "s" }, { bad: 1 }, {},
              { out: "a", x: 2 }];

/* Argument batteries: one array of tuples per arity "style". */
const argsets = [];
for (const c of cols) {
    argsets.push([c]);
    for (const v of vals.slice(0, 12)) argsets.push([c, v]);
    for (const v of vals.slice(0, 6)) argsets.push([c, v, v]);
    for (const b of bags) argsets.push([c, b]);
    argsets.push([c, 0, 0]);
    argsets.push([c, 1, 2, 3]);
    argsets.push([c, "sum"]);
    argsets.push([c, "mean"]); argsets.push([c, "min"]); argsets.push([c, "max"]);
    argsets.push([c, "count"]); argsets.push([c, "first"]); argsets.push([c, "last"]);
    argsets.push([c, 2]); argsets.push([c, -1]); argsets.push([c, 0.5]);
    argsets.push([c, 2, mask8]);
    argsets.push([c, 2, 0.25]);
    argsets.push([c, c]);
    argsets.push([c, [1, 2, 3]]);
    argsets.push([c, df2]);
    for (const m of masks) argsets.push([c, m]);
    argsets.push([c, c, c]);
    argsets.push([c, c, "sum"]);
    argsets.push([c, 2, c]);
    argsets.push([c, f64(1, 2, 3)]);
    argsets.push([c, 0.1, 0.9]);
    argsets.push([c, ["a", "b"]]);
}
/* verb-specific shapes */
argsets.push([f64(1, 0, 1, 0, 1, 0, 1, 0)]);
argsets.push([["a", "b"], ["a"]]);
argsets.push([[1, 2], [3, 4]]);
argsets.push([{ x: [1, 2], y: [3, 4] }, { x: [5], y: [6] }]);
argsets.push([[{ a: 1, b: 2 }, { a: 3, b: 4 }]]);
argsets.push([df2]);
argsets.push([df2, "a", "b"]); argsets.push([df2, "a", "b", "inner"]);
argsets.push([df2, "a", "b", "left"]); argsets.push([df2, "a", "b", "right"]);
argsets.push([df2, "a", "b", "outer"]); argsets.push([df2, "a", "b", "cross"]);
argsets.push([df2, "k", "t"]); argsets.push([srt, "k", "t"]);
argsets.push([[df, df2]]); argsets.push([[df]]); argsets.push([[]]);
argsets.push(["t", 2, "sum"]); argsets.push(["t", 2, "mean"]);
argsets.push(["t", 2, "min"]); argsets.push(["t", 2, "max"]);
argsets.push(["t", 2, "count"]); argsets.push(["t", 2, "last"]);
argsets.push(["tu", 2, "sum"]);                    // unsorted: the refusal path
argsets.push(["a", 2, "sum"]); argsets.push(["s", 2, "sum"]);
argsets.push(["t", 0, "sum"]); argsets.push(["t", -1, "sum"]);
argsets.push(["t", NaN, "sum"]); argsets.push(["t", 2, "bogus"]);
argsets.push(["a", "b", "t"]); argsets.push(["a", "b", "t", "sum"]);
argsets.push(["a", "b", "t", "mean"]); argsets.push(["a", "s", "a", "first"]);
argsets.push(["a", "b", "nan", "last"]); argsets.push(["s", "a", "a", "count"]);
argsets.push(["a", "b", "t", "bogus"]);
argsets.push([["a", "b"]]); argsets.push([["a", "b"], ["x"]]);
argsets.push([["a"], ["b"], ["t"]]);
argsets.push(["a", 2, "b", 3, "count"]);
argsets.push(["a", f64(0.5)]); argsets.push(["a", f64(0, 1)]);
argsets.push(["a", 3]); argsets.push(["a", 3, 0.5]);
argsets.push(["a", 4]); argsets.push(["t", 3, f64(1)]);
argsets.push(["a", "b", 1, 1]); argsets.push(["t", "a", 1, 1]);
argsets.push(["a", 2, 3]); argsets.push(["a", 2, 3, 4]);
argsets.push(["a", 2, f64(1, 2)]); argsets.push(["a", f64(1, 2), f64(1, 2)]);
argsets.push(["a", "b", f64(1, 1), f64(2, 2)]);
argsets.push([["a", "b", "t"], ["s"]]);
argsets.push([{ group: "s", value: "a", how: "sum" }]);
argsets.push([mask8]);
argsets.push([["a", "b", "t", "nan"], 2]);
argsets.push(["s", ["x", "z"]]);
argsets.push(["a", [1, 5]]);
argsets.push(["a", 2, "b", 2, 2]);

const names = Object.getOwnPropertyNames(DataFrame.prototype)
    .filter(n => n !== "constructor" && n !== "toJSON");
console.log("methods:", names.length, "argsets:", argsets.length);

let ops = 0, thrown = 0;
function drive(obj) {
    for (const name of names) {
        const fn = obj[name];
        if (typeof fn !== "function") continue;
        for (const args of argsets) {
            ops++;
            try { fn.apply(obj, args); } catch (e) { thrown++; }
        }
    }
}
for (const f of frames) drive(f);
console.log("churn ops:", ops, "throws:", thrown);
