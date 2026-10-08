import { DataFrame } from "dyna:dataframe";
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";
import { StableStringify } from "dyna:encoding";
import { Duration } from "dyna:time";
import { CBORCanonical, ValueHash, structuredClone as sclone } from "dyna:serialize";
import { Decimal } from "dyna:decimal";
import { Table } from "dyna:structures";
import * as simd from "dyna:simd";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
const TMP = "/tmp/dynajs_optguide_test";

{
    const v = new Float64Array(600); v.fill(1); v[0] = 1e308; v[1] = 1e308;
    const df = new DataFrame({ v: v });
    for (const w of [255, 256, 257]) {
        const r = df.ROLLING_SUM("v", w);
        eq(r[599], w, "roll_sum w=" + w + ": a departed 1e308 must not poison the tail");
        ok(isFinite(r[599]), "roll_sum w=" + w + " tail is finite");
    }
    const r = df.ROLLING_SUM("v", 256);
    ok(r[255] > 1e307, "the window that really does overflow still reports it");

    const u = new Float64Array(2000);
    for (let i = 0; i < u.length; i++) u[i] = ((i * 7919) % 1000) / 7;
    const d2 = new DataFrame({ u: u });
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

    const w2 = new Float64Array(800); w2.fill(2); w2[0] = Infinity;
    const d3 = new DataFrame({ w: w2 });
    const r3 = d3.ROLLING_SUM("w", 256);
    ok(r3[255] === Infinity, "a window containing Inf reports Inf");
    eq(r3[799], 512, "a window past the Inf is unaffected");

    const w3 = new Float64Array(800); w3.fill(2); w3[0] = NaN;
    const r4 = new DataFrame({ w: w3 }).ROLLING_SUM("w", 256);
    ok(Number.isNaN(r4[255]), "a window containing NaN reports NaN");
    eq(r4[799], 512, "a window past the NaN is unaffected");

    const rm = df.ROLLING_MEAN("v", 256);
    eq(rm[599], 1, "ROLLING_MEAN recovers after the departed 1e308");
}

{
    const mk = (n, rows) => {
        const f = new CSVFile(new Path(TMP + "_" + n + ".csv"));
        f.create({ headers: ["A", "B", "C"], rows: rows || [["1", "2", "3"], ["4", "5", "6"]],
                   overwrite: true });
        return f;
    };
    let threw = null;
    try { mk("rn").renameColumn({ oldName: "B", newName: "" }); } catch (e) { threw = e; }
    ok(!threw, "renameColumn to an empty name is accepted" + (threw ? ": " + threw.message : ""));
    threw = null;
    try { mk("ac").addColumn({ column: "" }); } catch (e) { threw = e; }
    ok(!threw, "addColumn with an empty name is accepted" + (threw ? ": " + threw.message : ""));
    threw = null;
    try { mk("ad").addColumn({ column: "D", defaultValue: "" }); } catch (e) { threw = e; }
    ok(!threw, "addColumn with an empty defaultValue is accepted");

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
    for (const n of [65534, 65535, 65536, 65537]) {
        const f = new CSVFile(new Path(TMP + "_b" + n + ".csv"));
        f.create({ headers: ["A", "B"], rows: [["y".repeat(n), "tail"]], overwrite: true });
        const got = f.readRowRange({ start: 0, end: 1 });
        eq(got.rows[0][0].length, n, "cell of exactly " + n + " bytes round-trips");
        eq(got.rows[0][1], "tail", "the cell after a " + n + "-byte cell is intact");
    }
}

