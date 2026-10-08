// flags: --std
// timeout: 300
// tests/test_audit_w2_native_1.js -- native-module regressions from audit
// wave 2. Each block names the finding it pins; every assertion fails on the
// pre-fix binary (most as a sanitizer report or a crash, which is why this
// suite is also in the sanitizer leg).
//   borrow pin   a buffer lent to a running native call cannot be detached,
//                resized or transferred from inside that call (D1-p8, D3-01)
//   resource pin a native object cannot be closed from inside one of its own
//                methods (M1b-04)
//   S1-01        X.deserialize() never trusts X.prototype.constructor
//   N2-A/B/C     URLSearchParams iteration, dot-segment removal, IDNA bound
//   M1a-01/03/04/05/07  dataframe
//   M1b-03/05/06/08/09  ml
//   M1c-23/24    Levenshtein band, Decimal OOM path
// build-note: needs the native modules (CONFIG_NATIVE_MODULES=y)
import * as enc from "dyna:encoding";
import * as ml from "dyna:ml";
import { Fenwick, BitSet, UnionFind, SegTree, BloomFilter, Trie, LRU, SortedSet, SortedMap, Heap, List, Deque, RingBuffer } from "dyna:structures";
import { URL, URLSearchParams } from "dyna:url";
import { DataFrame } from "dyna:dataframe";
import { Levenshtein } from "dyna:matcher";
import { PBKDF2 } from "dyna:crypto";
import { Exec, args } from "dyna:sys";

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

// ---- borrow pin ------------------------------------------------------------
{
    const ab = new ArrayBuffer(1 << 16, { maxByteLength: 1 << 17 });
    const u = new Uint8Array(ab);
    u.fill(0xE3);
    const seen = [];
    const r = enc.detectEncoding(u, {
        get fallback() {
            seen.push(errName(() => ab.transfer()));
            seen.push(errName(() => ab.resize(0)));
            seen.push(errName(() => ab.transferToFixedLength()));
            return "latin1";
        },
    });
    eq(seen.join(","), "TypeError,TypeError,TypeError", "a borrowed buffer refuses transfer/resize/transferToFixedLength");
    eq(ab.byteLength, 1 << 16, "the borrowed buffer kept its length");
    eq(typeof r, "string", "detectEncoding completed on the intact buffer");
    eq(errName(() => ab.resize(8)), "none", "the pin ends when the native call returns");
    eq(ab.byteLength, 8, "resize works again afterwards");
    const moved = ab.transfer();
    eq(ab.byteLength, 0, "transfer works again afterwards");
    eq(moved.byteLength, 8, "transfer moved the bytes");
}
{
    // A buffer borrowed by an INNER native call may be detached by the outer
    // script once that inner call has returned, even while an unrelated
    // native call (Array.prototype.forEach) is still on the stack.
    const ab = new ArrayBuffer(64);
    let name = "?";
    [1].forEach(() => {
        enc.detectEncoding(new Uint8Array(ab));
        name = errName(() => ab.transfer());
    });
    eq(name, "none", "a finished inner borrow does not outlive its call");
}
{
    // PBKDF2 reads `iterations`/`length` AFTER it has borrowed `password`
    // (D3-01 shape): the getters must not be able to shrink that buffer.
    const ab = new ArrayBuffer(4096, { maxByteLength: 8192 });
    const pw = new Uint8Array(ab);
    pw.fill(3);
    const salt = new Uint8Array(16);
    const want = Array.from(PBKDF2({ password: pw, salt, iterations: 2, length: 32 })).join();
    const inner = [];
    const opts = { password: pw, salt };
    for (const k of ["iterations", "length"]) {
        Object.defineProperty(opts, k, {
            enumerable: true,
            get() { inner.push(errName(() => ab.resize(1))); return k === "iterations" ? 2 : 32; },
        });
    }
    const got = Array.from(PBKDF2(opts)).join();
    eq(inner.join(","), "TypeError,TypeError", "PBKDF2: option getters cannot shrink the borrowed password");
    eq(got, want, "PBKDF2 derived the key from the intact password");
}

// ---- resource pin (M1b-04) ---------------------------------------------------
{
    let pipe, got = "?";
    const evil = { fit() { got = errName(() => pipe.close()); return this; }, transform(X) { return X; } };
    const last = { fit() { return this; }, predict() { return [0]; } };
    pipe = new ml.Pipeline([evil, evil, last]);
    const outer = errName(() => pipe.fit([[1, 2], [3, 4]], [0, 1]));
    eq(got, "TypeError", "a stage cannot close the pipeline that is running it");
    eq(outer, "none", "the pipeline finished its fit");
    eq(pipe.closed, false, "the pipeline is still open");
    pipe.close();
    eq(pipe.closed, true, "close works once no method is running");
}

