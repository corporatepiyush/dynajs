// Parametric black-box contract test for dyna:dataframe, generated from dynajs.d.ts lines 6322-6686. Engine sources not consulted.

import { DataFrame, isDataFrame } from "dyna:dataframe";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function assertEqArr(actual, expected, msg) { n++; const A = ArrayBuffer.isView(actual) ? Array.from(actual) : actual; if (!eqArr(A, expected)) throw new Error("assertion failed (arr): " + msg + " — got |" + JSON.stringify(A) + "| expected |" + JSON.stringify(expected) + "|"); }
function runBespoke(rows) {
    for (const [label, fn] of rows) {
        try { fn(); } catch (e) { throw new Error("row [" + label + "]: " + (e && e.message ? e.message : String(e))); }
    }
}

const f64 = (a) => new Float64Array(a);
const i32 = (a) => new Int32Array(a);
const EPS = 1e-9;
const M13 = new Uint8Array([1, 0, 1, 0, 1]);          // rows 0,2,4 selected
const M01 = new Uint8Array([0, 1, 1, 1, 1]);          // rows 1..4 selected

/* Hand-checkable 5-row frame.
 *  x  = [1,2,3,4,5]   y = [10,20,30,40,50]   gid = [0,1,0,1,0]
 *  s  = [a,b,a,b,c]   v = [1,2,3,1,5]        vd  = [1,2,1,2,3]
 *  pos= [0,1,0,2,3]   bits = [1,3,2,6,4]
 */
const df = new DataFrame({
    x: f64([1, 2, 3, 4, 5]),
    y: f64([10, 20, 30, 40, 50]),
    gid: i32([0, 1, 0, 1, 0]),
    s: ["a", "b", "a", "b", "c"],
    v: f64([1, 2, 3, 1, 5]),
    vd: f64([1, 2, 1, 2, 3]),
    pos: i32([0, 1, 0, 2, 3]),
    bits: i32([1, 3, 2, 6, 4]),
});

const nf = new DataFrame({ xn: f64([1, NaN, 3]) });                    // NA row
const uf = new DataFrame({ u: f64([3, 1, 2]) });                       // unsorted
const unf = new DataFrame({ u: f64([3, NaN, 1]) });                    // unsorted + NaN
const tf = new DataFrame({ t: f64([1, 2, 2]) });                       // ties

/* NaN-tolerant numeric array compare: NaN slots in `exp` must be NaN in
 * `got`; everything else is compared within EPS. */
function cmpArrNa(got, exp, msg) {
    n++;
    const A = Array.from(got);
    if (A.length !== exp.length) throw new Error("assertion failed (arr): " + msg + " — length " + A.length + " ≠ " + exp.length);
    for (let i = 0; i < A.length; i++) {
        const g = A[i], e = exp[i];
        if (typeof e === "number" && Number.isNaN(e)) {
            if (!Number.isNaN(g)) throw new Error("assertion failed (arr): " + msg + " — [" + i + "] got |" + g + "| expected NaN");
        } else if (typeof g !== "number" || !(Math.abs(g - e) <= EPS)) {
            throw new Error("assertion failed (arr): " + msg + " — [" + i + "] got |" + g + "| expected |" + e + "|");
        }
    }
}
/* GroupResult -> key->number map, order-free compare. */
function assertGroupNum(gr, exp, msg) {
    n++;
    const m = new Map();
    for (let i = 0; i < gr.keys.length; i++) m.set(String(gr.keys[i]), gr.values[i]);
    const ek = Object.keys(exp);
    if (m.size !== ek.length) throw new Error("assertion failed: " + msg + " — group count " + m.size + " ≠ " + ek.length);
    for (const k of ek) {
        if (!m.has(k)) throw new Error("assertion failed: " + msg + " — missing group " + k);
        const v = m.get(k);
        if (typeof v !== "number" || !(Math.abs(v - exp[k]) <= EPS)) throw new Error("assertion failed: " + msg + " — group " + k + " got |" + v + "| expected |" + exp[k] + "|");
    }
}
/* GroupArrays -> key->number[] map, order-free keys, NaN-aware values. */
function assertGroupArr(ga, exp, msg) {
    n++;
    const m = new Map();
    for (let i = 0; i < ga.keys.length; i++) m.set(String(ga.keys[i]), Array.from(ga.values[i]));
    const ek = Object.keys(exp);
    if (m.size !== ek.length) throw new Error("assertion failed: " + msg + " — group count " + m.size + " ≠ " + ek.length);
    for (const k of ek) {
        if (!m.has(k)) throw new Error("assertion failed: " + msg + " — missing group " + k);
        try { cmpArrNa(m.get(k), exp[k], msg + " — group " + k); } catch (e) { throw new Error(e.message); }
    }
}

/* ================================================================== *
 *  Table 1: construction — dtype mapping rows.
 *  Doc: "f64 f32 i32 u32 i16 u16 i8 u8 str"; number[] -> "str" coercion.
 * ================================================================== */
const DTYPE_ROWS = [
    ["Float64Array -> f64", () => new Float64Array([1]), "f64"],
    ["Float32Array -> f32", () => new Float32Array([1]), "f32"],
    ["Int32Array -> i32", () => new Int32Array([1]), "i32"],
    ["Uint32Array -> u32", () => new Uint32Array([1]), "u32"],
    ["Int16Array -> i16", () => new Int16Array([1]), "i16"],
    ["Uint16Array -> u16", () => new Uint16Array([1]), "u16"],
    ["Int8Array -> i8", () => new Int8Array([1]), "i8"],
    ["Uint8Array -> u8", () => new Uint8Array([1]), "u8"],
    ["number[] -> str (documented coercion)", () => [1, 2], "str"],
    ["string[] -> str", () => ["a"], "str"],
];
for (const [label, col, dtype] of DTYPE_ROWS) {
    const f = new DataFrame({ c: col() });
    assertEq(f.DTYPES().c, dtype, label);
}

runBespoke([
    ["TypedArray columns are ZERO-COPY aliased (runtime-verified)", () => {
        const a = new Float64Array([1, 2]);
        const f = new DataFrame({ a });
        a[0] = 42;
        assertEq(f.TO_COLUMNS().a[0], 42, "source mutation shows through the frame");
    }],
    ["TO_COLUMNS hands out fresh copies", () => {
        const a = new Float64Array([1, 2]);
        const f = new DataFrame({ a });
        const got = f.TO_COLUMNS().a;
        got[0] = 7;
        assertEq(f.TO_COLUMNS().a[0], 1, "mutating the handed-out copy leaves the frame untouched");
    }],
    ["COPY decouples the frame from the source array", () => {
        const a = new Float64Array([1, 2]);
        const f = new DataFrame({ a });
        const cp = f.COPY();
        a[0] = 99;
        assertEq(cp.TO_COLUMNS().a[0], 1, "COPY is an exact copy");
        assert(isDataFrame(cp), "COPY result is a frame");
    }],
    ["shape properties: ROWS/COLS/COLUMNS in column order", () => {
        assertEq(df.ROWS, 5, "ROWS");
        assertEq(df.COLS, 8, "COLS");
        assertEqArr(df.COLUMNS, ["x", "y", "gid", "s", "v", "vd", "pos", "bits"], "COLUMNS");
    }],
]);

/* Table 2: isDataFrame brand rows — true only for engine-built frames. */
const BRAND_ROWS = [
    ["constructor frame", df, true],
    ["COPY result", df.COPY(), true],
    ["SELECT result", df.SELECT(["x"]), true],
    ["SLICE result", df.SLICE(0, 1), true],
    ["FROM_RECORDS result", df.FROM_RECORDS([{ a: 1 }]), true],
    ["lookalike object with ROWS/COLS/COLUMNS", { ROWS: 5, COLS: 1, COLUMNS: ["x"] }, false],
    ["object chained to a real frame (no brand)", Object.create(df), false],
    ["array", [], false],
    ["string", "df", false],
    ["number", 5, false],
    ["null", null, false],
    ["undefined", undefined, false],
];
for (const [label, v, expected] of BRAND_ROWS) assertEq(isDataFrame(v), expected, label + " — isDataFrame brand");

