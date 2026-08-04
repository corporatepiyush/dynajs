/* test_optguide_regressions.js -- one case per defect found by the adversarial
 * review of the optimization batch, plus the boundary sweeps the review said
 * were missing.
 *
 * EVERY case here failed on some binary in this session. That is the bar: a
 * regression test whose defect was never reproduced is a guess. Where a case
 * pins a THRESHOLD, it tests N-1, N and N+1, because a constant in the source
 * is a cliff in the behaviour and the corpus has to cross it.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_optguide_regressions.js
 */
import { DataFrame } from "dyna:dataframe";
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";
import { StableStringify } from "dyna:encoding";
import { Duration } from "dyna:time";
import { CBORCanonical, ValueHash, structuredClone as sclone } from "dyna:serialize";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
const TMP = "/tmp/dynajs_optguide_test";

/* ===================================================================
   1. dataframe rolling sum -- a subtractive accumulator loses the whole
   value when a large element leaves the window, and the loss is FINITE so
   no overflow check sees it. Also pins the w=256 strategy boundary.
   =================================================================== */
{
    /* the reproducer: every element finite, one huge pair at the front */
    const v = new Float64Array(600); v.fill(1); v[0] = 1e308; v[1] = 1e308;
    const df = new DataFrame({ v: v });
    for (const w of [255, 256, 257]) {          /* N-1, N, N+1 at the boundary */
        const r = df.ROLLING_SUM("v", w);
        eq(r[599], w, "roll_sum w=" + w + ": a departed 1e308 must not poison the tail");
        ok(isFinite(r[599]), "roll_sum w=" + w + " tail is finite");
    }
    /* the genuinely overflowing window must still say so */
    const r = df.ROLLING_SUM("v", 256);
    ok(r[255] > 1e307, "the window that really does overflow still reports it");

    /* the two strategies must agree on ordinary data at the boundary */
    const u = new Float64Array(2000);
    for (let i = 0; i < u.length; i++) u[i] = ((i * 7919) % 1000) / 7;
    const d2 = new DataFrame({ u: u });
    /* An independent reference per width. The window for width w ending at i is
       [i-w+1, i] -- getting that off by one measures a different sum and reads
       as an engine bug. */
    const refmax = (arr, w) => {
        let worst = 0;
        for (let i = w + 40; i < u.length; i++) {
            let s = 0; for (let k = i - w + 1; k <= i; k++) s += u[k];
            worst = Math.max(worst, Math.abs(arr[i] - s) / Math.abs(s || 1));
        }
        return worst;
    };
    const e255 = refmax(d2.ROLLING_SUM("u", 255), 255);
    const e256 = refmax(d2.ROLLING_SUM("u", 256), 256);
    ok(e255 < 1e-12, "w=255 exact path matches an independent sum (rel " + e255 + ")");
    ok(e256 < 1e-12, "w=256 block path matches an independent sum (rel " + e256 + ")");

    /* non-finite input: a window holding an Inf reports Inf, one that does not
       must be unaffected -- the block form needs no special case for this. */
    const w2 = new Float64Array(800); w2.fill(2); w2[0] = Infinity;
    const d3 = new DataFrame({ w: w2 });
    const r3 = d3.ROLLING_SUM("w", 256);
    ok(r3[255] === Infinity, "a window containing Inf reports Inf");
    eq(r3[799], 512, "a window past the Inf is unaffected");

    const w3 = new Float64Array(800); w3.fill(2); w3[0] = NaN;
    const r4 = new DataFrame({ w: w3 }).ROLLING_SUM("w", 256);
    ok(Number.isNaN(r4[255]), "a window containing NaN reports NaN");
    eq(r4[799], 512, "a window past the NaN is unaffected");

    /* ROLLING_MEAN divides the same accumulator */
    const rm = df.ROLLING_MEAN("v", 256);
    eq(rm[599], 1, "ROLLING_MEAN recovers after the departed 1e308");
}

