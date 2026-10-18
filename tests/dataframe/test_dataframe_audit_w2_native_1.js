// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_1.js: the dataframe sections.
import { DataFrame } from "dyna:dataframe";
import { args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
// ---- dataframe ---------------------------------------------------------------
{
    const sab = new Float64Array(new SharedArrayBuffer(24));
    sab.set([1, 2, 3]);
    const df = new DataFrame({ sab });
    sab[0] = 100;
    eq(df.SUM("sab"), 6, "a SharedArrayBuffer column is a snapshot taken at construction (M1a-01)");
    const mask = new Uint8Array(new SharedArrayBuffer(3));
    eq(errName(() => df.SUM("sab", mask)), "TypeError", "a SharedArrayBuffer mask is refused");

    const cells = ["=SUM(A1,B1)", "plain", "-1,5", "@x\"y", "=a\nb", "+1"];
    const csv = new DataFrame({ s: cells, n: new Float64Array([1, 2, 3, 4, 5, 6]) }).TO_CSV({ escapeFormulas: true });
    const want = "s,n\n\"'=SUM(A1,B1)\",1\nplain,2\n\"'-1,5\",3\n\"'@x\"\"y\",4\n\"'=a\nb\",5\n'+1,6\n";
    eq(csv, want, "TO_CSV formula guard sits inside the quotes (M1a-03)");

    const nums = new DataFrame({ x: new Float64Array([0.1, 1 / 3, 1e21, -0, Infinity, -Infinity, NaN, 123456789.125]) }).TO_CSV();
    eq(nums, "x\n0.1\n0.3333333333333333\n1e+21\n0\nInfinity\n-Infinity\n\n123456789.125\n", "TO_CSV prints the shortest round-trip form (M1a-07)");

    const n = 60000, w = 30000;
    const a = new Float64Array(n);
    for (let i = 0; i < n; i++) a[i] = i % 97;
    const clean = new DataFrame({ a: a.slice() });
    const t0 = performance.now();
    const base = clean.ROLLING_VAR("a", w);
    const tClean = performance.now() - t0;
    a[w + 3] = NaN;
    const dirty = new DataFrame({ a });
    const t1 = performance.now();
    const got = dirty.ROLLING_VAR("a", w);
    const tDirty = performance.now() - t1;
    ok(tDirty < Math.max(tClean, 0.5) * 50, "one NaN does not turn ROLLING_VAR quadratic (clean " + tClean.toFixed(2) + "ms, with NaN " + tDirty.toFixed(2) + "ms) (M1a-04)");
    let wrong = 0;
    for (let i = 0; i < n; i++) {
        const inWin = i >= w - 1 && (w + 3) <= i && (w + 3) > i - w;
        if (i < w - 1) { if (!Number.isNaN(got[i])) wrong++; }
        else if (inWin) { if (!Number.isNaN(got[i])) wrong++; }
        else if (Math.abs(got[i] - base[i]) > 1e-9 * Math.abs(base[i])) wrong++;
    }
    eq(wrong, 0, "ROLLING_VAR is NaN exactly while the NaN is inside the window");
    const small = new DataFrame({ x: new Float64Array([1, 2, NaN, 4, 5, 6, 8]) });
    const sv = Array.from(small.ROLLING_VAR("x", 2));
    eq(JSON.stringify(sv), JSON.stringify([null, 0.5, null, null, 0.5, 0.5, 2]), "small-window ROLLING_VAR values (control)");

    const isinDf = new DataFrame({ k: new Float64Array([1, 2, 3]) });
    const vals = new Array(20000).fill(1);
    vals.push({ valueOf() { throw new RangeError("boom"); } });
    eq(errName(() => isinDf.ISIN("k", vals)), "RangeError", "ISIN surfaces the coercion error itself (M1a-05)");
    eq(Array.from(isinDf.ISIN("k", [2, 3])).join(""), "011", "ISIN still answers (control)");
}
if (failures) {
    print("test_dataframe_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_dataframe_audit_w2_native_1 failed");
}
print("test_dataframe_audit_w2_native_1: " + checks + " checks passed");