/* Table 3: INFO/SCHEMA/DTYPES/MEMORY_USAGE shapes. */
runBespoke([
    ["DTYPES covers every column with the right tag", () => {
        const d = df.DTYPES();
        assertEq(Object.keys(d).length, 8, "one entry per column");
        assertEq(d.x, "f64", "x dtype");
        assertEq(d.gid, "i32", "gid dtype");
        assertEq(d.s, "str", "s dtype");
    }],
    ["SCHEMA: one {name,type} per column, in order", () => {
        const sc = df.SCHEMA();
        assertEq(sc.length, 8, "length");
        assertEq(sc[0].name, "x", "first name");
        assertEq(sc[0].type, "f64", "first type");
        assertEqArr(sc.map((c) => c.name), df.COLUMNS, "schema order matches COLUMNS");
    }],
    ["INFO: {rows, cols, dtypes, bytes, total_bytes}", () => {
        const info = df.INFO();
        assertEq(info.rows, 5, "rows");
        assertEq(info.cols, 8, "cols");
        assertEq(info.dtypes.x, "f64", "dtypes entry");
        assert(info.bytes.x > 0, "per-column byte counts positive");
        let sum = 0;
        for (const k of Object.keys(info.bytes)) sum += info.bytes[k];
        assertEq(info.total_bytes, sum, "total_bytes is the sum of the per-column bytes");
    }],
    ["MEMORY_USAGE: {columns, total} consistent with INFO bytes", () => {
        const mu = df.MEMORY_USAGE();
        assertEq(mu.columns.x, df.INFO().bytes.x, "column bytes parity between the two doors");
        let sum = 0;
        for (const k of Object.keys(mu.columns)) sum += mu.columns[k];
        assertEq(mu.total, sum, "total is the sum of the per-column bytes");
    }],
]);

/* ================================================================== *
 *  Table 4: reductions — [label, actual, expected]; every expectation
 *  is hand-computed (arithmetic in the label/comment).
 * ================================================================== */
const REDUCE_ROWS = [
    ["SUM(x) = 1+2+3+4+5", df.SUM("x"), 15],
    ["SUM(y) = 10+20+30+40+50", df.SUM("y"), 150],
    ["SUM(x, M13) = 1+3+5 (masked-out rows do not contribute)", df.SUM("x", M13), 9],
    ["MEAN(x) = 15/5", df.MEAN("x"), 3],
    ["MEAN(x, M13) = (1+3+5)/3", df.MEAN("x", M13), 3],
    ["MIN(x)", df.MIN("x"), 1],
    ["MAX(x)", df.MAX("x"), 5],
    ["MIN(x, M13) = 1", df.MIN("x", M13), 1],
    ["MAX(x, M13) = 5", df.MAX("x", M13), 5],
    ["COUNT(x)", df.COUNT("x"), 5],
    ["COUNT(x, M13)", df.COUNT("x", M13), 3],
    ["COUNT(x, zero mask)", df.COUNT("x", new Uint8Array(5)), 0],
    ["PRODUCT(x) = 1*2*3*4*5", df.PRODUCT("x"), 120],
    ["PRODUCT(x, M13) = 1*3*5", df.PRODUCT("x", M13), 15],
    ["DOT_PRODUCT(x,y) = 10+40+90+160+250", df.DOT_PRODUCT("x", "y"), 550],
    ["DOT_PRODUCT(x,y,M13) = 10+90+250", df.DOT_PRODUCT("x", "y", M13), 350],
    // SUM_CHECKED is defined only on integer columns (engine TypeError says so;
    // DOC GAP noted in FINDINGS.md, d.ts now states the restriction)
    ["SUM_CHECKED(bits) = 1+3+2+6+4 = 16 (exact integer sum)", df.SUM_CHECKED("bits"), 16],
    ["BITWISE_AND(bits) = 1&3&2&6&4 = 0", df.BITWISE_AND("bits"), 0],
    ["BITWISE_OR(bits) = 1|3|2|6|4 = 7", df.BITWISE_OR("bits"), 7],
    ["BITWISE_XOR(bits) = 1^3^2^6^4 = 2", df.BITWISE_XOR("bits"), 2],
    ["BITWISE_AND(bits, mask [1,1,1,0,0]) = 1&3&2 = 0", df.BITWISE_AND("bits", new Uint8Array([1, 1, 1, 0, 0])), 0],
    ["BITWISE_OR(bits, mask [1,1,1,0,0]) = 3", df.BITWISE_OR("bits", new Uint8Array([1, 1, 1, 0, 0])), 3],
    ["BITWISE_XOR(bits, mask [1,1,1,0,0]) = 1^3^2 = 0", df.BITWISE_XOR("bits", new Uint8Array([1, 1, 1, 0, 0])), 0],
    ["VARIANCE(x) sample = 10/4 = 2.5", df.VARIANCE("x"), 2.5],
    ["VARIANCE_POP(x) = 10/5 = 2", df.VARIANCE_POP("x"), 2],
    ["STDDEV(x) = sqrt(2.5)", df.STDDEV("x"), Math.sqrt(2.5)],
    ["STDDEV_POP(x) = sqrt(2)", df.STDDEV_POP("x"), Math.sqrt(2)],
    ["SKEW(x) = 0 (symmetric data)", df.SKEW("x"), 0],
    ["SKEW_SAMP(x) = 0 (symmetric data)", df.SKEW_SAMP("x"), 0],
    ["SEM(x) = sqrt(2.5)/sqrt(5) = sqrt(0.5)", df.SEM("x"), Math.sqrt(0.5)],
    ["MEDIAN(x) = 3", df.MEDIAN("x"), 3],
    ["MEDIAN(x, M13) = median(1,3,5) = 3", df.MEDIAN("x", M13), 3],
    ["QUANTILE(x,0) = 1 (endpoint exact)", df.QUANTILE("x", 0), 1],
    ["QUANTILE(x,1) = 5 (endpoint exact)", df.QUANTILE("x", 1), 5],
    ["PERCENTILE_CONT(x,0.5) = 3", df.PERCENTILE_CONT("x", 0.5), 3],
    ["PERCENTILE_DISC(x,0.5) = 3", df.PERCENTILE_DISC("x", 0.5), 3],
    ["ENTROPY(x) = log2(5) (5 distinct values, p=1/5 each)", df.ENTROPY("x"), Math.log2(5)],
    ["ENTROPY(s) = -(2*(2/5)log2(2/5))-(1/5)log2(1/5)", df.ENTROPY("s"), -(2 * (2 / 5)) * Math.log2(2 / 5) - (1 / 5) * Math.log2(1 / 5)],
    // API.md: MAD is the MEAN absolute deviation from the mean (6/5 here),
    // distinct from MEDIAN_ABSOLUTE_DEVIATION = median(|x - median|) = 1
    ["MAD(x) = mean(|x-3|) = 6/5", df.MAD("x"), 1.2],
    ["GROUP_BITMAP(gid) = 2 distinct values {0,1}", df.GROUP_BITMAP("gid"), 2],
];
for (const [label, actual, expected] of REDUCE_ROWS) {
    assertClose(actual, expected, EPS, label);
}

runBespoke([
    ["SUM_CHECKED refuses a floating-point column (TypeError names the column)", () => {
        assertThrows(() => df.SUM_CHECKED("x"), "float column refuses a checked sum",
            TypeError, /SUM_CHECKED.*'x'/);
    }],
    ["empty-selection MIN/MAX are undefined; parity QUANTILE/MEDIAN; MAD alias", () => {
        const none = new Uint8Array(5);
        assertEq(df.MIN("x", none), undefined, "MIN over an empty selection");
        assertEq(df.MAX("x", none), undefined, "MAX over an empty selection");
        assertEq(df.QUANTILE("x", 0.5), df.MEDIAN("x"), "QUANTILE(0.5) parity with MEDIAN");
        assertEq(df.MAD("x"), df.MEDIAN_ABSOLUTE_DEVIATION("x") === undefined ? undefined : df.MAD("x"), "MAD defined");
        // API.md: the two deviations are DIFFERENT statistics, not aliases
        assertClose(df.MAD("x"), 1.2, EPS, "MAD = mean |x-mean|");
        assertClose(df.MEDIAN_ABSOLUTE_DEVIATION("x"), 1, EPS, "MEDIAN_ABSOLUTE_DEVIATION = median |x-median|");
    }],
    ["QUANTILES from one gather: [0,0.5,1] -> endpoints + median", () => {
        cmpArrNa(df.QUANTILES("x", [0, 0.5, 1]), [1, 3, 5], "QUANTILES");
    }],
    ["DESCRIBE one-pass stats (count/sum/mean/min/max pinned; variance flavor unpinned)", () => {
        const d = df.DESCRIBE("x");
        assertEq(d.count, 5, "count");
        assertEq(d.sum, 15, "sum");
        assertClose(d.mean, 3, EPS, "mean");
        assertEq(d.min, 1, "min");
        assertEq(d.max, 5, "max");
    }],
    ["COUNT_NULLS counts NaN (typed-column NA representation)", () => {
        assertEq(nf.COUNT_NULLS("xn"), 1, "one NaN in xn");
    }],
    ["FILL_NA replaces NaN", () => {
        cmpArrNa(nf.FILL_NA("xn", 0), [1, 0, 3], "FILL_NA");
    }],
    ["IS_NA / NOT_NA / DROP_NA", () => {
        assertEqArr(nf.IS_NA("xn"), [0, 1, 0], "IS_NA");
        assertEqArr(nf.NOT_NA("xn"), [1, 0, 1], "NOT_NA");
        assertEqArr(nf.DROP_NA("xn"), [1, 0, 1], "DROP_NA mask");
    }],
]);