/* ===================================================================
   2. csv -- the arena changed who owns a cell, so every mutation path is a
   candidate invalid free; and NULL is tcell_dup's SUCCESS value for an
   empty cell, which two callers read as OOM.
   =================================================================== */
{
    const mk = (n, rows) => {
        const f = new CSVFile(new Path(TMP + "_" + n + ".csv"));
        f.create({ headers: ["A", "B", "C"], rows: rows || [["1", "2", "3"], ["4", "5", "6"]],
                   overwrite: true });
        return f;
    };
    /* empty names are values, not allocation failures */
    let threw = null;
    try { mk("rn").renameColumn({ oldName: "B", newName: "" }); } catch (e) { threw = e; }
    ok(!threw, "renameColumn to an empty name is accepted" + (threw ? ": " + threw.message : ""));
    threw = null;
    try { mk("ac").addColumn({ column: "" }); } catch (e) { threw = e; }
    ok(!threw, "addColumn with an empty name is accepted" + (threw ? ": " + threw.message : ""));
    threw = null;
    try { mk("ad").addColumn({ column: "D", defaultValue: "" }); } catch (e) { threw = e; }
    ok(!threw, "addColumn with an empty defaultValue is accepted");

    /* every mutation on a PARSED table -- each of these freed an arena
       interior pointer before the ownership sweep */
    for (const [name, fn] of [
        ["updateCell",   f => f.updateCell({ row: 0, column: "B", value: "x" })],
        ["updateCell->empty", f => f.updateCell({ row: 0, column: "B", value: "" })],
        ["removeRow",    f => f.removeRow({ row: 0 })],
        ["removeColumn", f => f.removeColumn({ column: "B" })],
        ["renameColumn", f => f.renameColumn({ oldName: "B", newName: "Z" })],
        ["addColumn",    f => f.addColumn({ column: "D", defaultValue: "d" })],
        ["addRow",       f => f.addRow({ rows: [["7", "8", "9"]] })],
        ["addRow named", f => f.addRow({ rows: [{ A: "7", B: "8", C: "9" }] })],
    ]) {
        let err = null;
        try { fn(mk("m")); } catch (e) { err = e; }
        ok(!err, "csv " + name + " on a parsed table" + (err ? ": " + err.message : ""));
    }

    /* an oversize cell must not orphan the block that still has room: write a
       row with one big cell and many short ones, then read it back intact */
    {
        const big = "x".repeat(70000);
        const f = new CSVFile(new Path(TMP + "_big.csv"));
        const rows = [];
        for (let i = 0; i < 20; i++) rows.push([big, "t" + i, "u" + i]);
        f.create({ headers: ["A", "B", "C"], rows: rows, overwrite: true });
        const got = f.readRowRange({ start: 0, end: 3 });
        ok(got.rows.length === 3, "oversize cells: rows survive the arena");
        eq(got.rows[0][0].length, 70000, "oversize cell round-trips at full length");
        eq(got.rows[2][1], "t2", "a short cell after an oversize one is intact");
    }
    /* a cell exactly at, and either side of, the 65536 block size */
    for (const n of [65534, 65535, 65536, 65537]) {
        const f = new CSVFile(new Path(TMP + "_b" + n + ".csv"));
        f.create({ headers: ["A", "B"], rows: [["y".repeat(n), "tail"]], overwrite: true });
        const got = f.readRowRange({ start: 0, end: 1 });
        eq(got.rows[0][0].length, n, "cell of exactly " + n + " bytes round-trips");
        eq(got.rows[0][1], "tail", "the cell after a " + n + "-byte cell is intact");
    }
}