{
    eqj("Hello World".match(/[a-z]+/gi), ["Hello", "World"], "range_i fold matches both cases");
    eqj("aAbB".match(/[ab]/gi), ["a", "A", "b", "B"], "char class fold is exact");
    eq("XyZ".replace(/[a-z]/gi, "."), "...", "replace over a folded class");
    eq("straße".match(/STRASSE/i), null, "the fold does not overreach");
}

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
    // SEC-246: Duration components are bounded at |value| <= 1e12 (the
    // documented contract); this block's original 2^53-1 probes predate that
    // bound. Keep the safety intent with the largest ADMITTED magnitudes.
    let s = null;
    try { s = new Duration({ months: -1000000000000, days: 1000000000000 }).toString(); }
    catch (e) { s = "threw"; }
    ok(s === "threw",
       "the largest admitted mixed-sign duration is refused by toString, never corrupting the stack");
    ok(new Duration({ days: 1000000000000 }).toString().length < 90,
       "the largest admitted single-component duration stays inside the buffer");
    let over = null;
    try { new Duration({ days: 1000000000001 }); } catch (e) { over = e; }
    ok(over instanceof RangeError,
       "a component above the documented 1e12 bound is refused");
}

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

    const q = { "b": 1, "a": 2, "ab": 3, "abc": 4 };
    eqj(keysOf(StableStringify(q)), ["a", "ab", "abc", "b"],
        "RFC 8785 JCS: code-unit order, prefix first");
    const cb = CBORCanonical(q);
    eq(cb[1] & 0xe0, 0x60, "CBORCanonical emits a text key first");
    eq(String.fromCharCode(cb[2]), "a", "CBOR canonical: a 1-char key sorts before longer ones");
    eq(String.fromCharCode(cb[5]), "b", "CBOR canonical is LENGTH-first: 'b' precedes 'ab'");

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
    for (const n of [16, 127, 128, 129, 300]) {
        const a = {}, b = {};
        for (let i = 0; i < n; i++) a["k" + i] = i;
        for (let i = n - 1; i >= 0; i--) b["k" + i] = i;
        eq(ValueHash(a), ValueHash(b), "ValueHash stable at n=" + n + " (crosses the 128 table)");
    }
    const z1 = {}; z1["a b"] = 1; z1["a a"] = 2;
    const z2 = {}; z2["a a"] = 2; z2["a b"] = 1;
    eq(ValueHash(z1), ValueHash(z2), "NUL-bearing keys hash independent of insertion order");
    ok(CBORCanonical(z1).length === CBORCanonical(z2).length, "...and encode to the same length");
}

{
    const wpad = (b, n) => { let s = b; while (s.length < n) s += "一二三四五六七八"; return s.slice(0, n); };
    const npad = (b, n) => { let s = b; while (s.length < n) s += "abcdefghijklmnop"; return s.slice(0, n); };
    const LENS = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 64, 128];
    for (const n of LENS) {
        for (const [kind, pad] of [["narrow", npad], ["wide", wpad]]) {
            const a = pad("k" + n + "_", n);
            const b = (a + "!").slice(0, n);
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
        if (n > 0) {
            const nar = npad("m_", n - 1) + "b";
            const wid = npad("m_", n - 1) + "一";
            eq(nar.length, wid.length, "mixed len " + n + ": equal lengths");
            ok(nar !== wid, "mixed len " + n + ": a wide unit never equals a byte");
            ok(nar < wid, "mixed len " + n + ": ordering across widths");
        }
    }
    for (const [kind, pad] of [["narrow", npad], ["wide", wpad]]) {
        const m = new Map(), keys = [];
        for (let i = 0; i < 500; i++) { const k = pad("key" + i + "_", 12); keys.push(k); m.set(k, i); }
        let hits = 0;
        for (let i = 0; i < 500; i++) if (m.get((keys[i] + "#").slice(0, 12)) === i) hits++;
        eq(hits, 500, kind + " Map keys: every distinct-object lookup hits");
        ok(!m.has(pad("absent_", 12) + "zz"), kind + " Map: an absent key misses");
    }
    const ws = ["二", "一", "三z", "三a", "鿿"];
    eqj(ws.slice().sort(), ["一", "三a", "三z", "二", "鿿"],
        "wide sort matches UTF-16 code-unit order (4E00 4E09 4E8C 9FFF)");
}