/* ================================================================== *
 *  Table 5: positional doors — HEAD/TAIL/FIRST/LAST/ARG_MIN/ARG_MAX.
 * ================================================================== */
const POSITIONAL_ROWS = [
    ["HEAD default n = 5 (whole frame)", df.HEAD("x"), [1, 2, 3, 4, 5]],
    ["HEAD(x,2)", df.HEAD("x", 2), [1, 2]],
    ["HEAD clamps n to the frame", df.HEAD("x", 99), [1, 2, 3, 4, 5]],
    ["TAIL(x,2)", df.TAIL("x", 2), [4, 5]],
    ["ARG_MIN(x) = 0 (index of 1)", df.ARG_MIN("x"), 0],
    ["ARG_MAX(x) = 4 (index of 5)", df.ARG_MAX("x"), 4],
    ["ARG_MIN(x, M01) = 1 (min of 2,3,4,5 is 2 at index 1)", df.ARG_MIN("x", M01), 1],
];
for (const [label, actual, expected] of POSITIONAL_ROWS) {
    if (Array.isArray(expected)) cmpArrNa(actual, expected, label);
    else assertEq(actual, expected, label);
}
runBespoke([
    ["FIRST/LAST scalar doors; empty selection undefined", () => {
        assertEq(df.FIRST("x"), 1, "FIRST");
        assertEq(df.LAST("x"), 5, "LAST");
        assertEq(df.FIRST("x", new Uint8Array(5)), undefined, "FIRST empty selection");
        assertEq(df.LAST("x", new Uint8Array(5)), undefined, "LAST empty selection");
    }],
]);

/* ================================================================== *
 *  Table 6: mask verbs — [label, mask, expected bytes].
 * ================================================================== */
const MASK_ROWS = [
    ["GT(x,3)", df.GT("x", 3), [0, 0, 0, 1, 1]],
    ["GE(x,3)", df.GE("x", 3), [0, 0, 1, 1, 1]],
    ["LT(x,3)", df.LT("x", 3), [1, 1, 0, 0, 0]],
    ["LE(x,3)", df.LE("x", 3), [1, 1, 1, 0, 0]],
    ["EQ(x,3)", df.EQ("x", 3), [0, 0, 1, 0, 0]],
    ["NE(x,3)", df.NE("x", 3), [1, 1, 0, 1, 1]],
    ["BETWEEN(x,2,4) inclusive both ends", df.BETWEEN("x", 2, 4), [0, 1, 1, 1, 0]],
    ["BETWEEN(x,1,5) full range all set", df.BETWEEN("x", 1, 5), [1, 1, 1, 1, 1]],
    ["ISIN(x,[2,4])", df.ISIN("x", [2, 4]), [0, 1, 0, 1, 0]],
    ["ISIN(s,['a','c'])", df.ISIN("s", ["a", "c"]), [1, 0, 1, 0, 1]],
    ["DROP_DUPLICATES(s): 1 on FIRST occurrences", df.DROP_DUPLICATES("s"), [1, 1, 0, 0, 1]],
    ["DROP_DUPLICATES(x): all distinct all set", df.DROP_DUPLICATES("x"), [1, 1, 1, 1, 1]],
    ["BITMASK([1,0,0,0,1]) = word 0b10001 LSB first", df.BITMASK(new Uint8Array([1, 0, 0, 0, 1])), [17]],
];
for (const [label, mask, expected] of MASK_ROWS) assertEqArr(mask, expected, label);

runBespoke([
    // EQ/NE are numeric-verb doors (API.md: value is a Number); on a string
    // column they refuse rather than compare dictionary codes — string
    // matching goes through ISIN, which does match strings.
    ["EQ/NE on a string column refuse (string columns match via ISIN)", () => {
        assertThrows(() => df.EQ("s", "a"), "string column EQ refuses", TypeError, /dictionary codes/);
        assertThrows(() => df.NE("s", "a"), "string column NE refuses", TypeError, /dictionary codes/);
    }],
    ["ALL/ANY", () => {
        // the mask is a ROWS-byte mask (API.md); the frame has 5 rows
        assertEq(df.ALL(new Uint8Array([1, 1, 0, 1, 1])), false, "ALL with a zero");
        assertEq(df.ALL(new Uint8Array([1, 1, 1, 1, 1])), true, "ALL nonzero");
        assertEq(df.ANY(new Uint8Array([0, 1, 0, 0, 0])), true, "ANY with a set");
        assertEq(df.ANY(new Uint8Array([0, 0, 0, 0, 0])), false, "ANY all zero");
        assertThrows(() => df.ALL(new Uint8Array([1, 1, 0])), "short mask refuses", Error, /5 bytes/);
    }],
    ["FILTER keeps rows where the mask byte is nonzero", () => {
        const f = df.FILTER(M13);
        assertEq(f.ROWS, 3, "kept rows");
        cmpArrNa(f.TO_COLUMNS().x, [1, 3, 5], "kept x values");
    }],
    ["MASK zeroes masked-out numeric rows to fill", () => {
        const f = new DataFrame({ x: f64([1, 2, 3, 4, 5]) }).MASK(M13, -1);
        cmpArrNa(f.TO_COLUMNS().x, [1, -1, 3, -1, 5], "MASK numeric fill");
    }],
    ["MASK fills string columns too", () => {
        const f = new DataFrame({ s: ["a", "b", "a", "b", "c"] }).MASK(M13, "z");
        assertEqArr(f.TO_COLUMNS().s, ["a", "z", "a", "z", "c"], "MASK string fill");
    }],
    ["BOOL_AND/OR/XOR", () => {
        const b3 = new DataFrame({ b: f64([1, 0, 1]) });
        assertEq(b3.BOOL_AND("b"), false, "BOOL_AND [1,0,1]");
        assertEq(b3.BOOL_OR("b"), true, "BOOL_OR [1,0,1]");
        assertEq(b3.BOOL_XOR("b"), false, "BOOL_XOR [1,0,1] (even truthy count)");
        const b5 = new DataFrame({ b: f64([1, 1, 1]) });
        assertEq(b5.BOOL_XOR("b"), true, "BOOL_XOR [1,1,1] (odd truthy count)");
    }],
]);

/* ================================================================== *
 *  Table 7: GROUP_BY_* with hand-computed groups.
 *  g0 rows {0,2,4}, g1 rows {1,3}.
 * ================================================================== */
const GROUP_NUM_ROWS = [
    ["GROUP_BY_SUM(gid,x): g0 1+3+5=9, g1 2+4=6", df.GROUP_BY_SUM("gid", "x"), { "0": 9, "1": 6 }],
    ["GROUP_BY_MEAN(gid,x): g0 9/3=3, g1 6/2=3", df.GROUP_BY_MEAN("gid", "x"), { "0": 3, "1": 3 }],
    ["GROUP_BY_MIN(gid,x): g0 1, g1 2", df.GROUP_BY_MIN("gid", "x"), { "0": 1, "1": 2 }],
    ["GROUP_BY_MAX(gid,x): g0 5, g1 4", df.GROUP_BY_MAX("gid", "x"), { "0": 5, "1": 4 }],
    ["GROUP_BY_COUNT(gid): g0 3 rows, g1 2 rows", df.GROUP_BY_COUNT("gid"), { "0": 3, "1": 2 }],
    ["SUM_MAP parity with GROUP_BY_SUM", df.SUM_MAP("gid", "x"), { "0": 9, "1": 6 }],
    ["MIN_MAP parity with GROUP_BY_MIN", df.MIN_MAP("gid", "x"), { "0": 1, "1": 2 }],
    ["MAX_MAP parity with GROUP_BY_MAX", df.MAX_MAP("gid", "x"), { "0": 5, "1": 4 }],
    ["string key door: GROUP_BY_SUM(s,x): a 1+3=4, b 2+4=6, c 5", df.GROUP_BY_SUM("s", "x"), { a: 4, b: 6, c: 5 }],
    ["GROUP_BIT_AND(gid,bits): g0 1&2&4=0, g1 3&6=2", df.GROUP_BIT_AND("gid", "bits"), { "0": 0, "1": 2 }],
    ["GROUP_BIT_OR(gid,bits): g0 1|2|4=7, g1 3|6=7", df.GROUP_BIT_OR("gid", "bits"), { "0": 7, "1": 7 }],
    ["GROUP_BIT_XOR(gid,bits): g0 1^2^4=7, g1 3^6=5", df.GROUP_BIT_XOR("gid", "bits"), { "0": 7, "1": 5 }],
];
for (const [label, gr, expected] of GROUP_NUM_ROWS) assertGroupNum(gr, expected, label);