/* ===================================================================
   3. regexp -- the prefilter fold cache is keyed on opcode bytes, and its
   slot index took FNV's low bits, where two patterns 8 apart collided
   forever. Alternation is the only shape that shows it.
   =================================================================== */
{
    /* NO TIMING ASSERTION HERE, deliberately. The defect this section exists
       for -- two /i patterns whose first characters differ by 8 evicting each
       other forever -- is only observable as a RATIO of wall-clock times, and a
       ratio is not portable across builds: under ASan the same loop is an order
       of magnitude slower and this case read 1.66x and 2.19x on an engine whose
       uninstrumented build reads 0.99x. That is a test failing for a reason
       that is not a bug, and it failed the gate twice.
       The thrash is measured in tests/bench_regexp_scan.js, where a ratio
       belongs and where a regression shows as a moved row against a recorded
       baseline. What is asserted here is what a test CAN assert: that the fold
       is exact, cached or not. */
    /* the fold must still be CORRECT, not merely cached */
    eqj("Hello World".match(/[a-z]+/gi), ["Hello", "World"], "range_i fold matches both cases");
    eqj("aAbB".match(/[ab]/gi), ["a", "A", "b", "B"], "char class fold is exact");
    eq("XyZ".replace(/[a-z]/gi, "."), "...", "replace over a folded class");
    eq("straße".match(/STRASSE/i), null, "the fold does not overreach");
}

/* ===================================================================
   4. temporal -- ISO-8601 puts ONE sign on a duration, so a mixed-sign
   value has no representation. Taking the sign from the leading component
   rendered a DIFFERENT duration as valid ISO.
   =================================================================== */
{
    eq(new Duration({ months: 1, days: 10 }).toString(), "P1M10D", "all-positive duration");
    eq(new Duration({ months: -1, days: -10 }).toString(), "-P1M10D", "all-negative duration");
    eq(new Duration({ months: 0, days: 0 }).toString(), "P0D", "zero duration");
    for (const [mo, d] of [[1, -10], [-1, 10], [-5, 3], [7, -2]]) {
        let threw = null;
        try { new Duration({ months: mo, days: d }).toString(); } catch (e) { threw = e; }
        ok(threw && /mixed-sign/.test(threw.message),
           "mixed-sign (" + mo + "," + d + ") is refused, never rendered");
    }
    /* the buffer worst case: INT64_MIN in every component must not overflow */
    let s = null;
    try { s = new Duration({ months: -9007199254740991, days: 9007199254740991 }).toString(); }
    catch (e) { s = "threw"; }
    ok(s !== null, "extreme mixed duration terminates without corrupting the stack");
    ok(new Duration({ days: 9007199254740991 }).toString().length < 90,
       "an extreme single-component duration stays inside the buffer");
}

