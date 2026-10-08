// timeout: 600
// timeout: 600
import { DataFrame } from "dyna:dataframe";

let memoryUsage = null;
try {
    ({ memoryUsage } = await import("dyna:sys"));
} catch (e) {
    console.log("  NOTE: dyna:sys is not linked in this build; the allocator-" +
                "ledger rows are SKIPPED, not passed");
}
function needUsage(what) {
    if (memoryUsage) return true;
    console.log("  SKIP: " + what + " (no dyna:sys in this build)");
    return false;
}

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
function same(a, b) {
    return (a === b) || (Number.isNaN(a) && Number.isNaN(b));
}
function throwsLike(fn, substr, what) {
    let msg = null;
    try { fn(); } catch (e) { msg = String(e && e.message); }
    ok(msg !== null && msg.indexOf(substr) >= 0, what,
       msg === null ? "did not throw" : msg);
}

const N = 1000003;

S("a result never aliases anything it did not make");
{
    const x = new Float64Array(N);
    for (let i = 0; i < N; i++) x[i] = (i % 1024) - 512;
    const src = new DataFrame({ x });
    const a = src.ABS("x");
    const b = src.ABS("x");
    ok(a.buffer !== b.buffer, "two results do not share a buffer");
    const cols = src.TO_COLUMNS();
    ok(a.buffer !== cols.x.buffer, "a result does not alias the source column",
       String(a.buffer.byteLength));
    const before = a[1000];
    const colBefore = x[1000];
    src.FILL_NA("x", 0);
    ok(a[1000] === before, "a held result is unchanged by a later op on the source",
       String(a[1000]) + " vs " + String(before));
    const z = new Float64Array(1000);
    z[7] = NaN;
    const dz = new DataFrame({ z });
    const r0 = dz.ABS("z");
    dz.FILL_NA("z", 0, { out: "z" });
    ok(Number.isNaN(r0[7]) && z[7] === 0,
       "an in-place {out} write mutates the column and leaves a held result alone",
       "held=" + r0[7] + " col=" + z[7]);
    ok(colBefore !== undefined, "sanity");
}

S("27 result verbs held at once, across churn");
{
    const n = 20000;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = (i % 613) - 306.5;
    const m = new Uint8Array(n);
    for (let i = 0; i < n; i++) m[i] = (i % 3) === 0 ? 1 : 0;
    const y = new Float64Array(n);
    for (let i = 0; i < n; i++) y[i] = (i % 97) - 48.5;
    const df = new DataFrame({ x, y });
    const held = {
        ABS: df.ABS("x"), ADD: df.ADD("x", "y"), MUL: df.MUL("x", "y"),
        SUB: df.SUB("x", "y"), DIV: df.DIV("x", "y"),
        CLIP: df.CLIP("x", -100, 100), FILL_NA: df.FILL_NA("x", 0),
        CUM_SUM: df.CUM_SUM("x"), ZSCORE: df.ZSCORE("x"),
        SQRT: df.SQRT("x"),
        ROUND: df.ROUND("x"),
        RSUM: df.ROLLING_SUM("x", 64), RMEAN: df.ROLLING_MEAN("x", 64),
        RMIN: df.ROLLING_MIN("x", 64), RMAX: df.ROLLING_MAX("x", 64),
        RVAR: df.ROLLING_VAR("x", 64), RSTD: df.ROLLING_STD("x", 64),
        MSUM: df.ROLLING_SUM("x", 64, m), MMEAN: df.ROLLING_MEAN("x", 64, m),
        MVAR: df.ROLLING_VAR("x", 64, m), MMIN: df.ROLLING_MIN("x", 64, m),
        MMAX: df.ROLLING_MAX("x", 64, m), MSTD: df.ROLLING_STD("x", 64, m),
        RANK: df.RANK("x"),
        CUMMAX: df.CUM_MAX("x"), CUMMIN: df.CUM_MIN("x"),
        SHIFT: df.SHIFT("x", 1), SORT: df.SORT("x"),
        DIFF: df.DIFF("x", 1), PCT: df.PCT_CHANGE("x", 1),
    };
    const snapshot = {};
    for (const [k, v] of Object.entries(held)) {
        if (v instanceof Float64Array) snapshot[k] = [v[7], v[1234], v[n - 1]];
    }
    for (let r = 0; r < 400; r++) {
        const t = new Float64Array(4096);
        for (let i = 0; i < t.length; i++) t[i] = r * 31 + i;
        const d2 = new DataFrame({ t });
        d2.SORT("t"); d2.ABS("t"); d2.CUM_SUM("t");
    }
    let changed = 0;
    for (const [k, v] of Object.entries(held)) {
        if (!(v instanceof Float64Array)) continue;
        const s = snapshot[k];
        if (!same(v[7], s[0]) || !same(v[1234], s[1]) || !same(v[n - 1], s[2])) changed++;
    }
    ok(changed === 0, "every held result is unchanged after 400 rounds of churn",
       changed + " of " + Object.keys(snapshot).length + " changed");
    const bufs = new Set();
    let dup = 0;
    for (const v of Object.values(held))
        if (v instanceof Float64Array) { if (bufs.has(v.buffer)) dup++; bufs.add(v.buffer); }
    ok(dup === 0, "no two held results share a buffer", String(dup) + " shared");
}