const GROUP_ARR_ROWS = [
    ["GROUP_ARRAY(gid,v): g0 rows(0,2,4)=[1,3,5], g1 rows(1,3)=[2,1]", df.GROUP_ARRAY("gid", "v"), { "0": [1, 3, 5], "1": [2, 1] }],
    ["GROUP_ARRAY(gid,vd) keeps duplicates in row order", df.GROUP_ARRAY("gid", "vd"), { "0": [1, 1, 3], "1": [2, 2] }],
    ["GROUP_UNIQ_ARRAY(gid,vd) dedupes: g0 [1,3], g1 [2]", df.GROUP_UNIQ_ARRAY("gid", "vd"), { "0": [1, 3], "1": [2] }],
    ["GROUP_ARRAY_SORTED(gid,vd) sorts within groups", df.GROUP_ARRAY_SORTED("gid", "vd"), { "0": [1, 1, 3], "1": [2, 2] }],
    ["GROUP_ARRAY_MOVING_SUM(gid,vd,2): g0 [1,1+1,1+3]=[1,2,4], g1 [2,4]", df.GROUP_ARRAY_MOVING_SUM("gid", "vd", 2), { "0": [1, 2, 4], "1": [2, 4] }],
    ["GROUP_ARRAY_MOVING_AVG(gid,vd,2): g0 [1,1,2], g1 [2,2]", df.GROUP_ARRAY_MOVING_AVG("gid", "vd", 2), { "0": [1, 1, 2], "1": [2, 2] }],
    ["GROUP_ARRAY_LAST(gid,vd,2): last 2 per group", df.GROUP_ARRAY_LAST("gid", "vd", 2), { "0": [1, 3], "1": [2, 2] }],
];
runBespoke([
    ["GROUP_ARRAY_INSERT_AT(v,pos,6,0): dense slots, later rows overwrite -> [3,2,1,5,0,0]", () => {
        // rows: 1@0, 2@1, 3@0, 1@2, 5@3 -> slot0=3(slot overwritten), slot1=2, slot2=1, slot3=5, rest fill
        cmpArrNa(df.GROUP_ARRAY_INSERT_AT("v", "pos", 6, 0), [3, 2, 1, 5, 0, 0], "insert-at dense array");
    }],
]);
for (const [label, ga, expected] of GROUP_ARR_ROWS) assertGroupArr(ga, expected, label);

runBespoke([
    ["GROUP_ARRAY_SAMPLE(gid,vd,1): k values per group, drawn from that group", () => {
        const ga = df.GROUP_ARRAY_SAMPLE("gid", "vd", 1);
        const m = new Map();
        for (let i = 0; i < ga.keys.length; i++) m.set(String(ga.keys[i]), Array.from(ga.values[i]));
        assertEq(m.size, 2, "two groups");
        assertEq(m.get("0").length, 1, "g0 sample size");
        assertEq(m.get("1").length, 1, "g1 sample size");
        assertEq(m.get("0")[0] === 1 || m.get("0")[0] === 3, true, "g0 sample drawn from {1,3}");
        assertEq(m.get("1")[0] === 2, true, "g1 sample drawn from {2}");
    }],
    ["GROUP_ARRAY_INTERSECT: values present in EVERY group", () => {
        cmpArrNa(df.GROUP_ARRAY_INTERSECT("gid", "v"), [1], "{1,3,5} intersect {2,1} = {1}");
    }],
    ["GROUP_CONCAT joins with the separator", () => {
        assertEq(df.GROUP_CONCAT("s", "|"), "a|b|a|b|c", "GROUP_CONCAT");
    }],
    ["JSON_AGG / JSON_OBJECT_AGG / strict variants (values parsed, compared numerically)", () => {
        const agg = JSON.parse(df.JSON_AGG("gid", "x"));
        assertGroupArr({ keys: Object.keys(agg), values: Object.keys(agg).map((k) => agg[k]) }, { "0": [1, 3, 5], "1": [2, 4] }, "JSON_AGG groups");
        const obj = JSON.parse(df.JSON_OBJECT_AGG("gid", "x"));
        assertEq(obj["0"], 5, "OBJECT_AGG: last row per key wins"); // standard object-agg overwrite
        assertEq(obj["1"], 4, "OBJECT_AGG g1");
        const strictAgg = JSON.parse(df.JSON_AGG_STRICT("gid", "x"));
        assertEq(JSON.stringify(strictAgg), JSON.stringify(agg), "JSON_AGG_STRICT parity on clean data");
        const strictObj = JSON.parse(df.JSON_OBJECT_AGG_STRICT("gid", "x"));
        assertEq(JSON.stringify(strictObj), JSON.stringify(obj), "JSON_OBJECT_AGG_STRICT parity on clean data");
    }],
]);

/* ================================================================== *
 *  Table 8: elementwise verbs — number and column-name operands,
 *  RSUB/RDIV number-only; out:{} overload as the second door.
 * ================================================================== */
const EW_ROWS = [
    ["ADD(x,10)", df.ADD("x", 10), [11, 12, 13, 14, 15]],
    ["ADD(x,'y') col-vs-col", df.ADD("x", "y"), [11, 22, 33, 44, 55]],
    ["SUB(x,1)", df.SUB("x", 1), [0, 1, 2, 3, 4]],
    ["SUB('y',x) col-vs-col", df.SUB("y", "x"), [9, 18, 27, 36, 45]],
    ["MUL(x,'y') col-vs-col", df.MUL("x", "y"), [10, 40, 90, 160, 250]],
    ["DIV('y',x) col-vs-col = 10 everywhere", df.DIV("y", "x"), [10, 10, 10, 10, 10]],
    ["DIV(x,2)", df.DIV("x", 2), [0.5, 1, 1.5, 2, 2.5]],
    ["POW(x,2)", df.POW("x", 2), [1, 4, 9, 16, 25]],
    ["RSUB(x,10) = k - col", df.RSUB("x", 10), [9, 8, 7, 6, 5]],
    ["RDIV(x,60) = k / col", df.RDIV("x", 60), [60, 30, 20, 15, 12]],
    ["ABS over [-1,2,-3]", new DataFrame({ c: f64([-1, 2, -3]) }).ABS("c"), [1, 2, 3]],
    ["ROUND([1.4,2.6])", new DataFrame({ c: f64([1.4, 2.6]) }).ROUND("c"), [1, 3]],
    ["FLOOR([-1.5,2.7])", new DataFrame({ c: f64([-1.5, 2.7]) }).FLOOR("c"), [-2, 2]],
    ["CEIL([-1.5,2.2])", new DataFrame({ c: f64([-1.5, 2.2]) }).CEIL("c"), [-1, 3]],
    ["SIGN([-3,0,2])", new DataFrame({ c: f64([-3, 0, 2]) }).SIGN("c"), [-1, 0, 1]],
    ["SQRT([4,9])", new DataFrame({ c: f64([4, 9]) }).SQRT("c"), [2, 3]],
    ["LOG([1]) = 0", new DataFrame({ c: f64([1]) }).LOG("c"), [0]],
    ["LOG([e]) = 1", new DataFrame({ c: f64([2.718281828459045]) }).LOG("c"), [1]],
    ["EXP([0,1]) = [1,e]", new DataFrame({ c: f64([0, 1]) }).EXP("c"), [1, 2.718281828459045]],
    ["CLIP(x,2,4)", df.CLIP("x", 2, 4), [2, 2, 3, 4, 4]],
    ["WHERE(M13,'x',0)", df.WHERE(M13, "x", 0), [1, 0, 3, 0, 5]],
    ["WHERE(M13,100,'y')", df.WHERE(M13, 100, "y"), [100, 20, 100, 40, 100]],
    ["WHERE(M13,'x','y') col-vs-col", df.WHERE(M13, "x", "y"), [1, 20, 3, 40, 5]],
];
for (const [label, actual, expected] of EW_ROWS) cmpArrNa(actual, expected, label);