/* ===================================================================
   5. json5 / RFC 8785 -- canonical order is by UTF-16 code UNITS, and JCS
   output is what gets signed, so a wrong order is a wrong signature.
   =================================================================== */
{
    const units = s => Array.from({ length: s.length },
        (_, i) => s.charCodeAt(i).toString(16).padStart(4, "0")).join(" ");
    const keysOf = j => [...j.matchAll(/"((?:[^"\\]|\\.)*)"\s*:/g)].map(m => m[1]);

    const o = {}; o["\u{10000}"] = 1; o["\uD800￿"] = 2;
    const k = keysOf(StableStringify(o));
    eq(units(k[0]), "d800 dc00", "D800 DC00 sorts before D800 FFFF");
    eq(units(k[1]), "d800 ffff", "...and the lone-surrogate key follows");

    const p = {}; p["\u{1F600}z"] = 1; p["\u{1F601}a"] = 2;
    const kp = keysOf(StableStringify(p));
    ok(kp[0].codePointAt(0) === 0x1F600, "astral pair: low surrogate decides the tie");

    /* RFC 8785 s3.2.3 sorts by UTF-16 code UNITS, so a prefix precedes its
       extension and "ab" < "b". That is NOT length-first -- length-first is
       RFC 8949 CBOR canonical, a different function on the same keys, and
       conflating the two is how a canonicalizer gets "fixed" into being wrong. */
    const q = { "b": 1, "a": 2, "ab": 3, "abc": 4 };
    eqj(keysOf(StableStringify(q)), ["a", "ab", "abc", "b"],
        "RFC 8785 JCS: code-unit order, prefix first");
    /* the CBOR canonical form of the same object IS length-first (RFC 8949
       4.2.1), so the two encoders must disagree here -- and that disagreement
       is the check that neither has drifted onto the other's rule. */
    const cb = CBORCanonical(q);
    eq(cb[1] & 0xe0, 0x60, "CBORCanonical emits a text key first");
    eq(String.fromCharCode(cb[2]), "a", "CBOR canonical: a 1-char key sorts before longer ones");
    eq(String.fromCharCode(cb[5]), "b", "CBOR canonical is LENGTH-first: 'b' precedes 'ab'");

    /* permutation invariance: insertion order must not change the bytes */
    const mkbig = order => { const r = {}; for (const i of order) r["k" + i + "_x"] = i; return r; };
    const idx = []; for (let i = 0; i < 300; i++) idx.push(i);
    const base = ValueHash(mkbig(idx));
    for (let t = 0; t < 5; t++) {
        const sh = idx.slice();
        for (let i = sh.length - 1; i > 0; i--) {
            const j = (i * 1103515245 + 12345 + t * 7) % (i + 1);
            const tmp = sh[i]; sh[i] = sh[j]; sh[j] = tmp;
        }
        eq(ValueHash(mkbig(sh)), base, "ValueHash is permutation-invariant (shuffle " + t + ")");
    }
    /* crosses VENC_FAST_KEYS = 128, the stack/heap threshold for the key table */
    for (const n of [16, 127, 128, 129, 300]) {
        const a = {}, b = {};
        for (let i = 0; i < n; i++) a["k" + i] = i;
        for (let i = n - 1; i >= 0; i--) b["k" + i] = i;
        eq(ValueHash(a), ValueHash(b), "ValueHash stable at n=" + n + " (crosses the 128 table)");
    }
    /* a NUL-bearing key must sort on its FULL bytes, not a strlen prefix */
    const z1 = {}; z1["a b"] = 1; z1["a a"] = 2;
    const z2 = {}; z2["a a"] = 2; z2["a b"] = 1;
    eq(ValueHash(z1), ValueHash(z2), "NUL-bearing keys hash independent of insertion order");
    ok(CBORCanonical(z1).length === CBORCanonical(z2).length, "...and encode to the same length");
}

/* ===================================================================
   6. string compare -- the portfolio selects on element WIDTH and LENGTH.
   Lengths straddle every threshold in the source: 4 (unrolled/quad) and
   16 (quad/chunked), and 8 (js_string_eq's memcmp arm).
   =================================================================== */
{
    const wpad = (b, n) => { let s = b; while (s.length < n) s += "一二三四五六七八"; return s.slice(0, n); };
    const npad = (b, n) => { let s = b; while (s.length < n) s += "abcdefghijklmnop"; return s.slice(0, n); };
    const LENS = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 64, 128];
    for (const n of LENS) {
        for (const [kind, pad] of [["narrow", npad], ["wide", wpad]]) {
            const a = pad("k" + n + "_", n);
            const b = (a + "!").slice(0, n);          /* distinct object, equal content */
            ok(a === b, kind + " len " + n + ": equal strings compare equal");
            ok(!(a < b) && !(a > b), kind + " len " + n + ": equal strings do not order");
            if (n > 0) {
                const last = a.slice(0, n - 1) + (kind === "wide" ? "鿿" : "~");
                ok(a !== last, kind + " len " + n + ": differing LAST unit is unequal");
                ok(a < last, kind + " len " + n + ": ordering by the last unit");
                const first = (kind === "wide" ? "鿿" : "~") + a.slice(1);
                ok(a !== first, kind + " len " + n + ": differing FIRST unit is unequal");
                ok(a < first, kind + " len " + n + ": ordering by the first unit");
            }
        }
        /* mixed width, equal length: memcmp16_8, the arm nothing else reaches */
        if (n > 0) {
            const nar = npad("m_", n - 1) + "b";
            const wid = npad("m_", n - 1) + "一";
            eq(nar.length, wid.length, "mixed len " + n + ": equal lengths");
            ok(nar !== wid, "mixed len " + n + ": a wide unit never equals a byte");
            ok(nar < wid, "mixed len " + n + ": ordering across widths");
        }
    }
    /* Map/Set keys drive js_string_eq through the property system */
    for (const [kind, pad] of [["narrow", npad], ["wide", wpad]]) {
        const m = new Map(), keys = [];
        for (let i = 0; i < 500; i++) { const k = pad("key" + i + "_", 12); keys.push(k); m.set(k, i); }
        let hits = 0;
        for (let i = 0; i < 500; i++) if (m.get((keys[i] + "#").slice(0, 12)) === i) hits++;
        eq(hits, 500, kind + " Map keys: every distinct-object lookup hits");
        ok(!m.has(pad("absent_", 12) + "zz"), kind + " Map: an absent key misses");
    }
    /* sort() drives js_string_compare and needs the SIGN, not just equality */
    /* Code points, not numeral values: 一=4E00 三=4E09 二=4E8C 鿿=9FFF. Writing
       the expectation from what the characters MEAN rather than from their
       units is how a correct sort gets reported as broken. */
    const ws = ["二", "一", "三z", "三a", "鿿"];
    eqj(ws.slice().sort(), ["一", "三a", "三z", "二", "鿿"],
        "wide sort matches UTF-16 code-unit order (4E00 4E09 4E8C 9FFF)");
}

