// timeout: 600
// timeout: 600
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0, section = "(none)";
const failures = [];
function S(name) { section = name; }
function bad(what, detail) {
    fail++;
    const line = "FAIL [" + section + "] " + what + (detail ? ": " + detail : "");
    failures.push(line);
    console.log(line);
}
function ok(cond, what, detail) { if (cond) pass++; else bad(what, detail); }
function eq(a, b, what) {
    ok((a === b) || (Number.isNaN(a) && Number.isNaN(b)), what, a + " !== " + b);
}
function throwsLike(fn, substr, what) {
    let msg = null, name = null;
    try { fn(); } catch (e) { msg = String(e && e.message); name = e && e.name; }
    ok(msg !== null && msg.indexOf(substr) >= 0, what,
       msg === null ? "did not throw" : name + ": " + msg);
}

function frames(n, D, ncol) {
    const k = new Int32Array(n), v = new Float64Array(n);
    for (let i = 0; i < n; i++) { k[i] = i % D; v[i] = i; }
    const L = { k, v }, R = { k, v };
    for (let c = 1; c < ncol; c++) { L["c" + c] = v; R["c" + c] = v; }
    return [new DataFrame(L), new DataFrame(R)];
}

S("a runaway JOIN product is refused in time bounded by the input");
{
    const time = (L, R, reps) => {
        let best = Infinity;
        for (let r = 0; r < reps; r++) {
            const t0 = performance.now();
            try { L.JOIN(R, "k", "k"); } catch (e) {}
            const dt = performance.now() - t0;
            if (dt < best) best = dt;
        }
        return best;
    };
    const seen = [];
    for (const n of [40000, 80000, 160000]) {
        const [L, R] = frames(n, 8, 1);
        throwsLike(() => L.JOIN(R, "k", "k"), "too many matches",
                   "n=" + n + " refuses a runaway product");
        const dt = time(L, R, 5);
        seen.push({ n, dt });
        console.log("  JOIN refuse n=" + n + " D=8: " + dt.toFixed(2) + " ms");
    }
    for (const s of seen)
        ok(s.dt < 5000, "JOIN n=" + s.n + " refused promptly",
           s.dt.toFixed(1) + " ms");
    const grow = seen[2].dt / Math.max(seen[0].dt, 0.01);
    ok(grow < 10, "the refusal must not scale with the pair product",
       "n=160000 cost " + seen[2].dt.toFixed(2) + " ms vs " +
       seen[0].dt.toFixed(2) + " ms at n=40000, ratio " + grow.toFixed(2) + "x");
}

S("the refusal is still a RangeError that names the cause");
{
    const [L, R] = frames(200000, 8, 1);
    let msg = null, name = null;
    try { L.JOIN(R, "k", "k"); } catch (e) { msg = e.message; name = e.name; }
    ok(name === "RangeError", "a runaway JOIN still throws RangeError", String(name));
    ok(msg && msg.indexOf("JOIN") === 0, "the message names the method", String(msg));
    ok(msg && msg.indexOf("too many matches") >= 0,
       "the message still says why", String(msg));
}

S("legitimate joins are untouched");
{
    let shapes = 0;
    for (const [n, D] of [[1000, 1], [1000, 1000], [5000, 7], [20000, 64]]) {
        const [L, R] = frames(n, D, 1);
        const cnt = new Int32Array(D);
        for (let i = 0; i < n; i++) cnt[i % D]++;
        let inner = 0;
        for (let i = 0; i < n; i++) inner += cnt[i % D];
        const cl = new Int32Array(D), cr = new Int32Array(D);
        for (let i = 0; i < n; i++) { cl[i % D]++; cr[i % D]++; }
        let outer = 0;
        for (let i = 0; i < D; i++) outer += cl[i] * cr[i];
        eq(L.JOIN(R, "k", "k").ROWS, inner, "inner n=" + n + " D=" + D);
        eq(L.JOIN(R, "k", "k", "left").ROWS, inner, "left n=" + n + " D=" + D);
        eq(L.JOIN(R, "k", "k", "right").ROWS, inner, "right n=" + n + " D=" + D);
        eq(L.JOIN(R, "k", "k", "outer").ROWS, outer, "outer n=" + n + " D=" + D);
        shapes++;
    }
    ok(shapes === 4, "the join sweep really ran", shapes + " shapes");
    const [L1, R1] = frames(2000, 1, 1);
    eq(L1.JOIN(R1, "k", "k").ROWS, 4000000, "a 1-key 2000x2000 join is 4e6 rows");
    const la = new DataFrame({ k: new Int32Array([1, 1, 2]), v: new Float64Array([1, 2, 3]) });
    const ra = new DataFrame({ k: new Int32Array([2, 3]), v: new Float64Array([4, 5]) });
    eq(la.JOIN(ra, "k", "k").ROWS, 1, "inner keeps only the matched pair");
    eq(la.JOIN(ra, "k", "k", "left").ROWS, 3, "left pads the two unmatched left rows");
    eq(la.JOIN(ra, "k", "k", "right").ROWS, 2, "right pads the unmatched right row");
    eq(la.JOIN(ra, "k", "k", "outer").ROWS, 4, "outer pads both sides");
}