{
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

    eqj([1, 2].zip([3, 4]), [[1, 3], [2, 4]], "zip fast arm");
    eqj([].zip([]), [], "zip of two empty arrays");
    eqj([1, 2, 3].zip([9]), [[1, 9]], "zip truncates to the shorter operand");
    {
        const a = [1, 2]; a.length = 5;
        const r = a.zip([9, 8, 7, 6, 5]);
        eq(r.length, 5, "zip length>count takes the generic arm without over-reading");
        eq(r[0][0], 1, "...and the present elements are right");
        ok(r[2][0] === undefined, "...and the absent ones are undefined");
    }
    {
        const h = [1, 2, 3]; delete h[1];
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

    for (let n = 2; n <= 17; n++) {
        const src = [];
        for (let i = 0; i < n; i++) src.push({ k: i % 3, id: i });
        const want = src.map((_, i) => i).sort((a, b) => (src[a].k - src[b].k) || (a - b));
        eqj(src.sortBy("k").map(x => x.id), want, "sortBy stable at n=" + n + " (pass parity)");
    }
    eqj([3, 1, 2].sortBy(), [1, 2, 3], "sortBy odd pass count lands in the caller's buffer");
    eqj([{ k: 2 }, { k: 1 }, { k: 3 }].sortBy("k", true).map(x => x.k), [3, 2, 1], "sortBy descending");
}

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

    for (const n of [16, 63, 64, 65, 2000]) {
        const root = {}; const child = { c: 1 };
        for (let i = 0; i < n; i++) root["a" + i] = { v: i };
        root.s1 = child; root.s2 = child;
        const r = sclone(root);
        eq(Object.keys(r).length, n + 2, "clone n=" + n + " keeps every key");
        eq(r["a" + (n - 1)].v, n - 1, "clone n=" + n + " copies values");
        ok(r.s1 === r.s2, "clone n=" + n + " preserves sharing across the memo threshold");
    }
    let deep = {}; const top = deep;
    for (let i = 0; i < 400; i++) { deep.n = {}; deep = deep.n; }
    let threw = false;
    try { sclone(top); } catch (e) { threw = true; }
    ok(threw, "clone refuses nesting past its depth cap");
}

{
    for (let i = 0; i < 20000; i++) {
        const o = {};
        o["sessionId_" + i] = "v";
        o["tokenName_" + i] = "w";
        o[0] = 1;
        ValueHash(o);
    }
    ok(true, "20000 encodes of string-then-integer-key objects completed");
    const o = { a: "x", 0: 1 };
    eq(ValueHash(o), ValueHash({ a: "x", 0: 1 }), "the fallback path still hashes deterministically");
}

{
    const refU = (bits, a) => { const m = 1n << BigInt(bits); return ((a % m) + m) % m; };
    const refI = (bits, a) => {
        if (bits === 0) return 0n;
        const m = 1n << BigInt(bits), h = 1n << BigInt(bits - 1);
        const v = ((a % m) + m) % m;
        return v >= h ? v - m : v;
    };
    const vals = [-1n, -2n, -12345678901234567890n, 0n, 1n, 255n,
                  1n << 63n, -(1n << 63n), (1n << 64n) - 1n, -(1n << 64n),
                  (1n << 200n) - 1n, -(1n << 200n) - 7n];
    let wrongU = 0, wrongI = 0, neg = 0;
    for (let bits = 0; bits <= 160; bits++) {
        for (const a of vals) {
            const u = BigInt.asUintN(bits, a);
            if (u !== refU(bits, a)) wrongU++;
            if (u < 0n) neg++;
            if (BigInt.asIntN(bits, a) !== refI(bits, a)) wrongI++;
        }
    }
    eq(wrongU, 0, "BigInt.asUintN matches a mod 2^bits over 160 widths");
    eq(neg, 0, "BigInt.asUintN never returns a negative BigInt");
    eq(wrongI, 0, "BigInt.asIntN is unchanged");

    if (typeof [].groupBy === "function") {
        const r = [1, 2].groupBy(() => "__proto__");
        ok(Object.getPrototypeOf(r) === Object.prototype,
           "groupBy: a __proto__ key does not retarget the result's prototype");
        ok(Array.isArray(r.__proto__ === Object.prototype ? r["__proto__"] : null) ||
           Object.hasOwn(r, "__proto__"),
           "groupBy: the __proto__ group is an own property of the result");
        const g = [1, 2, 3].groupBy((x) => (x % 2 ? "odd" : "even"));
        eqj(g.odd, [1, 3], "groupBy still groups normally");
        eqj(g.even, [2], "groupBy even bucket");
    }
}

