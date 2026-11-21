// covers: dyna:dataframe.DataFrame
// GROUP_BY_AGG(key, val, aggs, mask?): several grouped aggregates in one pass.
// Contract (dynajs.d.ts): each field equals the matching single GROUP_BY_* verb
// for the same key/val/mask; fields appear exactly for the requested aggs, in
// the fixed order sum, mean, count, min, max; an empty, unknown, repeated or
// non-string aggregate name is a TypeError. NaN: sum/mean propagate, count
// counts the row, min/max ignore it (a group whose values are all NaN has NaN
// min/max); an empty group is sum 0, mean NaN, count 0, min/max NaN.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_group_agg");
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
const str = (a) => JSON.stringify(Array.from(a, (x) => Object.is(x, -0) ? "-0" : String(x)));
function same(a, b, m) { const x = str(a), y = str(b); ok(x === y, m + "  (got " + x + ", want " + y + ")"); }
function throwsType(f, re, m) {
    try { f(); ok(false, m + ": no throw"); } catch (e) { ok(e instanceof TypeError && re.test(e.message), m + ": " + e); }
}

const ALL = ["sum", "mean", "count", "min", "max"];
const SINGLE = { sum: "GROUP_BY_SUM", mean: "GROUP_BY_MEAN", min: "GROUP_BY_MIN", max: "GROUP_BY_MAX" };
function single(df, agg, key, val, mask) {
    if (agg === "count") return mask ? df.GROUP_BY_COUNT(key, mask) : df.GROUP_BY_COUNT(key);
    return mask ? df[SINGLE[agg]](key, val, mask) : df[SINGLE[agg]](key, val);
}
function check(df, key, val, mask, tag) {
    const r = df.GROUP_BY_AGG(key, val, ALL, mask);
    for (const a of ALL) {
        const s = single(df, a, key, val, mask);
        same(r[a], s.values, tag + " " + a + " equals the single verb");
        ok(JSON.stringify(r.keys) === JSON.stringify(s.keys), tag + " " + a + " keys equal the single verb");
    }
}