S("PIVOT refuses an oversized dense rectangle before allocating it");
{
    const n = 200000, ni = 180000, np = 1000;
    const idx = new Int32Array(n), pv = new Int32Array(n);
    for (let i = 0; i < n; i++) { idx[i] = i % ni; pv[i] = i % np; }
    const df = new DataFrame({ i: idx, p: pv, v: new Float64Array(n) });
    const t0 = performance.now();
    throwsLike(() => df.PIVOT("i", "p", "v"), "accumulator slots",
               "a 180e6-slot PIVOT is refused");
    const dt = performance.now() - t0;
    console.log("  PIVOT refuse 180000x1000: " + dt.toFixed(2) + " ms");
    ok(dt < 1500, "the PIVOT refusal is prompt", dt.toFixed(1) + " ms");
    let msg = null;
    try { df.PIVOT("i", "p", "v"); } catch (e) { msg = e.message; }
    ok(msg && msg.indexOf("180000 index values x 1000 pivot values") >= 0,
       "the message names the two dimensions", String(msg));
    ok(msg && msg.indexOf("180000000 accumulator slots") >= 0,
       "the message names the slot count", String(msg));
}

S("PIVOT's bound is on accumulator BYTES, so it moves with the agg");
{
    const n = 20000, ni = 20000, np = 1000;
    const idx = new Int32Array(n), pv = new Int32Array(n);
    for (let i = 0; i < n; i++) { idx[i] = i % ni; pv[i] = i % np; }
    const v = new Float64Array(n);
    for (let i = 0; i < n; i++) v[i] = i + 1;
    const df = new DataFrame({ i: idx, p: pv, v });
    throwsLike(() => df.PIVOT("i", "p", "v", "first"), "accumulator slots",
               "agg=first at 2.0e7 slots (400 MB) is refused");
    const r = df.PIVOT("i", "p", "v", "count");
    eq(r.ROWS, ni, "agg=count at the same 2.0e7 slots (80 MB) builds");
    eq(r.COLS, np + 1, "and has one column per pivot value plus the index");
    const a = (() => { try { df.PIVOT("i", "p", "v", "first"); return ""; }
                        catch (e) { return e.message; } })();
    const b = (() => { try { df.PIVOT("i", "p", "v", "last"); return ""; }
                        catch (e) { return e.message; } })();
    ok(a.indexOf("13421772") >= 0, "first/last get the 20 B/cell ceiling (13.4e6)",
       String(a));
    ok(b.indexOf("13421772") >= 0, "last is priced like first", String(b));
}

S("PIVOT still builds the wide pivots it exists for");
{
    for (const [n, ni, np] of [[20000, 200, 50], [20000, 5000, 1000],
                               [20000, 1023, 1023]]) {
        const idx = new Int32Array(n), pv = new Int32Array(n);
        for (let i = 0; i < n; i++) { idx[i] = i % ni; pv[i] = i % np; }
        const v = new Float64Array(n);
        for (let i = 0; i < n; i++) v[i] = i + 1;
        const df = new DataFrame({ i: idx, p: pv, v });
        const r = df.PIVOT("i", "p", "v");
        eq(r.ROWS, ni, "PIVOT " + ni + "x" + np + " has one row per index value");
        eq(r.COLS, np + 1, "PIVOT " + ni + "x" + np + " has one column per pivot value");
    }
    const n = 20, ni = 4, np = 3;
    const idx = new Int32Array(n), pv = new Int32Array(n);
    const v = new Float64Array(n);
    for (let i = 0; i < n; i++) { idx[i] = i % ni; pv[i] = i % np; v[i] = i + 1; }
    const df = new DataFrame({ i: idx, p: pv, v });
    const r = df.PIVOT("i", "p", "v", "sum");
    const cols = r.TO_COLUMNS();
    const keys = Object.keys(cols);
    ok(keys.length === np + 1, "the sum pivot has a column per pivot value plus the index",
       keys.length + " columns");
    ok(cols[keys[0]].length === ni, "and one row per index value",
       String(cols[keys[0]].length));
    const m2 = 2000;
    const wid = new Int32Array(m2), wpv = new Int32Array(m2);
    for (let i = 0; i < m2; i++) { wid[i] = i % 7; wpv[i] = i; }
    const d2 = new DataFrame({ i: wid, p: wpv, v: new Float64Array(m2) });
    throwsLike(() => d2.PIVOT("i", "p", "v"), "DF_MAX_COLS",
               "more pivot values than DF_MAX_COLS is still refused");
}

console.log(`join/pivot caps: ${pass} passed, ${fail} failed`);
if (fail) {
    console.log(failures.join("\n"));
    throw new Error(fail + " dataframe join/pivot cap test(s) failed");
}