{
    const base = "Thu, 01 Jan 1970 00:00:00 GMT";
    eq(Date.parse(base), 0, "the plain form parses to the epoch");
    for (const pad of [1, 50, 120, 500, 5000]) {
        eq(Date.parse(" ".repeat(pad) + base), 0,
           "leading whitespace x" + pad + " does not change the timestamp");
    }
    eq(Date.parse("\t\n\r\f\v " + base), 0, "every whitespace form is skipped");
    ok(Number.isNaN(Date.parse(base + "x".repeat(500))), "an over-long input is NaN");
    ok(Number.isNaN(Date.parse("x".repeat(500))), "long garbage is NaN");
    eq(new Date(8.64e15).toISOString(), "+275760-09-13T00:00:00.000Z", "max Date");
    eq(new Date(-8.64e15).toISOString(), "-271821-04-20T00:00:00.000Z", "min Date");
    ok(Number.isNaN(new Date(8.64e15 + 1).getTime()), "one past the range is not a Date");
    for (const t of [0, 1, -1, 1e12, -1e12, 8.64e15, -8.64e15]) {
        eq(Date.parse(new Date(t).toISOString()), t, "toISOString round-trips at " + t);
    }
}

{
    const big = new Decimal("1".repeat(9000));
    let threw = null;
    try { big.mul(big); } catch (e) { threw = e; }
    ok(threw && /digit-pairs|too large/.test(threw.message),
       "decimal: an over-large multiply is refused");
    eq(String(new Decimal("123456789").mul(new Decimal("987654321"))),
       "121932631112635269", "decimal: an ordinary multiply is unaffected");
    eq(String(new Decimal("1".repeat(4000)).mul(new Decimal("1"))).length, 4000,
       "decimal: a lopsided multiply still runs");

    const t = new Table();
    for (const n of [1, 50, 175, 183, 184, 185, 200, 1000]) {
        const r = "r".repeat(n);
        t.put(r, "c", n);
        eq(t.get(r, "c"), n, "table key len " + n + " round-trips");
        ok(t.has(r, "c"), "table key len " + n + " has()");
    }
    ok(t.delete("r".repeat(184), "c"), "table delete at the inline boundary");
    ok(!t.has("r".repeat(184), "c"), "...and the entry is gone");
    eq(t.get("r".repeat(183), "c"), 183, "...while its neighbour survives");
}

{
    const run = (fn, vals) => { const a = new Float32Array(vals); simd[fn](a); return Array.from(a); };

    const sq = run("vsqrt", [0, 1, 4, 9, 100, 1e-8]);
    eq(sq[0], 0, "vsqrt(0) is 0, not NaN");
    for (const [i, v] of [[1, 1], [2, 4], [3, 9], [4, 100]]) {
        ok(Math.abs(sq[i] - Math.sqrt(v)) < 1e-3 * Math.max(1, Math.sqrt(v)),
           "vsqrt(" + v + ") ~ " + Math.sqrt(v) + " (got " + sq[i] + ")");
    }
    ok(sq.every((v) => !Number.isNaN(v)), "no NaN anywhere in the vsqrt result");

    const ex = run("vexp", [0, 1, 10, 80, 88, 90, 100, 200]);
    eq(ex[0], 1, "vexp(0) is 1");
    ok(ex[2] > 1e4 && ex[2] < 1e5, "vexp(10) is near 22026 (got " + ex[2] + ")");
    for (let i = 1; i < ex.length; i++) {
        ok(ex[i] >= ex[i - 1], "vexp is monotone at index " + i +
           " (" + ex[i - 1] + " -> " + ex[i] + ")");
    }
    ok(ex[5] > 1e38, "vexp(90) overflows UPWARD, not to zero (got " + ex[5] + ")");
    ok(ex[6] > 1e38, "vexp(100) likewise (got " + ex[6] + ")");
    ok(ex.every((v) => v >= 0), "vexp never returns a negative");
}

print("test_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