runBespoke([
    // API.md "The {out} in-place form": out names an EXISTING Float64Array
    // column (never created); the frame is returned and the buffer is mutated.
    ["out:{} writes INTO an existing column and returns the frame", () => {
        const f = new DataFrame({ x: f64([1, 2, 3, 4, 5]), x10: new Float64Array(5), xsq: new Float64Array(5) });
        const plain = f.ADD("x", 10);
        assert(f.ADD("x", 10, { out: "x10" }) === f, "out overload returns this");
        cmpArrNa(f.TO_COLUMNS().x10, Array.from(plain), "x10 holds the same result as the array door");
        assertThrows(() => f.ADD("x", 10, { out: "nope" }), "out column is never created", RangeError, /no such column/);
        const sq = f.SQRT("x");
        f.SQRT("x", { out: "xsq" });
        cmpArrNa(f.TO_COLUMNS().xsq, Array.from(sq), "SQRT out parity");
    }],
]);

/* ================================================================== *
 *  Table 9: ordering — sorting, ranking, frequency. NaN sorts last.
 * ================================================================== */
const ORDER_ROWS = [
    ["SORT(u) = [1,2,3]", uf.SORT("u"), [1, 2, 3]],
    ["SORT(un) puts NaN last", unf.SORT("u"), [1, 3, NaN]],
    ["ARG_SORT(u) = [1,2,0]", uf.ARG_SORT("u"), [1, 2, 0]],
    ["ARG_SORT(un) NaN last", unf.ARG_SORT("u"), [2, 0, 1]],
    ["RANK(t) ties share the mean of positions: [1,2,2] -> [1,2.5,2.5]", tf.RANK("t"), [1, 2.5, 2.5]],
    ["DENSE_RANK(t) counts distinct: [1,2,2]", tf.DENSE_RANK("t"), [1, 2, 2]],
    ["RANK(u) no ties: [3,1,2]", uf.RANK("u"), [3, 1, 2]],
    ["PERCENT_RANK(u) = (rank-1)/(n-1) = [1,0,0.5]", uf.PERCENT_RANK("u"), [1, 0, 0.5]],
    ["NTILE(x,2): first 5%2=1 buckets take an extra row -> [1,1,1,2,2]", df.NTILE("x", 2), [1, 1, 1, 2, 2]],
    ["NTILE(x,5): one row per bucket", df.NTILE("x", 5), [1, 2, 3, 4, 5]],
    ["NTILE(x,7): more buckets than rows", df.NTILE("x", 7), [1, 2, 3, 4, 5]],
    ["N_LARGEST(x,2) = [5,4]", df.N_LARGEST("x", 2), [5, 4]],
    ["N_SMALLEST(x,2) = [1,2]", df.N_SMALLEST("x", 2), [1, 2]],
    ["UNIQUE(x) first-seen order (already ascending)", df.UNIQUE("x"), [1, 2, 3, 4, 5]],
    ["CUM_SUM(x) = [1,3,6,10,15]", df.CUM_SUM("x"), [1, 3, 6, 10, 15]],
    ["CUM_PROD(x) = [1,2,6,24,120]", df.CUM_PROD("x"), [1, 2, 6, 24, 120]],
    ["CUM_MAX(x) = running max", df.CUM_MAX("x"), [1, 2, 3, 4, 5]],
    ["CUM_MIN(x) = running min", df.CUM_MIN("x"), [1, 1, 1, 1, 1]],
    ["CUM_MAX(u) = [3,3,3]", uf.CUM_MAX("u"), [3, 3, 3]],
    ["CUM_MIN(u) = [3,1,1]", uf.CUM_MIN("u"), [3, 1, 1]],
];
for (const [label, actual, expected] of ORDER_ROWS) cmpArrNa(actual, expected, label);

runBespoke([
    ["UNIQUE(s) strings in first-seen order; N_UNIQUE", () => {
        assertEqArr(df.UNIQUE("s"), ["a", "b", "c"], "UNIQUE first-seen");
        assertEq(df.N_UNIQUE("s"), 3, "N_UNIQUE");
    }],
    ["UNIQ_UP_TO: exact count, or n+1 meaning 'more than n'", () => {
        // x = [1,2,3,4,5] has FIVE distinct values (s is the 3-distinct column)
        assertEq(df.UNIQ_UP_TO("x", 1), 2, "5 distinct > 1 -> 2");
        assertEq(df.UNIQ_UP_TO("x", 4), 5, "5 distinct > 4 -> 5");
        assertEq(df.UNIQ_UP_TO("x", 5), 5, "5 distinct not > 5 -> exact 5");
        // numeric-only door: a string column refuses (N_UNIQUE/UNIQUE cover strings)
        assertThrows(() => df.UNIQ_UP_TO("s", 3), "string column refuses", Error, /string column/);
    }],
    ["VALUE_COUNTS frequencies", () => {
        assertGroupNum(df.VALUE_COUNTS("s"), { a: 2, b: 2, c: 1 }, "VALUE_COUNTS(s)");
    }],
    ["TOP_K returns the k most FREQUENT values (API.md: not the magnitude one)", () => {
        const gr = df.TOP_K("x", 2);
        // every value in x occurs once; ties resolve to row-order first ([1,2])
        assertEqArr(Array.from(gr.keys), [1, 2], "top keys");
        assertEqArr(Array.from(gr.values), [1, 1], "each seen once");
    }],
    ["MODE tie goes to the FIRST in row order (documented)", () => {
        assertEq(df.MODE("s"), "a", "tie a/b resolved to row-order first");
        assertEq(df.MODE("x"), 1, "all ties -> first row");
    }],
]);

/* ================================================================== *
 *  Table 10: scans — SHIFT/DIFF/ROLLING/EMA/PCT_CHANGE/ZSCORE.
 *  DOC-TENSION: incomplete leading rolling windows assumed NaN (the
 *  doc only pins "exactly ROWS entries"); only the complete windows
 *  are pinned numerically plus a NaN head check.
 * ================================================================== */
const SCAN_ROWS = [
    ["SHIFT(x,1): vacated head is NaN", df.SHIFT("x", 1), [NaN, 1, 2, 3, 4]],
    ["SHIFT(x) defaults to periods 1", df.SHIFT("x"), [NaN, 1, 2, 3, 4]],
    ["SHIFT(x,-1): vacated tail is NaN", df.SHIFT("x", -1), [2, 3, 4, 5, NaN]],
    ["DIFF(x) = consecutive deltas", df.DIFF("x"), [NaN, 1, 1, 1, 1]],
    ["DIFF(x,2)", df.DIFF("x", 2), [NaN, NaN, 2, 2, 2]],
    ["ROLLING_SUM(x,2) complete windows: [3,5,7,9]", df.ROLLING_SUM("x", 2), [NaN, 3, 5, 7, 9]],
    ["ROLLING_MEAN(x,2) complete windows", df.ROLLING_MEAN("x", 2), [NaN, 1.5, 2.5, 3.5, 4.5]],
    ["ROLLING_MIN(u,2)", uf.ROLLING_MIN("u", 2), [NaN, 1, 1]],
    ["ROLLING_MAX(u,2)", uf.ROLLING_MAX("u", 2), [NaN, 3, 2]],
    ["EMA(x,1): alpha 1 reproduces the column", df.EMA("x", 1), [1, 2, 3, 4, 5]],
    ["PCT_CHANGE(x) = [NaN,1,0.5,1/3,0.25]", df.PCT_CHANGE("x"), [NaN, 1, 0.5, 1 / 3, 0.25]],
];
for (const [label, actual, expected] of SCAN_ROWS) cmpArrNa(actual, expected, label);

runBespoke([
    ["ZSCORE is flavor-free symmetric: centered, symmetric pairs", () => {
        const z = df.ZSCORE("x");
        assertEq(z.length, 5, "exactly ROWS entries");
        assertClose(z[2], 0, EPS, "centered at the mean");
        assertClose(z[0], -z[4], EPS, "symmetric ends");
        assertClose(z[1], -z[3], EPS, "symmetric inner pair is (z1,z3)");
    }],
    ["DELTA_SUM: sum of positive consecutive differences", () => {
        assertClose(df.DELTA_SUM("x"), 4, EPS, "1+1+1+1");
        const d = new DataFrame({ c: f64([5, 4, 3, 2, 1]) });
        assertClose(d.DELTA_SUM("c"), 0, EPS, "no positive diffs");
        const m = new DataFrame({ c: f64([1, -2, 3]) });
        assertClose(m.DELTA_SUM("c"), 5, EPS, "diffs -3,+5 -> +5");
    }],
    ["DELTA_SUM_TIMESTAMP orders by time, not row", () => {
        const f = new DataFrame({ dv: f64([10, 20, 30]), dt: f64([2, 0, 1]) });
        // by time: 20,30,10 -> diffs +10,-20 -> positive sum 10
        assertClose(f.DELTA_SUM_TIMESTAMP("dv", "dt"), 10, EPS, "timestamp-ordered delta sum");
    }],
]);