// ---- S1-01 -------------------------------------------------------------------
{
    const cases = [
        ["Fenwick", Fenwick, () => { const f = new Fenwick(512); for (let i = 0; i < 512; i++) f.update(i, i + 0.5); return f; }, (c) => new c(1)],
        ["BitSet", BitSet, () => { const b = new BitSet(); for (let i = 0; i < 4096; i += 3) b.set(i); return b; }, (c) => new c()],
        ["UnionFind", UnionFind, () => { const u = new UnionFind(512); for (let i = 1; i < 512; i += 2) u.union(i - 1, i); return u; }, (c) => new c(1)],
        ["SegTree", SegTree, () => { const s = new SegTree(256, "sum"); for (let i = 0; i < 256; i++) s.update(i, i); return s; }, (c) => new c(1, "sum")],
        ["BloomFilter", BloomFilter, () => { const b = new BloomFilter(4096, 0.01); for (let i = 0; i < 100; i++) b.add("k" + i); return b; }, (c) => new c(8, 0.5)],
        ["Trie", Trie, () => { const t = new Trie(); for (let i = 0; i < 100; i++) t.insert("word" + i); return t; }, (c) => new c()],
        ["LRU", LRU, () => { const l = new LRU(64); for (let i = 0; i < 64; i++) l.set("k" + i, i); return l; }, (c) => new c(1)],
        ["SortedSet", SortedSet, () => { const s = new SortedSet(); for (let i = 0; i < 200; i++) s.add(i); return s; }, (c) => new c()],
        ["SortedMap", SortedMap, () => { const s = new SortedMap(); for (let i = 0; i < 200; i++) s.set(i, i); return s; }, (c) => new c()],
        ["Heap", Heap, () => { const h = new Heap(); for (let i = 0; i < 200; i++) h.push(i); return h; }, (c) => new c()],
        ["List", List, () => { const l = new List(); for (let i = 0; i < 200; i++) l.pushBack(i); return l; }, (c) => new c()],
        ["Deque", Deque, () => { const d = new Deque(); for (let i = 0; i < 200; i++) d.pushBack(i); return d; }, (c) => new c()],
        ["RingBuffer", RingBuffer, () => { const r = new RingBuffer(64); for (let i = 0; i < 64; i++) r.push(i); return r; }, (c) => new c(1)],
    ];
    for (const [name, C, build, small] of cases) {
        let bytes;
        try { bytes = build().serialize(); } catch (e) { ok(false, name + ": cannot build the fixture (" + e.message + ")"); continue; }
        const honest = C.deserialize(bytes);
        const saved = C.prototype.constructor;
        let hijacked = 0, viaObj, viaSmall;
        try {
            C.prototype.constructor = function () { hijacked++; return {}; };
            viaObj = C.deserialize(bytes);
            C.prototype.constructor = function () { hijacked++; return small(saved); };
            viaSmall = C.deserialize(bytes);
        } catch (e) {
            ok(false, name + ".deserialize threw under a replaced constructor: " + e.message);
        } finally {
            C.prototype.constructor = saved;
        }
        eq(hijacked, 0, name + ".deserialize never calls prototype.constructor");
        ok(viaObj instanceof C && viaSmall instanceof C, name + ".deserialize returns a genuine instance");
        let same = false;
        try {
            const a = honest.serialize(), b = viaSmall.serialize();
            same = a.length === b.length && a.every((v, i) => v === b[i]);
        } catch (e) { }
        ok(same, name + ".deserialize result is unaffected by the replaced constructor");
    }
}

