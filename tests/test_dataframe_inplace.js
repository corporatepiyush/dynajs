// flags: --std
/* test_dataframe_inplace.js —: {out: colName} in-place variants of the
 * elementwise verbs. Contracts under test:
 *   - in-place results are ELEMENT-IDENTICAL to the out-of-place outputs
 *     (numpy differential on the same data)
 *   - the target must be an EXISTING Float64Array column of exactly nrows
 *     entries (TypeError / RangeError otherwise), bag is strict
 *   - the verbs return the frame for chaining; out === source aliasing and
 *     caller-owned buffers are safe
 *   - the window family (rolling min/max/mean/sum/var/std, ema, pctChange,
 *     zscore) honours the same contract
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_dataframe_inplace.js */

import { DataFrame } from "dyna:dataframe";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
function throws(fn, m) { let t = false; try { fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mk(cols) { return new DataFrame(cols); }

/* ---------------- arithmetic verbs: identity vs out-of-place ---------------- */
{
    const A = [1, -2, 3, -4.5, NaN, 0, 7];
    const f = mk({ a: Float64Array.from(A), sink: new Float64Array(A.length) });
    /* build a fixed corpus once; every verb's in-place result must equal the
     * out-of-place result element for element (Object.is: NaN positions too) */
    const cases = [
        ["ABS",   (df) => df.ABS("a"),        (df) => df.ABS("a", { out: "sink" })],
        ["ROUND", (df) => df.ROUND("a"),      (df) => df.ROUND("a", { out: "sink" })],
        ["FLOOR", (df) => df.FLOOR("a"),      (df) => df.FLOOR("a", { out: "sink" })],
        ["CEIL",  (df) => df.CEIL("a"),       (df) => df.CEIL("a", { out: "sink" })],
        ["SQRT",  (df) => df.SQRT("a"),       (df) => df.SQRT("a", { out: "sink" })],
        ["EXP",   (df) => df.EXP("a"),        (df) => df.EXP("a", { out: "sink" })],
        ["SIGN",  (df) => df.SIGN("a"),       (df) => df.SIGN("a", { out: "sink" })],
        ["ADD",   (df) => df.ADD("a", 2),     (df) => df.ADD("a", 2, { out: "sink" })],
        ["SUB",   (df) => df.SUB("a", 2),     (df) => df.SUB("a", 2, { out: "sink" })],
        ["MUL",   (df) => df.MUL("a", 2.5),   (df) => df.MUL("a", 2.5, { out: "sink" })],
        ["DIV",   (df) => df.DIV("a", 2),     (df) => df.DIV("a", 2, { out: "sink" })],
        ["POW",   (df) => df.POW("a", 2),     (df) => df.POW("a", 2, { out: "sink" })],
        ["RSUB",  (df) => df.RSUB("a", 10),   (df) => df.RSUB("a", 10, { out: "sink" })],
        ["RDIV",  (df) => df.RDIV("a", 10),   (df) => df.RDIV("a", 10, { out: "sink" })],
        ["CLIP",  (df) => df.CLIP("a", -2, 3), (df) => df.CLIP("a", -2, 3, { out: "sink" })],
        ["FILL_NA", (df) => df.FILL_NA("a", -1), (df) => df.FILL_NA("a", -1, { out: "sink" })],
    ];
    for (const [name, ref, inplace] of cases) {
        const f1 = mk({ a: Float64Array.from(A), sink: new Float64Array(A.length) });
        const f2 = mk({ a: Float64Array.from(A), sink: new Float64Array(A.length) });
        const want = ref(f1);
        const got = inplace(f2);
        assert(got === f2, `${name} {out} returns the frame`);
        assert(eqArr([...want], [...f2.TO_COLUMNS().sink]),
            `${name} in-place output is element-identical`);
        assert(eqArr([...f1.TO_COLUMNS().sink], [...Array(A.length).fill(0)]),
            `${name} out-of-place form leaves the sink column untouched`);
    }
    /* log over the positives only (log of the negatives is NaN -- same rule
     * both forms, so it still round-trips) */
    const f1 = mk({ a: Float64Array.from(A), sink: new Float64Array(A.length) });
    const f2 = mk({ a: Float64Array.from(A), sink: new Float64Array(A.length) });
    assert(eqArr([...f1.LOG("a")], [...f2.LOG("a", { out: "sink" }).TO_COLUMNS().sink]),
        "LOG in-place identity");
}

/* column-vs-column right operand, and out === source aliasing */
{
    const f = mk({
        x: Float64Array.from([1, 2, 3]),
        y: Float64Array.from([10, 20, 30]),
        z: new Float64Array(3),
    });
    const want = f.ADD("x", "y");
    f.ADD("x", "y", { out: "z" });
    assert(eqArr([...want], [...f.TO_COLUMNS().z]), "ADD(col, col, {out})");
    /* out === source: safe because the kernel writes a scratch buffer back */
    const f2 = mk({ x: Float64Array.from([1, 2, 3]) });
    f2.MUL("x", 10, { out: "x" });
    assert(eqArr([10, 20, 30], [...f2.TO_COLUMNS().x]), "out === source aliasing");
    /* the SCREAMING aliases accept the same bag */
    const f3 = mk({ x: Float64Array.from([1, 2]), s: new Float64Array(2) });
    assert(f3.SUB("x", 1, { out: "s" }) === f3, "SCREAMING alias takes {out}");
    /* caller-owned backing buffer is mutated in place */
    const owned = new Float64Array([9, 9]);
    const f4 = new DataFrame({ x: Float64Array.from([1, 2]), o: owned });
    f4.ADD("x", 5, { out: "o" });
    assert(eqArr([...owned], [6, 7]), "caller-owned out buffer mutated");
}

/* chaining: pipelines stop allocating intermediate Float64Arrays */
{
    const f = mk({
        a: Float64Array.from([1, 2, 3, 4]),
        b: new Float64Array(4),
        c: new Float64Array(4),
    });
    const got = f.ADD("a", 1, { out: "b" }).MUL("b", 2, { out: "c" }).SUB("c", 2, { out: "b" });
    assert(got === f, "chaining returns the same frame");
    assert(eqArr([...f.TO_COLUMNS().b], [2, 4, 6, 8]), "pipeline value");
}

/* ---------------- target validation ---------------- */
{
    const f = mk({
        f64: new Float64Array(3),
        i32: new Int32Array(3),
        f32: new Float32Array(3),
        u8: new Uint8Array(3),
    });
    for (const [col, tn] of [["i32", "Int32Array"], ["f32", "Float32Array"], ["u8", "Uint8Array"]]) {
        throws(() => f.ADD("f64", 1, { out: col }),
            `${tn} target throws TypeError`);
        let msg = "";
        try { f.ADD("f64", 1, { out: col }); } catch (e) { msg = e.message; }
        assert(msg.includes(col) && msg.includes(tn), `error names the column and its type (${tn})`);
    }
    throws(() => f.ADD("f64", 1, { out: "nope" }), "missing out column throws");
    throws(() => f.ADD("f64", 1, { out: 42 }), "non-string out throws");
    throws(() => f.ADD("f64", 1, { oute: "f64" }), "typo'd bag key throws");
    throws(() => f.ADD("f64", 1, { out: "i32", extra: 1 }), "extra bag key throws");
    /* a string column target */
    const fs = new DataFrame({ a: Float64Array.from([1]), s: ["x"] });
    throws(() => fs.ADD("a", 1, { out: "s" }), "string column target throws");
    /* A short out column cannot be constructed through the ctor (it refuses
     * ragged columns), so the length guard in C is unreachable from here --
     * documented as defense-in-depth, not an observable error. */
}

/* ---------------- window family ---------------- */
{
    const V = [3, 1, 4, 1, 5, 9, 2, 6];
    const verbs = [
        ["ROLLING_MIN",  (df) => df.ROLLING_MIN("v", 3),  (df) => df.ROLLING_MIN("v", 3, { out: "w" })],
        ["ROLLING_MAX",  (df) => df.ROLLING_MAX("v", 3),  (df) => df.ROLLING_MAX("v", 3, { out: "w" })],
        ["ROLLING_MEAN", (df) => df.ROLLING_MEAN("v", 3), (df) => df.ROLLING_MEAN("v", 3, { out: "w" })],
        ["ROLLING_SUM",  (df) => df.ROLLING_SUM("v", 3),  (df) => df.ROLLING_SUM("v", 3, { out: "w" })],
        ["ROLLING_VAR",  (df) => df.ROLLING_VAR("v", 3),  (df) => df.ROLLING_VAR("v", 3, { out: "w" })],
        ["ROLLING_STD",  (df) => df.ROLLING_STD("v", 3),  (df) => df.ROLLING_STD("v", 3, { out: "w" })],
        ["EMA",         (df) => df.EMA("v", 0.5),       (df) => df.EMA("v", 0.5, { out: "w" })],
        ["PCT_CHANGE",   (df) => df.PCT_CHANGE("v"),      (df) => df.PCT_CHANGE("v", { out: "w" })],
        ["pctChange2",  (df) => df.PCT_CHANGE("v", 2),   (df) => df.PCT_CHANGE("v", 2, { out: "w" })],
        ["ZSCORE",      (df) => df.ZSCORE("v"),         (df) => df.ZSCORE("v", { out: "w" })],
    ];
    for (const [name, ref, inplace] of verbs) {
        const f1 = mk({ v: Float64Array.from(V), w: new Float64Array(V.length) });
        const f2 = mk({ v: Float64Array.from(V), w: new Float64Array(V.length) });
        const want = ref(f1);
        const got = inplace(f2);
        assert(got === f2, `${name} {out} returns the frame`);
        assert(eqArr([...want], [...f2.TO_COLUMNS().w]),
            `${name} in-place output is element-identical`);
    }
    /* SCREAMING window aliases */
    const f = mk({ v: Float64Array.from(V), w: new Float64Array(V.length) });
    assert(f.ROLLING_MEAN("v", 2, { out: "w" }) === f, "ROLLING_MEAN takes {out}");
    assert(f.EMA("v", 0.5, { out: "w" }) === f, "EMA alias takes {out}");
    assert(f.ZSCORE("v", { out: "w" }) === f, "ZSCORE alias takes {out}");
    /* mask + out compose (mask at its slot, out after) */
    const mask = Uint8Array.from([1, 1, 1, 0, 1, 1, 1, 1]);
    const f1 = mk({ v: Float64Array.from(V), w: new Float64Array(V.length) });
    const f2 = mk({ v: Float64Array.from(V), w: new Float64Array(V.length) });
    const want = f1.ROLLING_MEAN("v", 2, mask);
    f2.ROLLING_MEAN("v", 2, mask, { out: "w" });
    assert(eqArr([...want], [...f2.TO_COLUMNS().w]), "mask + {out} compose");
}

/* rev2: a bag in the OPERAND slot is refused, not read as a NaN operand */
{
    const f = mk({ x: Float64Array.from([1, 2, 3]), o: new Float64Array(3) });
    for (const op of ["ADD", "SUB", "MUL", "DIV", "POW", "RSUB", "RDIV"]) {
        let msg = "";
        try { f[op]("x", { out: "o" }); } catch (e) { msg = e.message; }
        /* the message names the op in its SCREAMING spelling */
        assert(msg.toUpperCase().includes(op.toUpperCase()) &&
               msg.includes("right operand"), `${op} refuses a bag operand`);
        assert(eqArr([...f.TO_COLUMNS().x], [1, 2, 3]), `${op} left data untouched`);
        assert(eqArr([...f.TO_COLUMNS().o], [0, 0, 0]), `${op} out untouched`);
    }
    /* FILL_NA would no-op silently on a NaN fill: refused too */
    let msg = "";
    try { f.FILL_NA("x", { out: "o" }); } catch (e) { msg = e.message; }
    assert(msg.includes("FILL_NA") && msg.includes("fill value"),
        "FILL_NA refuses a bag value");
    /* the numeric and string operand forms are unchanged */
    assert(eqArr([...f.ADD("x", 1)], [2, 3, 4]), "numeric operand unchanged");
    assert(eqArr([...f.ADD("x", "x")], [2, 4, 6]), "column operand unchanged");
    /* ROLLING_VAR/STD with a bag-only form: the defaulted-away window is a
     * DETERMINISTIC 0 (dfo_open zeroes the skipped scalar), so the call
     * rejects with the same RangeError every time -- never a garbage window
     * from uninitialized stack */
    {
        const fw = mk({ v: Float64Array.from([1, 2, 3, 4]), w: new Float64Array(4) });
        let seen = new Set();
        for (let i = 0; i < 20; i++) {
            try { fw.ROLLING_VAR("v", { out: "w" }); } catch (e) { seen.add(e.message); }
        }
        assert(seen.size === 1, "ROLLING_VAR(col, {out}) rejects deterministically");
        assert([...seen][0].includes("window must be a positive integer"),
            "rejection names the window");
        try { fw.ROLLING_STD("v", { out: "w" }); } catch (e) {
            assert(e.message.includes("window must be a positive integer"),
                "ROLLING_STD same deterministic rejection");
        }
    }
}

/* out-of-place forms are untouched: same binary, same results */
{
    const f = mk({ a: Float64Array.from([1, 2, 3]) });
    assert(f.ADD("a", 1) instanceof Float64Array, "no bag -> Float64Array, as before");
    assert(f.ADD("a", 1, undefined) instanceof Float64Array, "explicit undefined bag");
    assert(f.ADD("a", 1, null) instanceof Float64Array, "null bag");
}

/* numpy differential: ADD/SUB/MUL/DIV in-place vs the same ops on the same
 * corpus (pinned here; generated with numpy 2.5) */
{
    const xs = [0.1, 0.7, 1.3, 2.9, 4.2, 5.5];
    const pins = {
        add2: xs.map(x => x + 2),
        mul3: xs.map(x => x * 3),
        div4: xs.map(x => x / 4),
        sub1: xs.map(x => x - 1),
    };
    const f = mk({
        a: Float64Array.from(xs),
        b: new Float64Array(xs.length),
        c: new Float64Array(xs.length),
        d: new Float64Array(xs.length),
        e: new Float64Array(xs.length),
    });
    f.ADD("a", 2, { out: "b" }).MUL("a", 3, { out: "c" })
     .DIV("a", 4, { out: "d" }).SUB("a", 1, { out: "e" });
    assert(eqArr(pins.add2, [...f.TO_COLUMNS().b]), "ADD 2 vs numpy");
    assert(eqArr(pins.mul3, [...f.TO_COLUMNS().c]), "MUL 3 vs numpy");
    assert(eqArr(pins.div4, [...f.TO_COLUMNS().d]), "DIV 4 vs numpy");
    assert(eqArr(pins.sub1, [...f.TO_COLUMNS().e]), "SUB 1 vs numpy");
}

console.log(`test_dataframe_inplace: ${n} assertions ok`);