/* ================================================================== *
 *  Table 11: pairwise stats on y = 2x (cf frame).
 * ================================================================== */
const cf = new DataFrame({ xx: f64([1, 2, 3, 4, 5]), yy: f64([2, 4, 6, 8, 10]) });
const PAIR_ROWS = [
    ["CORR(y=2x) = 1", cf.CORR("xx", "yy"), 1],
    ["RANK_CORR (monotone) = 1", cf.RANK_CORR("xx", "yy"), 1],
    ["REGR_SLOPE(y,x) = 2", cf.REGR_SLOPE("yy", "xx"), 2],
    ["REGR_INTERCEPT(y,x) = 0", cf.REGR_INTERCEPT("yy", "xx"), 0],
    ["REGR_R2 = 1", cf.REGR_R2("yy", "xx"), 1],
    ["REGR_COUNT = 5", cf.REGR_COUNT("xx", "yy"), 5],
    ["REGR_AVG_X = 3", cf.REGR_AVG_X("yy", "xx"), 3],
    ["REGR_AVG_Y = mean(yy) = 6", cf.REGR_AVG_Y("yy", "xx"), 6],
    ["REGR_SXX = sum dx^2 = 10", cf.REGR_SXX("yy", "xx"), 10],
    ["REGR_SYY = sum dy^2 = 40 (dy = yy-6)", cf.REGR_SYY("yy", "xx"), 40],
    ["REGR_SXY = sum dx*dy = 20 (dx = xx-3, dy = yy-6)", cf.REGR_SXY("yy", "xx"), 20],
    ["COV_SAMP = 20/4 = 5", cf.COV_SAMP("xx", "yy"), 5],
    ["COV_POP = 20/5 = 4", cf.COV_POP("xx", "yy"), 4],
];
for (const [label, actual, expected] of PAIR_ROWS) assertClose(actual, expected, EPS, label);

runBespoke([
    ["CORR_MATRIX / COV_MATRIX shapes and values", () => {
        const cm = cf.CORR_MATRIX(["xx", "yy"]);
        assertEqArr(cm.columns, ["xx", "yy"], "corr columns");
        assertEq(cm.n, 2, "corr n");
        cmpArrNa(cm.matrix, [1, 1, 1, 1], "corr matrix");
        const cv = cf.COV_MATRIX(["xx", "yy"]);
        cmpArrNa(cv.matrix, [2.5, 5, 5, 10], "cov matrix (sample flavor assumed)"); // DOC-TENSION: matches COV_SAMP
    }],
    ["RATE / IRATE / BOUNDING_RATIO", () => {
        const rtv = new DataFrame({ rv: f64([10, 20, 40]), rt: f64([0, 1, 3]) });
        assertClose(rtv.RATE("rv", "rt"), 10, EPS, "(40-10)/(3-0)");
        assertClose(rtv.IRATE("rv", "rt"), 10, EPS, "(40-20)/(3-1) most recent interval");
        const bnd = new DataFrame({ bx: f64([0, 2, 5]), by: f64([0, 4, 5]) });
        assertClose(bnd.BOUNDING_RATIO("bx", "by"), 1, EPS, "(5-0)/(5-0) leftmost-rightmost slope");
    }],
    ["RANGE_AGG half-open union; RANGE_INTERSECT_AGG common interval", () => {
        const ra = new DataFrame({ lo: f64([0, 2, 8]), hi: f64([3, 5, 10]) });
        const u = ra.RANGE_AGG("lo", "hi");
        cmpArrNa(u.starts, [0, 8], "union starts");
        cmpArrNa(u.ends, [5, 10], "union ends");
        const ri = new DataFrame({ lo: f64([0, 2, 3]), hi: f64([4, 5, 9]) });
        const inter = ri.RANGE_INTERSECT_AGG("lo", "hi");
        assertClose(inter.start, 3, EPS, "common start = max lo");
        assertClose(inter.end, 4, EPS, "common end = min hi");
        assertEq(ra.RANGE_INTERSECT_AGG("lo", "hi"), undefined, "disjoint ranges have no common interval");
    }],
]);

/* ================================================================== *
 *  Table 12: SLICE / SELECT / DROP_COLUMNS / RENAME (clamp +
 *  Array.prototype.slice negative indexing, documented).
 * ================================================================== */
const SLICE_ROWS = [
    ["SLICE(1,3) rows 1..2", (f) => f.SLICE(1, 3), 2, [2, 3]],
    ["SLICE(-2) last two rows", (f) => f.SLICE(-2), 2, [4, 5]],
    ["SLICE(0,-1) all but last", (f) => f.SLICE(0, -1), 4, [1, 2, 3, 4]],
    ["SLICE(2) through the end", (f) => f.SLICE(2), 3, [3, 4, 5]],
    ["SLICE(3,100) clamps end", (f) => f.SLICE(3, 100), 2, [4, 5]],
    ["SLICE(2,2) empty", (f) => f.SLICE(2, 2), 0, []],
    ["SLICE(-100) clamps start to 0", (f) => f.SLICE(-100), 5, [1, 2, 3, 4, 5]],
];
for (const [label, op, expRows, expX] of SLICE_ROWS) {
    const f = op(df);
    assertEq(f.ROWS, expRows, label + " — row count");
    cmpArrNa(f.TO_COLUMNS().x, expX, label + " — x values");
}

runBespoke([
    ["SELECT keeps the order given", () => {
        const f = df.SELECT(["y", "x"]);
        assertEqArr(f.COLUMNS, ["y", "x"], "order given");
        assertEq(f.ROWS, 5, "rows preserved");
    }],
    ["DROP_COLUMNS is the complement in column order", () => {
        const f = df.DROP_COLUMNS(["x", "s"]);
        assertEqArr(f.COLUMNS, ["y", "gid", "v", "vd", "pos", "bits"], "complement");
    }],
    ["RENAME per {old: new}", () => {
        const f = df.RENAME({ x: "xx" });
        assertEqArr(f.COLUMNS, ["xx", "y", "gid", "s", "v", "vd", "pos", "bits"], "renamed columns");
        cmpArrNa(f.TO_COLUMNS().xx, [1, 2, 3, 4, 5], "renamed column data");
    }],
    ["COPY parity", () => {
        const c = df.COPY();
        assertEqArr(c.COLUMNS, df.COLUMNS, "same columns");
        cmpArrNa(c.TO_COLUMNS().x, [1, 2, 3, 4, 5], "same data");
    }],
    ["SAMPLE(n, seed) is deterministic and row-counted", () => {
        const a = df.SAMPLE(3, 42);
        const b = df.SAMPLE(3, 42);
        assertEq(a.ROWS, 3, "n rows without replacement");
        assertEq(JSON.stringify(a.TO_COLUMNS().x), JSON.stringify(b.TO_COLUMNS().x), "seeded shuffle deterministic");
        const sorted = Array.from(a.TO_COLUMNS().x).sort((p, q) => p - q);
        for (let i = 1; i < sorted.length; i++) assert(sorted[i] !== sorted[i - 1], "without replacement");
        assertEq(df.SAMPLE(0).ROWS, 0, "SAMPLE(0)");
    }],
]);

/* ================================================================== *
 *  Table 13: exports — TO_COLUMNS/TO_RECORDS/TO_JSON/TO_CSV/
 *  FROM_RECORDS.
 * ================================================================== */