// ---- N2-A / N2-B / N2-C --------------------------------------------------------
{
    const sp = new URLSearchParams("a=1&b=2&c=3");
    let fired = 0;
    Object.defineProperty(Array.prototype, "0", { configurable: true, set(v) { fired++; sp.delete("a"); sp.delete("b"); sp.delete("c"); } });
    let out;
    try {
        const it = sp.entries();
        out = it.next();
    } finally {
        delete Array.prototype[0];
    }
    eq(fired, 0, "URLSearchParams iterator result never reaches an Array.prototype setter");
    eq(out.value.join("="), "a=1", "URLSearchParams iterator yields own-property pairs");
    let vfired = 0;
    Object.defineProperty(Object.prototype, "value", { configurable: true, set(v) { vfired++; } });
    let r2;
    try { r2 = sp.keys().next(); } finally { delete Object.prototype.value; }
    eq(vfired, 0, "iterator result object is built with own properties");
    eq(r2.value, "a", "iterator result is intact");

    const cost = (n) => {
        const u = new URL("http://h/");
        const p = "a/".repeat(n) + "../".repeat(n);
        const t0 = performance.now();
        u.pathname = p;
        return [performance.now() - t0, u.pathname];
    };
    cost(2000);
    const [c1] = cost(20000), [c8, path] = cost(160000);
    eq(path, "/", "dot segments cancel every pushed segment");
    ok(c8 < Math.max(c1, 0.2) * 40, "pathname setter is linear in its input (20k " + c1.toFixed(2) + "ms, 160k " + c8.toFixed(2) + "ms)");
    eq(new URL("http://h/a/b/../c/./d/..").pathname, "/a/c/", "dot-segment semantics (control)");
    eq(new URL("http://h/a/%2e%2e/b").pathname, "/b", "encoded dot segments (control)");
    eq(new URL("file:///C:/a/../../b").pathname, "/C:/b", "file drive letter is not popped (control)");

    const host = (n) => { let s = ""; for (let i = 0; i < n; i++) s += String.fromCodePoint(0x4E00 + (i % 20000)); return s; };
    eq(URL.canParse("http://" + host(4096 - 4 - 1) + ".com/"), true, "IDNA host one under the 4096 code point bound parses");
    eq(URL.canParse("http://" + host(4096 - 4) + ".com/"), true, "IDNA host of exactly 4096 mapped code points parses");
    eq(URL.canParse("http://" + host(4096 - 4 + 1) + ".com/"), false, "IDNA host of 4097 mapped code points is refused");
    eq(URL.canParse("http://" + host(20000) + ".com/"), false, "a far larger IDNA host is refused");
    eq(new URL("http://b\u00fccher.example/").hostname, "xn--bcher-kva.example", "IDNA mapping (control)");
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

// ---- ml --------------------------------------------------------------------------
{
    const X = [], y = [];
    for (let i = 0; i < 40; i++) { X.push([i, i * 2 + 1, (i * 7) % 5]); y.push(3 * i + 1); }
    const m = new ml.LinearRegression().fit(X, y);
    let fired = 0;
    Object.defineProperty(Array.prototype, "1", { configurable: true, set(v) { fired++; m.close(); } });
    let coef;
    try { coef = m.coef; } finally { delete Array.prototype[1]; }
    eq(fired, 0, "fitted-attribute getters never reach an Array.prototype setter (M1b-03)");
    eq(coef.length, 3, "coef is complete");
    m.close();

    let polluted = 0;
    Object.defineProperty(Object.prototype, "train", { configurable: true, get() { polluted++; return [100000, 100001, 100002]; }, set(v) { } });
    let res = "?";
    try {
        const XX = [], yy = [];
        for (let i = 0; i < 10; i++) { XX.push([i, i + 1]); yy.push(i % 2); }
        try { ml.crossValScore(() => new ml.GaussianNB(), XX, yy, { k: 2 }); res = "ok"; } catch (e) { res = e.name; }
    } finally {
        delete Object.prototype.train;
    }
    eq(polluted, 0, "fold tables are own properties; an Object.prototype accessor is never consulted (M1b-05)");
    ok(res === "ok" || res === "TypeError" || res === "RangeError", "crossValScore completed or refused cleanly (" + res + ")");

    const shared = new Float64Array(new SharedArrayBuffer(8 * 6));
    shared.set([1, 2, 3, 4, 5, 6]);
    eq(errName(() => new ml.StandardScaler().fit(shared, 3, 2)), "TypeError", "fit refuses a Float64Array over a SharedArrayBuffer (M1b-06)");
    eq(errName(() => ml.CSR.fromDense(shared, 3, 2)), "TypeError", "CSR.fromDense refuses a shared Float64Array");
    eq(errName(() => new ml.StandardScaler().fit(new Float64Array(shared), 3, 2)), "none", "a private copy is accepted (control)");

    const pts = [];
    for (let i = 0; i < 60; i++) pts.push([Math.sin(i) * 3 + (i % 3) * 10, Math.cos(i * 1.7) * 3 + (i % 3) * 7]);
    const a7 = new ml.KMeans(3, 7).fit(pts).inertia;
    const o7 = new ml.KMeans(3, { seed: 7 }).fit(pts).inertia;
    eq(o7, a7, "KMeans(k, { seed }) honours the seed it is given (M1b-08)");
    eq(errName(() => new ml.KMeans(3, "7")), "TypeError", "KMeans refuses a non-number seed");
    eq(errName(() => new ml.KMeans(3, { seed: "x" })), "TypeError", "KMeans refuses a non-number { seed }");

    const Xs = [], ys = [];
    for (let i = 0; i < 20; i++) { Xs.push([i, (i * 3) % 7]); ys.push(i < 10 ? 0 : 1); }
    const svc = new ml.SVC().fit(Xs, ys);
    const flat = new Float64Array(40);
    Xs.forEach((r, i) => { flat[2 * i] = r[0]; flat[2 * i + 1] = r[1]; });
    ok(svc.decisionFunction(flat, 20, 2) instanceof Float64Array, "binary SVC.decisionFunction(flat X) is a Float64Array (M1b-09)");
    ok(Array.isArray(svc.decisionFunction(Xs)), "binary SVC.decisionFunction(rows) stays an array (control)");
}

// ---- M1c-23 ------------------------------------------------------------------------
{
    const ref = (a, b) => {
        let prev = new Array(b.length + 1);
        for (let j = 0; j <= b.length; j++) prev[j] = j;
        for (let i = 1; i <= a.length; i++) {
            const cur = [i];
            for (let j = 1; j <= b.length; j++)
                cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
            prev = cur;
        }
        return prev[b.length];
    };
    let bad = 0, n = 0;
    for (const m of [63, 64, 65, 66, 97]) {
        for (const k of [m, m + 1, 2 * m - 1, 2 * m, 2 * m + 1, 2 * m + 3, 3 * m, 5 * m]) {
            const variants = [
                ["x".repeat(m), "y".repeat(k)],
                ["ab".repeat(m).slice(0, m), "ba".repeat(k).slice(0, k)],
                ["x".repeat(m), "x".repeat(k)],
                ["\u0416".repeat(m), "\u0416\u0429".repeat(k).slice(0, k)],
            ];
            for (const [a, b] of variants) {
                n++;
                const w1 = ref(a, b);
                if (Levenshtein(a, b) !== w1 || Levenshtein(b, a) !== w1) {
                    if (bad++ < 4) print("  FAIL: Levenshtein len " + a.length + "/" + b.length + " got " + Levenshtein(a, b) + " want " + w1);
                }
            }
        }
    }
    checks++;
    if (bad) failures++;
    print("  Levenshtein differential: " + n + " pairs, " + bad + " mismatches");
    eq(Levenshtein("x".repeat(70), "y".repeat(200), { max: 10 }), 11, "Levenshtein { max } early-out (control)");
}

// ---- M1c-24 / budgets: child processes -----------------------------------------------
{
    const run = (args, src) => Exec(BIN, args.concat(["-e", src]), { timeoutMs: 60000, encoding: "utf8" });
    const dec = run(["--module", "--native-memory-limit", "8000000"],
        'import { Decimal } from "dyna:decimal"; const keep=[]; const big="7".repeat(50000);' +
        'try { for(;;) keep.push(new Decimal(big)); } catch(e) {}' +
        'try { for(;;) keep.push(new Decimal("1")); } catch(e) {}' +
        'const one = keep[keep.length-1]; let out=[];' +
        'for (const op of ["divmod","add","mul","sub","div"]) { try { one[op](3); out.push("ok"); } catch(e) { out.push(e.name); } }' +
        'print(out.join(","));');
    ok(dec.signal === null && dec.code === 0, "Decimal at the native limit throws instead of crashing (exit " + dec.code + ", signal " + dec.signal + ") (M1c-24)");
    ok(/^(ok|InternalError|RangeError)(,(ok|InternalError|RangeError)){4}/.test((dec.stdout || "").trim()), "Decimal ops report a catchable error at the limit (" + (dec.stdout || "").trim() + ")");

    const t0 = performance.now();
    const fit = run(["--module", "--timeout-ms", "400"],
        // A synchronous Argon2id of 256 MiB x 16 passes: several seconds in
        // one native call that never looks at the deadline. (This used to be a
        // LogisticRegression fit; fits poll the deadline since wave 3 and end
        // with "interrupted", which tests/test_audit_w3_native_4.js pins.)
        'import { Argon2id } from "dyna:crypto";' +
        'Argon2id.hash("pw", new Uint8Array(16), { memory: 262144, iterations: 16, encoded: false }); print("fit returned");');
    const ms = performance.now() - t0;
    ok(fit.code !== 0 && !fit.timedOut, "a native loop that never polls is still stopped by --timeout-ms (exit " + fit.code + ") (P1-08)");
    ok(ms < 8000, "the hard stop lands near the deadline (" + (ms | 0) + "ms for a 400ms budget)");
    ok(!/fit returned/.test(fit.stdout || ""), "the fit did not run to completion");
    eq(fit.code, 113, "the hard stop has its own exit code (113)");

    const quick = run(["--timeout-ms", "20000"], 'print(1+1)');
    ok(quick.code === 0 && (quick.stdout || "").trim() === "2", "a script inside its budget is unaffected by the watchdog (control)");

    const tla = run(["--module"], 'await new Promise(() => {}); print("unreachable");');
    ok(tla.code !== 0 && !tla.timedOut && /never settle/.test(tla.stderr || ""), "an await that can never settle is an error, not a spin (C1-13)");
}

if (failures) {
    print("test_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_audit_w2_native_1 failed");
}
print("test_audit_w2_native_1: " + checks + " checks passed");