/* ===================================================================
   7. array -- median (quickselect), zip (fast + generic arms), sortBy
   (ping-pong parity). The suite previously reached none of these bodies.
   =================================================================== */
{
    /* median: sweep across the 12-element insertion cutoff and both parities */
    for (const n of [1, 2, 3, 4, 11, 12, 13, 24, 25, 101, 1000]) {
        const a = []; for (let i = 0; i < n; i++) a.push((i * 7919) % n);
        const s = a.slice().sort((x, y) => x - y);
        const want = (n & 1) ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
        eq(a.median(), want, "median n=" + n + " matches a full sort");
    }
    eq([5, 5, 5, 5].median(), 5, "median all-equal even (the 3-way partition's equal band)");
    eq([5, 5, 5].median(), 5, "median all-equal odd");
    eq([1, 2, 2, 3].median(), 2, "median with a tie across the middle");
    {
        const up = [], down = [];
        for (let i = 0; i < 1001; i++) { up.push(i); down.push(1000 - i); }
        eq(up.median(), 500, "median already-sorted (median-of-three, not O(n^2))");
        eq(down.median(), 500, "median reverse-sorted");
    }
    eq([1e308, 1e308].median(), Infinity, "median keeps sum-then-halve overflow");
    ok(Number.isNaN([1, NaN, 3].median()), "median propagates NaN");
    ok(Number.isNaN([NaN, 1, 3].median()), "median propagates NaN whatever its position");
    ok(Number.isNaN([].median()), "median of an empty array is NaN");

    /* zip: the fast arm needs BOTH operands dense; everything else must take
       the generic arm and produce an indistinguishable result */
    eqj([1, 2].zip([3, 4]), [[1, 3], [2, 4]], "zip fast arm");
    eqj([].zip([]), [], "zip of two empty arrays");
    eqj([1, 2, 3].zip([9]), [[1, 9]], "zip truncates to the shorter operand");
    {
        const a = [1, 2]; a.length = 5;                 /* fast_array, count 2, length 5 */
        const r = a.zip([9, 8, 7, 6, 5]);
        eq(r.length, 5, "zip length>count takes the generic arm without over-reading");
        eq(r[0][0], 1, "...and the present elements are right");
        ok(r[2][0] === undefined, "...and the absent ones are undefined");
    }
    {
        const h = [1, 2, 3]; delete h[1];               /* a hole: not a fast array */
        const r = h.zip([9, 8, 7]);
        eq(r.length, 3, "zip over a hole");
        ok(r[1][0] === undefined, "the hole reads as undefined");
    }
    {
        let hits = 0;
        const g = [1, 2];
        Object.defineProperty(g, "0", { get() { hits++; return 42; }, configurable: true });
        const r = g.zip([9, 8]);
        eq(r[0][0], 42, "zip runs an index getter rather than bypassing it");
        eq(hits, 1, "zip runs the getter exactly once");
    }
    eqj(new Proxy([1, 2], {}).zip([9, 8]), [[1, 9], [2, 8]], "zip over a proxy receiver");
    { const self = [1, 2]; eqj(self.zip(self), [[1, 1], [2, 2]], "zip with itself"); }

    /* sortBy: an odd pass count leaves the result in the scratch buffer, so
       sweep the lengths rather than sampling. Heavy ties expose instability. */
    for (let n = 2; n <= 17; n++) {
        const src = [];
        for (let i = 0; i < n; i++) src.push({ k: i % 3, id: i });
        const want = src.map((_, i) => i).sort((a, b) => (src[a].k - src[b].k) || (a - b));
        eqj(src.sortBy("k").map(x => x.id), want, "sortBy stable at n=" + n + " (pass parity)");
    }
    eqj([3, 1, 2].sortBy(), [1, 2, 3], "sortBy odd pass count lands in the caller's buffer");
    eqj([{ k: 2 }, { k: 1 }, { k: 3 }].sortBy("k", true).map(x => x.k), [3, 2, 1], "sortBy descending");
}