runBespoke([
    ["TO_COLUMNS returns typed copies with the right values", () => {
        const c = df.TO_COLUMNS();
        cmpArrNa(c.x, [1, 2, 3, 4, 5], "x copy");
        assertEqArr(c.gid, [0, 1, 0, 1, 0], "gid copy");
        assertEqArr(c.s, ["a", "b", "a", "b", "c"], "string column as array");
    }],
    ["TO_RECORDS: one object per row", () => {
        const recs = df.TO_RECORDS();
        assertEq(recs.length, 5, "one per row");
        assertEq(recs[0].x, 1, "row0 x");
        assertEq(recs[0].y, 10, "row0 y");
        assertEq(recs[0].gid, 0, "row0 gid");
        assertEq(recs[0].s, "a", "row0 s");
        assertEq(recs[4].y, 50, "row4 y");
        assertEq(recs[4].s, "c", "row4 s");
    }],
    ["TO_JSON is the TO_RECORDS array serialized; NaN/Infinity become null", () => {
        assertDeepEq2(JSON.parse(df.TO_JSON()), df.TO_RECORDS(), "JSON parity with TO_RECORDS");
        const recs = JSON.parse(nf.TO_JSON());
        assertEq(recs[1].xn, null, "NaN serialized as null");
        const inf = new DataFrame({ d: f64([Infinity]) });
        assertEq(JSON.parse(inf.TO_JSON())[0].d, null, "Infinity serialized as null");
    }],
    ["TO_CSV: header + rows, RFC 4180 quoting", () => {
        const small = new DataFrame({ a: f64([1, 2]), b: ["x", "y"] });
        assertEq(small.TO_CSV(), "a,b\n1,x\n2,y\n", "exact CSV for a clean 2x2 frame");
        const quoted = new DataFrame({ t: ["a,b", "c"], n: f64([1, 2]) });
        assertEq(quoted.TO_CSV(), 't,n\n"a,b",1\nc,2\n', "comma-bearing cell is quoted (header included)");
    }],
    ["TO_CSV escapeFormulas prefixes ' to =/+/-/@ cells", () => {
        const ef = new DataFrame({ t: ["=1+1", "-abc", "ok"] });
        assertEq(ef.TO_CSV(), "t\n=1+1\n-abc\nok\n", "without the flag, untouched");
        assertEq(ef.TO_CSV({ escapeFormulas: true }), "t\n'=1+1\n'-abc\nok\n", "with the flag, formula cells escaped");
    }],
    ["FROM_RECORDS builds an equivalent frame; round trip parity", () => {
        const built = df.FROM_RECORDS([{ a: 1, b: "x" }, { a: 2, b: "y" }]);
        assertEq(built.ROWS, 2, "rows from records");
        cmpArrNa(built.TO_COLUMNS().a, [1, 2], "numeric column");
        assertEqArr(built.TO_COLUMNS().b, ["x", "y"], "string column");
        const back = df.FROM_RECORDS(new DataFrame({ a: f64([1, 2]), b: ["x", "y"] }).TO_RECORDS());
        assertEq(back.ROWS, 2, "round trip rows");
        cmpArrNa(back.TO_COLUMNS().a, [1, 2], "round trip values");
    }],
]);
function assertDeepEq2(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }

/* ================================================================== *
 *  Table 14: reshape — JOIN/ASOF_JOIN/CONCAT/RESAMPLE/PIVOT/MELT.
 *  // assumption: non-matching join fills are NaN (typed columns).
 * ================================================================== */
runBespoke([
    ["JOIN inner: only matching keys", () => {
        const l = new DataFrame({ k: i32([0, 1, 2]), v: f64([10, 20, 30]) });
        const r = new DataFrame({ k: i32([1, 2, 3]), w: f64([4, 5, 6]) });
        const f = l.JOIN(r, "k", "k", "inner");
        assertEq(f.ROWS, 2, "inner row count");
        const recs = f.TO_RECORDS();
        cmpArrNa(recs.map((x) => x.k), [1, 2], "inner keys");
        cmpArrNa(recs.map((x) => x.v), [20, 30], "inner left values");
        cmpArrNa(recs.map((x) => x.w), [4, 5], "inner right values");
    }],
    ["JOIN left keeps every left row (missing fill NaN)", () => {
        const l = new DataFrame({ k: i32([0, 1, 2]), v: f64([10, 20, 30]) });
        const r = new DataFrame({ k: i32([1, 2, 3]), w: f64([4, 5, 6]) });
        const f = l.JOIN(r, "k", "k", "left");
        assertEq(f.ROWS, 3, "left row count");
        assertEq(Number.isNaN(f.TO_RECORDS()[0].w), true, "unmatched w is NaN");
    }],
    ["JOIN right keeps every right row", () => {
        const l = new DataFrame({ k: i32([0, 1, 2]), v: f64([10, 20, 30]) });
        const r = new DataFrame({ k: i32([1, 2, 3]), w: f64([4, 5, 6]) });
        const f = l.JOIN(r, "k", "k", "right");
        assertEq(f.ROWS, 3, "right row count");
        assertEq(Number.isNaN(f.TO_RECORDS()[2].v), true, "unmatched v is NaN");
    }],
    ["JOIN outer = 4 rows", () => {
        const l = new DataFrame({ k: i32([0, 1, 2]), v: f64([10, 20, 30]) });
        const r = new DataFrame({ k: i32([1, 2, 3]), w: f64([4, 5, 6]) });
        assertEq(l.JOIN(r, "k", "k", "outer").ROWS, 4, "outer row count");
    }],
    ["ASOF_JOIN: last right time <= left time", () => {
        // API.md: both time columns INTEGER and both frames sorted ascending
        const l = new DataFrame({ t: i32([1, 5]), av: f64([1, 2]) });
        const r = new DataFrame({ t: i32([2, 3, 7]), bv: f64([20, 30, 70]) });
        const f = l.ASOF_JOIN(r, "t", "t");
        assertEq(f.ROWS, 2, "asof rows");
        assertEq(Number.isNaN(f.TO_RECORDS()[0].bv), true, "no right row <= t=1");
        assertEq(f.TO_RECORDS()[1].bv, 30, "right row at t=3 matches left t=5");
    }],
    ["CONCAT stacks rows (column sets must match exactly)", () => {
        const a = new DataFrame({ x: f64([1, 2]) });
        const b = new DataFrame({ x: f64([3, 4, 5]) });
        const f = a.CONCAT(b);
        assertEq(f.ROWS, 5, "stacked rows");
        cmpArrNa(f.TO_COLUMNS().x, [1, 2, 3, 4, 5], "stacked x");
        assertThrows(() => a.CONCAT(df), "column count mismatch refuses", Error, /column count mismatch/);
    }],
    ["RESAMPLE buckets a sorted time column (sum/mean/min/max/count)", () => {
        // one-column contract: RESAMPLE aggregates the TIME column itself and
        // emits {bucket, value}; half-open [t0+k*i, t0+(k+1)*i) buckets
        const f = new DataFrame({ tv: f64([0, 1, 2, 3, 4]) });
        const vals = (r) => Array.from(r.TO_COLUMNS().value);
        assertEqArr(Array.from(f.RESAMPLE("tv", 2, "sum").TO_COLUMNS().bucket), [0, 2, 4], "bucket starts");
        cmpArrNa(vals(f.RESAMPLE("tv", 2, "sum")), [1, 5, 4], "sum buckets (0+1, 2+3, 4)");
        cmpArrNa(vals(f.RESAMPLE("tv", 2, "mean")), [0.5, 2.5, 4], "mean buckets");
        cmpArrNa(vals(f.RESAMPLE("tv", 2, "min")), [0, 2, 4], "min buckets");
        cmpArrNa(vals(f.RESAMPLE("tv", 2, "max")), [1, 3, 4], "max buckets");
        cmpArrNa(vals(f.RESAMPLE("tv", 2, "count")), [2, 2, 1], "count buckets");
    }],
    ["PIVOT(index, columns, values, sum): one row per index, one col per key", () => {
        const p = new DataFrame({ gid: i32([0, 1, 0, 1, 0]), s: ["a", "b", "a", "b", "c"], x: f64([1, 2, 3, 4, 5]) });
        const f = p.PIVOT("gid", "s", "x", "sum");
        assertEq(f.ROWS, 2, "one row per distinct index");
        assertEq(f.COLS, 4, "index col + a,b,c");
        const recs = f.TO_RECORDS();
        const r0 = recs.find((x) => x.gid === 0);
        const r1 = recs.find((x) => x.gid === 1);
        assertClose(r0.a, 4, EPS, "g0 a: 1+3");
        assertEq(Number.isNaN(r0.b), true, "g0 has no b");
        assertClose(r0.c, 5, EPS, "g0 c");
        assertClose(r1.b, 6, EPS, "g1 b: 2+4");
    }],
    ["MELT: each (row, valueVar) pair becomes one output row", () => {
        const f = df.MELT(["gid"], ["x", "y"]);
        assertEq(f.ROWS, 10, "5 rows x 2 value vars (documented shape)");
        assertEq(f.COLS, 3, "id cols + variable + value"); // assumption on column set
    }],
]);

/* ================================================================== *
 *  Table 15: documented refusals — numeric verbs refuse str columns.
 * ================================================================== */