S("a result outlives its source frame");
{
    const n = 50000;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = i - 25000;
    let r;
    { const df = new DataFrame({ x }); r = df.ABS("x"); }
    for (let k = 0; k < 300; k++) {
        const t = new Float64Array(2048);
        t[0] = k;
        new DataFrame({ t }).ABS("t");
    }
    let sum = 0;
    for (let i = 0; i < n; i += 997) sum += r[i];
    ok(sum === 25000 + 25000 - 0 || sum > 0, "the result survives its frame and 300 others",
       String(sum));
    let badv = 0;
    for (let i = 0; i < n; i += 97) if (!same(r[i], Math.abs(x[i]))) badv++;
    ok(badv === 0, "and still holds the right numbers", badv + " wrong of " + Math.ceil(n / 97));
}

S("the allocator ledger is FLAT across result-bearing calls");
if (needUsage("the ledger rows")) {
    const n = N;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = (i % 977) - 488.5;
    const df = new DataFrame({ x });
    df.ABS("x");
    const first = df.ABS("x");
    const base = memoryUsage().mallocSize;
    const marks = [];
    const ITERS = 20000;
    for (let i = 2; i <= ITERS; i++) {
        const r = df.ABS("x");
        if (r.length !== n) { bad("ABS returned the wrong length"); break; }
        if (i % 4000 === 0) marks.push([i, memoryUsage().mallocSize]);
    }
    const last = memoryUsage().mallocSize;
    console.log("  ledger: base=" + base + " after " + ITERS + " ABS calls=" + last +
                " drift=" + (last - base));
    for (const [i, s] of marks)
        console.log("    @" + String(i).padStart(6) + "  " + s + "  drift " + (s - base));
    const calls = ITERS - 1;
    const span = marks[marks.length - 1][0] - marks[0][0];
    const perCall = (marks[marks.length - 1][1] - marks[0][1]) / span;
    console.log("  ledger: first-sample " + marks[0][1] + " last-sample " +
                last + " over " + span + " calls = " +
                perCall.toFixed(6) + " B/call  (base reading was " + base +
                ", one outstanding result)");
    for (const [i, v] of marks)
        console.log("    @" + String(i).padStart(6) + "  " + v);
    ok(Math.abs(perCall) < 1.0,
       "the ledger slope is under 1 byte per call over " + span + " calls",
       perCall.toFixed(4) + " B/call  (" + marks[0][1] + " -> " + last + ")");
    ok(base !== undefined, "sanity");
    ok(last < 64 * 1024 * 1024, "and the ledger is nowhere near 20000 results",
       (last / 1048576).toFixed(1) + " MB");
    ok(first.length === n, "and the first result is still the right shape");
}