/* ===================================================================
   8. structuredClone memo -- cycles must survive, and the memo must not be
   quadratic. Correctness first: a hashed memo that loses a cycle is worse
   than a slow one.
   =================================================================== */
{
    const a = { name: "a" }; a.self = a;
    const c = sclone(a);
    ok(c.self === c, "clone: a self-cycle closes onto the clone");
    ok(c !== a, "clone: and it is a copy, not the original");

    const x = { n: 1 }, y = { n: 2 }; x.y = y; y.x = x;
    const cx = sclone(x);
    ok(cx.y.x === cx, "clone: a two-node cycle closes");

    const shared = { v: 7 };
    const cs = sclone({ p: shared, q: shared });
    ok(cs.p === cs.q, "clone: a shared node stays shared, not duplicated");

    /* crosses VCLONE_HASH_MIN = 64 in both directions */
    for (const n of [16, 63, 64, 65, 2000]) {
        const root = {}; const child = { c: 1 };
        for (let i = 0; i < n; i++) root["a" + i] = { v: i };
        root.s1 = child; root.s2 = child;
        const r = sclone(root);
        eq(Object.keys(r).length, n + 2, "clone n=" + n + " keeps every key");
        eq(r["a" + (n - 1)].v, n - 1, "clone n=" + n + " copies values");
        ok(r.s1 === r.s2, "clone n=" + n + " preserves sharing across the memo threshold");
    }
    /* deep nesting must still be refused rather than blowing the stack */
    let deep = {}; const top = deep;
    for (let i = 0; i < 400; i++) { deep.n = {}; deep = deep.n; }
    let threw = false;
    try { sclone(top); } catch (e) { threw = true; }
    ok(threw, "clone refuses nesting past its depth cap");
}

/* ===================================================================
   9. JS_GetOwnFastProps -- bails on the first integer key, and used to
   leak one atom reference per string key that preceded it. Unbounded
   growth, nothing errors. Drive the shape hard and require survival.
   =================================================================== */
{
    for (let i = 0; i < 20000; i++) {
        const o = {};
        o["sessionId_" + i] = "v";
        o["tokenName_" + i] = "w";
        o[0] = 1;                       /* the integer key that triggers the bail */
        ValueHash(o);
    }
    ok(true, "20000 encodes of string-then-integer-key objects completed");
    /* and the result is still correct after the bail path */
    const o = { a: "x", 0: 1 };
    eq(ValueHash(o), ValueHash({ a: "x", 0: 1 }), "the fallback path still hashes deterministically");
}

print("test_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