__kitS.section("int key", () => {
    const k = new Int32Array([0, 0, 1, 3, 3, 4, 6]);
    const v = new Float64Array([1, NaN, NaN, 2, 5, -0, 7]);
    const s = ["a", "b", "a", "c", "c", "a", "b"];
    const i8 = new Int8Array([5, -3, 2, 2, 9, 0, 1]);
    const df = new DataFrame({ k, v, s, i8 });
    check(df, "k", "v", undefined, "int key");
    check(df, "s", "v", undefined, "string key");
    check(df, "k", "i8", undefined, "int8 values");
    const mask = new Uint8Array([1, 0, 1, 1, 0, 1, 0]);
    check(df, "k", "v", mask, "masked");
    check(df, "s", "i8", mask, "masked string key");

    const r = df.GROUP_BY_AGG("k", "v", ["max", "count", "sum"]);
    ok(JSON.stringify(Object.keys(r)) === JSON.stringify(["keys", "sum", "count", "max"]), "fields are keys then the requested aggs in fixed order: " + Object.keys(r));
    ok(r.sum instanceof Float64Array && r.count instanceof Float64Array, "fields are Float64Array");
    const one = df.GROUP_BY_AGG("k", "v", ["mean"]);
    ok(JSON.stringify(Object.keys(one)) === JSON.stringify(["keys", "mean"]), "one agg gives exactly one field");

    // NaN handling: group 0 = [1, NaN], group 1 = [NaN] only
    const n = df.GROUP_BY_AGG("k", "v", ALL);
    ok(Number.isNaN(n.sum[0]) && Number.isNaN(n.mean[0]), "a NaN value propagates into sum and mean");
    ok(n.count[0] === 2 && n.count[1] === 1, "count counts a NaN row");
    ok(n.min[0] === 1 && n.max[0] === 1, "min/max skip a NaN value");
    ok(Number.isNaN(n.min[1]) && Number.isNaN(n.max[1]), "an all-NaN group has NaN min and max");
    ok(Number.isNaN(df.GROUP_BY_MIN("k", "v").values[1]) && Number.isNaN(df.GROUP_BY_MAX("k", "v").values[1]) && Number.isNaN(df.MIN_MAP("k", "v").values[1]),
        "GROUP_BY_MIN/MAX (and MIN_MAP) give NaN for an all-NaN group, as the d.ts says");

    // empty groups: key 2 and 5 hold no row; key 6 is masked out below
    ok(n.sum[2] === 0 && Number.isNaN(n.mean[2]) && n.count[2] === 0 && Number.isNaN(n.min[2]) && Number.isNaN(n.max[2]),
        "an empty group is sum 0, mean NaN, count 0, min NaN, max NaN");
    const m = df.GROUP_BY_AGG("k", "v", ALL, mask);
    ok(m.sum[6] === 0 && m.count[6] === 0 && Number.isNaN(m.mean[6]) && Number.isNaN(m.min[6]), "a group the mask empties is an empty group");
    ok(Object.is(n.min[4], -0) && Object.is(n.max[4], -0), "-0 survives min/max");

    throwsType(() => df.GROUP_BY_AGG("k", "v", []), /empty/, "empty aggs");
    throwsType(() => df.GROUP_BY_AGG("k", "v", ["sum", "median"]), /unknown aggregate 'median'/, "unknown agg");
    throwsType(() => df.GROUP_BY_AGG("k", "v", ["min", "sum", "min"]), /'min' is listed twice/, "duplicate agg");
    throwsType(() => df.GROUP_BY_AGG("k", "v", ["sum", 3]), /aggs\[1\] is not a string/, "non-string agg");
    throwsType(() => df.GROUP_BY_AGG("k", "v", "sum"), /must be an array/, "aggs not an array");
    throwsType(() => df.GROUP_BY_AGG("k", "v"), /must be an array/, "aggs missing");
    throwsType(() => df.GROUP_BY_AGG("k", "s", ["sum"]), /string column/, "string value column");
    throwsType(() => df.GROUP_BY_AGG("v", "k", ["sum"]), /integer or string/, "float key");
});

__kitS.section("random u16 key / f32 values", () => {
    let seed = 17;
    const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return (seed >>> 8) % n; };
    const N = 5000;
    const k = new Uint16Array(N), v = new Float32Array(N), s = [], mask = new Uint8Array(N);
    const names = ["x", "y", "z", "w"];
    for (let i = 0; i < N; i++) { k[i] = rnd(300); v[i] = rnd(10) ? rnd(1000) / 8 - 40 : NaN; s.push(names[rnd(3)]); mask[i] = rnd(3) ? 1 : 0; }
    const df = new DataFrame({ k, v, s });
    check(df, "k", "v", undefined, "random u16 key / f32 values");
    check(df, "k", "v", mask, "random masked");
    check(df, "s", "v", mask, "random string key");
});

__kitS.section("filtered frame (dictionary holds absent strings)", () => {
    const df = new DataFrame({ s: ["a", "b", "c", "a", "d"], v: new Float64Array([1, 2, 3, 4, 5]) });
    const f = df.FILTER(new Uint8Array([1, 0, 0, 1, 1]));
    check(f, "s", "v", undefined, "filtered frame (dictionary holds absent strings)");
    const r = f.GROUP_BY_AGG("s", "v", ["sum", "count"]);
    ok(JSON.stringify(r.keys) === '["a","d"]' && r.sum.length === 2 && r.count[0] === 2, "string keys absent from the rows are pruned from keys and every field");
    check(df.SLICE(1, 3), "s", "v", new Uint8Array([0, 1]), "sliced frame with a mask");
});
__kitS.end();

print("test_dataframe_group_agg: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