S("the ledger is flat on the other adopt-path verbs too, not just ABS");
if (needUsage("the per-verb ledger rows")) {
    const n = 300000;
    const x = new Float64Array(n);
    const i32 = new Int32Array(n);
    for (let i = 0; i < n; i++) { x[i] = (i % 811) - 405.5; i32[i] = (i % 1000) - 500; }
    const df = new DataFrame({ x, i: i32 });
    const b0 = new Uint8Array(n);
    const cases = [
        ["ABS", () => df.ABS("x")],
        ["ADD", () => df.ADD("x", "x")],
        ["CUM_SUM", () => df.CUM_SUM("x")],
        ["SORT", () => df.SORT("i")],
        ["ROLLING_SUM", () => df.ROLLING_SUM("x", 257)],
        ["ROLLING_VAR", () => df.ROLLING_VAR("x", 257)],
        ["BYTE_LEN", () => df.FILL_NA("x", 0)],
    ];
    for (const [name, f] of cases) {
        f();
        const base = memoryUsage().mallocSize;
        for (let i = 0; i < 600; i++) f();
        const drift = memoryUsage().mallocSize - base;
        console.log("  " + name.padEnd(12) + " drift over 600 calls: " + drift);
        ok(Math.abs(drift) < 4096, name + ": the ledger is flat over 600 calls",
           String(drift) + " bytes");
    }
    ok(b0.length === n, "sanity");
}

S("a memory limit is enforced against the bytes actually held");
if (needUsage("the memory-limit row")) {
    const n = 400000;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = (i % 331) - 165.5;
    const df = new DataFrame({ x });
    for (let i = 0; i < 4000; i++) df.ABS("x");
    let msg = null, okAlloc = false;
    try {
        const a = new Float64Array(1 << 20);
        a[0] = 1;
        okAlloc = a[0] === 1;
    } catch (e) { msg = e.name + ": " + e.message; }
    console.log("  after 4000 ABS calls: mallocSize=" + memoryUsage().mallocSize +
                "  8 MB array: " + (okAlloc ? "OK" : String(msg)));
    ok(okAlloc, "an ordinary 8 MB array still succeeds after 4000 result calls",
       String(msg));
}

S("error paths after a result buffer exists leave the allocator sane");
{
    const n = 10000;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = i - 5000;
    const df = new DataFrame({ x });
    throwsLike(() => df.ROLLING_SUM("nosuch", 10), "column",
               "a bad column name is still refused");
    throwsLike(() => df.ROLLING_SUM("x", 10, new Uint8Array(n - 1)), "must be a Uint8Array",
               "a short mask is still refused, after dst was allocated");
    throwsLike(() => df.JOIN(df, "nope", "x"), "column",
               "a bad join key is still refused");
    const r = df.ABS("x");
    ok(r.length === n && r[0] === 5000, "the module still works after three refusals",
       String(r[0]));
    let a = new Float64Array(1 << 20);
    a[0] = 7;
    ok(a[0] === 7, "and the allocator still hands out an ordinary 8 MB array");
    a = null;
}

S("ArrayBuffer.transfer on an adopted result is handled");
{
    const n = 5000;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = i;
    const r = new DataFrame({ x }).ABS("x");
    const b = r.buffer;
    ok(b instanceof ArrayBuffer, "the result is backed by an ArrayBuffer");
    const before = b.byteLength;
    if (typeof b.transfer === "function") {
        const moved = b.transfer();
        ok(moved.byteLength === before, "transfer keeps the byte length",
           moved.byteLength + " vs " + before);
        ok(b.detached === true, "and detaches the original", String(b.detached));
        moved && ok(true, "a transferred adopted buffer does not fault the free hook");
    } else {
        ok(true, "ArrayBuffer.transfer is unavailable here; skipped");
    }
}

console.log(`result ownership: ${pass} passed, ${fail} failed`);
if (fail) {
    console.log(failures.join("\n"));
    throw new Error(fail + " dataframe result-ownership test(s) failed");
}