runBespoke([
    ["number[] columns coerce to str dtype and numeric verbs refuse them", () => {
        const f = new DataFrame({ n: [1.5, 2.5] });
        assertEq(f.DTYPES().n, "str", "number[] -> str (documented)");
        assertThrows(() => f.SUM("n"), "numeric verbs refuse coerced number[] columns");
    }],
    ["native string columns are refused by numeric verbs too", () => {
        assertThrows(() => df.SUM("s"), "SUM over a str column");
    }],
]);

/* ================================================================== *
 *  Non-finite and corruption hardening (audit regression rows). Each
 *  row cites the contract it holds: dynajs.d.ts DataFrame method
 *  signatures and the API.md wording quoted in the labels.
 * ================================================================== */
runBespoke([
    ["ISIN: a values entry that throws mid-scan propagates and leaves the frame usable", () => {
        // d.ts: ISIN(col, values): Uint8Array — a failing element read must
        // surface as an exception, never corrupt the cleanup of the scan.
        const evil = ["a", "b", "c", "d", "e", "f", "g", "h"];
        Object.defineProperty(evil, 5, { get() { throw new RangeError("boom"); } });
        assertThrows(() => df.ISIN("s", evil), "getter throw mid-scan propagates as a JS exception");
        assertEqArr(df.ISIN("s", ["a", "c"]), [1, 0, 1, 0, 1], "frame still answers ISIN after the failed scan");
    }],
    ["HISTOGRAM: non-finite values are excluded, never UB-cast into bin indices", () => {
        // d.ts: HISTOGRAM(col, bins): {edges, counts} — bins are equal-width
        // over the finite range; ±Inf/NaN rows contribute to no bin.
        const fh = new DataFrame({ x: f64([1, 2, 3, Infinity, -Infinity, NaN]) });
        const h = fh.HISTOGRAM("x", 4);
        const sum = Array.from(h.counts).reduce((a, b) => a + b, 0);
        assertEq(sum, 3, "only the 3 finite values are counted");
        cmpArrNa(Array.from(h.counts), [1, 0, 1, 1], "1,2,3 land in bins 0,2,3 of [1..3]");
        assert(Array.from(h.edges).every((e) => Number.isFinite(e)), "edges stay finite");
        const hn = fh.HISTOGRAM_NORMALIZED("x", 4);
        assertClose(Array.from(hn.counts).reduce((a, b) => a + b, 0), 1, EPS, "normalized counts sum to the kept fraction 3/3");
    }],
    ["HISTOGRAM: an all-non-finite column yields all-zero counts, not ±Inf bins", () => {
        const fi = new DataFrame({ x: f64([Infinity, -Infinity, Infinity, NaN]) });
        const h = fi.HISTOGRAM("x", 4);
        assertEq(Array.from(h.counts).reduce((a, b) => a + b, 0), 0, "nothing counted");
        assert(Array.from(h.edges).every((e) => Number.isFinite(e)), "edges finite (empty-range fallback)");
    }],
    ["APPROX_TOP_SUM/TOP_K_WEIGHTED/ANY_HEAVY refuse non-finite weights", () => {
        // API.md: "Returns ranks by SUMMED WEIGHT" — a NaN/Inf weight has no
        // rank and breaks the ordering; the module refuses it up front.
        const fw = new DataFrame({ k: i32([1, 2, 3]), w: f64([1, NaN, 2]) });
        assertThrows(() => fw.APPROX_TOP_SUM("k", "w", 2), "APPROX_TOP_SUM NaN weight", RangeError, /non-finite/);
        assertThrows(() => fw.TOP_K_WEIGHTED("k", "w", 2), "TOP_K_WEIGHTED NaN weight", RangeError, /non-finite/);
        assertThrows(() => fw.ANY_HEAVY("k", "w"), "ANY_HEAVY NaN weight", RangeError, /non-finite/);
        assertThrows(() => fw.QUANTILE_EXACT_WEIGHTED("k", "w", 0.5), "QUANTILE_EXACT_WEIGHTED NaN weight", RangeError, /non-finite/);
        const ok = new DataFrame({ k: i32([1, 1, 2]), w: f64([1, 2, 3]) });
        assertGroupNum(ok.APPROX_TOP_SUM("k", "w", 2), { 1: 3, 2: 3 }, "finite weights still rank by summed weight");
    }],
    ["ROLLING_MIN/ROLLING_MAX: an all-NaN window is NaN, not ±Infinity", () => {
        // d.ts: ROLLING_MIN(col, w): Float64Array — windows holding no finite
        // value have no min/max (pandas/SQL parity), only the warmup is NaN.
        const fr = new DataFrame({ x: f64([NaN, NaN, NaN, 5]) });
        cmpArrNa(Array.from(fr.ROLLING_MIN("x", 3)), [NaN, NaN, NaN, 5], "MIN: all-NaN window NaN, first finite window 5");
        cmpArrNa(Array.from(fr.ROLLING_MAX("x", 3)), [NaN, NaN, NaN, 5], "MAX: all-NaN window NaN, first finite window 5");
        const fm = new DataFrame({ x: f64([NaN, 2, NaN, 4]) });
        cmpArrNa(Array.from(fm.ROLLING_MIN("x", 2)), [NaN, 2, 2, 4], "MIN skips NaN inside a partly-finite window");
        cmpArrNa(Array.from(fm.ROLLING_MAX("x", 2)), [NaN, 2, 2, 4], "MAX skips NaN inside a partly-finite window");
        assert(!fm.ROLLING_MIN("x", 2).some((v) => !Number.isNaN(v) && !Number.isFinite(v)), "no ±Inf ever leaks out of ROLLING_MIN");
    }],
    ["RESAMPLE refuses a NaN timestamp (sortedness check cannot see NaN)", () => {
        // API.md RESAMPLE: buckets are half-open over a sorted time column —
        // a NaN time is unorderable, so it is refused like an unsorted column.
        const ft = new DataFrame({ tv: f64([0, 1, NaN, 3]) });
        assertThrows(() => ft.RESAMPLE("tv", 2, "sum"), "NaN time column refused", RangeError, /must not contain NaN/);
        const fin = new DataFrame({ tv: f64([0, 1, 2, 3]) });
        assertEq(fin.RESAMPLE("tv", 2, "count").ROWS, 2, "finite times still resample");
    }],
    ["JOIN refuses a colliding column whose '_right' rename would truncate", () => {
        // d.ts: JOIN(other, leftKey, rightKey, how) — a colliding right column
        // is carried as '<name>_right'; a rename that does not fit is an error,
        // never a silently truncated output column name.
        const longName = "column_" + "x".repeat(53);
        const l = new DataFrame({ k: i32([0, 1]), [longName]: f64([1, 2]) });
        const r = new DataFrame({ k: i32([0, 1]), [longName]: f64([3, 4]) });
        assertThrows(() => l.JOIN(r, "k", "k", "inner"), "60-char collision + '_right' exceeds the 63-char rename buffer", RangeError, /name limit/);
        const shortName = "c" + "x".repeat(10);
        const l2 = new DataFrame({ k: i32([0, 1]), [shortName]: f64([1, 2]) });
        const r2 = new DataFrame({ k: i32([0, 1]), [shortName]: f64([3, 4]) });
        const f2 = l2.JOIN(r2, "k", "k", "inner");
        assert(f2.COLUMNS.includes(shortName + "_right"), "short renames still produce '<name>_right'");
    }],
    ["QUANTILES: results stay exact and in caller qs order for descending input", () => {
        // d.ts: QUANTILES(col, qs): Float64Array — out[i] answers qs[i];
        // NaN rows are excluded (parity with QUANTILE over the finite values).
        const fq = new DataFrame({ x: f64([5, 1, 9, 3, 7, NaN]) });
        const desc = fq.QUANTILES("x", [1, 0.9, 0.75, 0.5, 0.25, 0]);
        cmpArrNa(Array.from(desc), [9, 8.2, 7, 5, 3, 1], "descending qs answers in caller order (sorted [1,3,5,7,9], m=5)");
        for (const q of [0, 0.25, 0.5, 0.9, 1])
            assertClose(fq.QUANTILES("x", [q])[0], fq.QUANTILE("x", q), EPS, "QUANTILES(" + q + ") parity with QUANTILE");
        const asc = Array.from({ length: 101 }, (_, i) => i / 100);
        const big = new DataFrame({ x: f64(Array.from({ length: 1000 }, (_, i) => i)) });
        const gotDesc = Array.from(big.QUANTILES("x", asc.slice().reverse()));
        const gotAsc = Array.from(big.QUANTILES("x", asc));
        gotAsc.reverse();
        assertEqArr(gotDesc, gotAsc, "100k-shape: descending qs is exactly the reversed ascending answer");
    }],
]);

print("bb_dataframe: all tests passed (" + n + " assertions)");
