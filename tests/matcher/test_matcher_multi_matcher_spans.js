// timeout: 240
// covers: dyna:matcher.MultiMatcher
// MultiMatcher.matchSpansInto: differential against a brute-force byte matcher and allIn
// (all-hits mode) and a leftmost-longest reference (longestOnly), over seeded random
// pattern sets and texts with overlapping, nested, shared-prefix, duplicate and
// non-ASCII patterns; the capacity contract (fill to capacity, return the total);
// argument validation (types, lengths, detached/resizable/immutable views, shared memory,
// strict options read before the views).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_multi_matcher_spans");
import { MultiMatcher } from "dyna:matcher";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const eqJ = (got, want, w) => {
    const a = JSON.stringify(got), b = JSON.stringify(want);
    ok(a === b, w, a === b ? "" : "got " + a.slice(0, 300) + " want " + b.slice(0, 300));
};
const throws = (fn, Ctor, re, w) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(e instanceof Ctor && (!re || re.test(String(e.message))), w, e.name + ": " + e.message); }
};
const enc = new TextEncoder();

function rng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function patBytes(pats) { return pats.map(p => enc.encode(p)); }
function firstIndexOf(pb) {
    const first = [];
    for (let i = 0; i < pb.length; i++) {
        let f = i;
        for (let j = 0; j < i; j++)
            if (pb[j].length === pb[i].length && pb[j].every((x, k) => x === pb[i][k])) { f = j; break; }
        first.push(f);
    }
    return first;
}
function matchesAt(buf, p, s) {
    if (s + p.length > buf.length) return false;
    for (let k = 0; k < p.length; k++) if (buf[s + k] !== p[k]) return false;
    return true;
}
function bruteAll(pats, buf) {
    const pb = patBytes(pats), first = firstIndexOf(pb), out = [];
    for (let e = 1; e <= buf.length; e++) {
        const here = [];
        for (let i = 0; i < pb.length; i++)
            if (first[i] === i && pb[i].length <= e && matchesAt(buf, pb[i], e - pb[i].length))
                here.push([e - pb[i].length, e, i]);
        here.sort((x, y) => (y[1] - y[0]) - (x[1] - x[0]));
        out.push(...here);
    }
    return out;
}
function bruteLongest(pats, buf) {
    const pb = patBytes(pats), out = [];
    let s = 0;
    while (s < buf.length) {
        let best = -1;
        for (let i = 0; i < pb.length; i++)
            if (matchesAt(buf, pb[i], s) && (best < 0 || pb[i].length > pb[best].length)) best = i;
        if (best >= 0) { out.push([s, s + pb[best].length, best]); s += pb[best].length; }
        else s++;
    }
    return out;
}
function spans(m, buf, opts, cap) {
    const c = cap === undefined ? buf.length * 8 + 8 : cap;
    const a = new Int32Array(c), b = new Int32Array(c), d = new Int32Array(c);
    const n = opts === undefined ? m.matchSpansInto(buf, a, b, d) : m.matchSpansInto(buf, a, b, d, opts);
    const out = [];
    for (let i = 0; i < Math.min(n, c); i++) out.push([a[i], b[i], d[i]]);
    return { n, out };
}
function allInBytes(m, text) {
    return m.allIn(text).map(h => {
        const s = enc.encode(text.slice(0, h.at)).length;
        return [s, s, h.index];
    });
}

print("== MultiMatcher.matchSpansInto ==");

__kitS.section("worked example: he/she/his/hers/h over 'ushers his'", () => {
    const m = new MultiMatcher(["he", "she", "his", "hers", "h"]);
    const buf = enc.encode("ushers his");
    const r = spans(m, buf);
    eqJ(r.out, [[2, 3, 4], [1, 4, 1], [2, 4, 0], [2, 6, 3], [7, 8, 4], [7, 10, 2]], "all hits by end, longest first at one end");
    ok(r.n === 6, "returns the total", String(r.n));
    eqJ(spans(m, buf, { longestOnly: true }).out, [[1, 4, 1], [7, 10, 2]], "leftmost-longest: she then his");
    eqJ(spans(m, buf, { longestOnly: false }).out, r.out, "longestOnly:false is the default");
    eqJ(spans(m, buf, {}).out, r.out, "empty opts is the default");
    eqJ(spans(m, buf, undefined).out, r.out, "undefined opts is the default");
    eqJ(spans(new MultiMatcher(["abc", "b", "bcd"]), enc.encode("abcd"), { longestOnly: true }).out, [[0, 3, 0]],
        "leftmost wins over a longer later-starting hit");
    eqJ(spans(new MultiMatcher(["ab", "abcd", "abc"]), enc.encode("abcdab"), { longestOnly: true }).out, [[0, 4, 1], [4, 6, 0]],
        "longest at one start, resume after its end");
    eqJ(spans(new MultiMatcher(["aa"]), enc.encode("aaaaa"), { longestOnly: true }).out, [[0, 2, 0], [2, 4, 0]],
        "non-overlapping: aaaaa -> [0,2) [2,4)");
    eqJ(spans(new MultiMatcher(["aa"]), enc.encode("aaaaa")).out, [[0, 2, 0], [1, 3, 0], [2, 4, 0], [3, 5, 0]],
        "all overlapping hits of aa");
});

__kitS.section("duplicates, ties and non-ASCII byte offsets", () => {
    const m = new MultiMatcher(["x", "ab", "ab", "b"]);
    const buf = enc.encode("abx");
    eqJ(spans(m, buf).out, [[0, 2, 1], [1, 2, 3], [2, 3, 0]], "identical patterns report the first index only");
    eqJ(spans(m, buf, { longestOnly: true }).out, [[0, 2, 1], [2, 3, 0]], "longestOnly tie at one start and length -> lowest index");
    const w = new MultiMatcher(["é", "日本", "\u{1F600}", "bé"]);
    const t = "aébé日本\u{1F600}!";
    eqJ(spans(w, enc.encode(t)).out, [[1, 3, 0], [3, 6, 3], [4, 6, 0], [6, 12, 1], [12, 16, 2]],
        "UTF-8 byte offsets, end exclusive");
    eqJ(spans(w, enc.encode(t), { longestOnly: true }).out, [[1, 3, 0], [3, 6, 3], [6, 12, 1], [12, 16, 2]],
        "longestOnly over multi-byte text");
    const partial = new Uint8Array([0xc3, 0xa9, 0xc3]);
    eqJ(spans(w, partial).out, [[0, 2, 0]], "buf bytes need not be valid UTF-8");
    eqJ(spans(w, new Uint8Array(0)), { n: 0, out: [] }, "empty buf -> 0");
    eqJ(spans(w, new Uint8Array(0), { longestOnly: true }), { n: 0, out: [] }, "empty buf longestOnly -> 0");
    eqJ(spans(w, enc.encode("zzz")), { n: 0, out: [] }, "no hit -> 0");
    const big = new MultiMatcher(["abcdefghij", "a"]);
    eqJ(spans(big, enc.encode("abcdefghi"), { longestOnly: true }).out, [[0, 1, 1]], "a long pattern cut off by the end of buf");
    eqJ(spans(big, enc.encode("abcdefghij"), { longestOnly: true }).out, [[0, 10, 0]], "a long pattern ending exactly at the end");
});

__kitS.section("subarray buf reads only its own bytes", () => {
    const m = new MultiMatcher(["ab", "b"]);
    const whole = enc.encode("abab");
    eqJ(spans(m, whole.subarray(1, 3)).out, [[0, 1, 1]], "offsets relative to the view, view end respected");
    eqJ(spans(m, whole.subarray(2)).out, [[0, 2, 0], [1, 2, 1]], "byteOffset view");
});

__kitS.section("differential: seeded random patterns and texts", () => {
    const alphas = [["a", "b"], ["a", "b", "c"], ["a", "é", "日"], ["x", "\u{1F600}", "y", "z"]];
    let cases = 0, hitsAll = 0, hitsLong = 0, bad = 0;
    for (let seed = 1; seed <= 600; seed++) {
        const r = rng(seed * 7919);
        const al = alphas[seed % alphas.length];
        const word = (lo, hi) => { let s = ""; const L = lo + Math.floor(r() * (hi - lo + 1)); for (let i = 0; i < L; i++) s += al[Math.floor(r() * al.length)]; return s; };
        const text = word(0, seed % 5 === 0 ? 400 : 60);
        const np = 1 + Math.floor(r() * 10);
        const pats = [];
        for (let i = 0; i < np; i++) {
            const k = r();
            if (k < 0.3 && text.length > 1) {
                const cps = [...text], s = Math.floor(r() * cps.length), L = 1 + Math.floor(r() * Math.min(8, cps.length - s));
                pats.push(cps.slice(s, s + L).join(""));
            } else if (k < 0.45 && pats.length) {
                const b = pats[Math.floor(r() * pats.length)];
                pats.push(r() < 0.5 ? b : b + word(1, 2));
            } else pats.push(word(1, 6));
        }
        const m = new MultiMatcher(pats);
        const buf = enc.encode(text);
        const want = bruteAll(pats, buf);
        const got = spans(m, buf);
        const viaAllIn = allInBytes(m, text);
        const okAll = got.n === want.length && JSON.stringify(got.out) === JSON.stringify(want)
            && JSON.stringify(viaAllIn) === JSON.stringify(want.map(h => [h[0], h[0], h[2]]));
        const wantL = bruteLongest(pats, buf);
        const gotL = spans(m, buf, { longestOnly: true });
        const okL = gotL.n === wantL.length && JSON.stringify(gotL.out) === JSON.stringify(wantL);
        const okCount = m.countIn(text) === got.n;
        if (!(okAll && okL && okCount)) {
            if (bad++ < 3) print("  seed " + seed + " pats " + JSON.stringify(pats) + " text " + JSON.stringify(text.slice(0, 80))
                + "\n   all got " + JSON.stringify(got.out).slice(0, 200) + "\n   all want " + JSON.stringify(want).slice(0, 200)
                + "\n   lo got " + JSON.stringify(gotL.out).slice(0, 200) + "\n   lo want " + JSON.stringify(wantL).slice(0, 200));
        }
        cases++; hitsAll += want.length; hitsLong += wantL.length;
    }
    ok(bad === 0, "600 seeded cases match brute force, allIn and countIn", bad + " mismatching");
    ok(hitsAll > 5000 && hitsLong > 2000, "the random cases are dense", hitsAll + " / " + hitsLong);
});

__kitS.section("longestOnly over long text with a long pattern (ring wrap-around)", () => {
    const r = rng(42);
    const pats = ["abababababababababab", "ab", "ba", "aab", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "b"];
    let text = "";
    for (let i = 0; i < 20000; i++) text += r() < 0.08 ? "b".repeat(30 + Math.floor(r() * 20)) : (r() < 0.5 ? "a" : "b");
    const buf = enc.encode(text), m = new MultiMatcher(pats);
    const want = bruteLongest(pats, buf), got = spans(m, buf, { longestOnly: true });
    ok(got.n === want.length && JSON.stringify(got.out) === JSON.stringify(want), "long text matches the reference",
        got.n + " vs " + want.length);
});

__kitS.section("capacity: fill to capacity, return the total, leave the rest", () => {
    const m = new MultiMatcher(["a", "aa", "aaa"]);
    const buf = enc.encode("aaaaaa");
    const full = spans(m, buf).out, fullL = spans(m, buf, { longestOnly: true }).out;
    ok(full.length === 15 && fullL.length === 2, "reference totals", full.length + "/" + fullL.length);
    for (const [opts, ref] of [[undefined, full], [{ longestOnly: true }, fullL]]) {
        for (const cap of [0, 1, ref.length - 1, ref.length, ref.length + 3]) {
            const a = new Int32Array(cap).fill(-7), b = new Int32Array(cap).fill(-7), d = new Int32Array(cap).fill(-7);
            const n = m.matchSpansInto(buf, a, b, d, opts);
            const k = Math.min(cap, ref.length);
            const got = [];
            for (let i = 0; i < k; i++) got.push([a[i], b[i], d[i]]);
            const tailOk = [...a.slice(k), ...b.slice(k), ...d.slice(k)].every(x => x === -7);
            ok(n === ref.length, "cap " + cap + (opts ? " longest" : "") + " returns the total", String(n));
            eqJ(got, ref.slice(0, k), "cap " + cap + (opts ? " longest" : "") + " writes the leading hits");
            ok(tailOk, "cap " + cap + (opts ? " longest" : "") + " leaves slots past the written prefix untouched");
        }
    }
    const n = m.matchSpansInto(buf, new Int32Array(2), new Int32Array(2), new Int32Array(2));
    const a = new Int32Array(n), b = new Int32Array(n), d = new Int32Array(n);
    ok(m.matchSpansInto(buf, a, b, d) === n, "grow to the returned count and retry");
    eqJ([...a].map((s, i) => [s, b[i], d[i]]), full, "the retry fills every hit");
    const big = "a".repeat(200000);
    ok(m.matchSpansInto(enc.encode(big), new Int32Array(0), new Int32Array(0), new Int32Array(0)) === m.countIn(big),
        "capacity 0 counts like countIn on 200 KB", String(m.countIn(big)));
});

__kitS.section("validation: argument types and lengths", () => {
    const m = new MultiMatcher(["ab"]);
    const buf = enc.encode("abab"), o = () => new Int32Array(4);
    throws(() => m.matchSpansInto("abab", o(), o(), o()), TypeError, /buf must be a Uint8Array/, "string buf");
    throws(() => m.matchSpansInto(buf.buffer, o(), o(), o()), TypeError, /buf must be a Uint8Array/, "ArrayBuffer buf");
    throws(() => m.matchSpansInto(new Int8Array(4), o(), o(), o()), TypeError, /buf must be a Uint8Array/, "Int8Array buf");
    throws(() => m.matchSpansInto(new Uint8ClampedArray(4), o(), o(), o()), TypeError, /buf must be a Uint8Array/, "Uint8ClampedArray buf");
    throws(() => m.matchSpansInto(new DataView(new ArrayBuffer(4)), o(), o(), o()), TypeError, /buf must be a Uint8Array/, "DataView buf");
    throws(() => m.matchSpansInto(buf, new Uint32Array(4), o(), o()), TypeError, /outStarts must be an Int32Array/, "Uint32Array outStarts");
    throws(() => m.matchSpansInto(buf, o(), new Float64Array(4), o()), TypeError, /outEnds must be an Int32Array/, "Float64Array outEnds");
    throws(() => m.matchSpansInto(buf, o(), o(), [0, 0, 0, 0]), TypeError, /outIds must be an Int32Array/, "plain array outIds");
    throws(() => m.matchSpansInto(buf, o(), o()), TypeError, /outIds/, "missing outIds");
    throws(() => m.matchSpansInto(buf, o(), o(), new Int32Array(3)), TypeError, /same length/, "unequal lengths");
    throws(() => m.matchSpansInto(buf, new Int32Array(5), o(), o()), TypeError, /same length/, "unequal lengths (first longer)");
    throws(() => MultiMatcher.prototype.matchSpansInto.call({}, buf, o(), o(), o()), TypeError, null, "foreign receiver");
});

__kitS.section("validation: options are strict and read first", () => {
    const m = new MultiMatcher(["ab"]);
    const buf = enc.encode("abab"), o = () => new Int32Array(4);
    throws(() => m.matchSpansInto(buf, o(), o(), o(), { bogus: 1 }), TypeError, /unknown option "bogus" \(valid: longestOnly\)/, "unknown key");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), { longestOnly: true, longest: true }), TypeError, /unknown option "longest"/, "near-miss key");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), { longestOnly: 1 }), TypeError, /longestOnly must be a boolean/, "numeric longestOnly");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), { longestOnly: "true" }), TypeError, /longestOnly must be a boolean/, "string longestOnly");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), null), TypeError, /opts must be an object/, "null opts");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), true), TypeError, /opts must be an object/, "boolean opts");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), [true]), TypeError, /opts must be an object/, "array opts");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), () => 1), TypeError, /opts must be an object/, "function opts");
    throws(() => m.matchSpansInto(buf, o(), o(), o(), { [Symbol("k")]: 1 }), TypeError, /unknown option/, "symbol key");
    const trap = new Proxy({}, { ownKeys: () => ["longestOnIy"], getOwnPropertyDescriptor: () => ({ value: true, enumerable: true, configurable: true }) });
    throws(() => m.matchSpansInto(buf, o(), o(), o(), trap), TypeError, /unknown option "longestOnIy"/, "proxy reporting a look-alike key");
    const boom = new Proxy({}, { ownKeys: () => { throw new RangeError("trap"); } });
    throws(() => m.matchSpansInto(buf, o(), o(), o(), boom), RangeError, /trap/, "ownKeys trap error propagates");
    const inherited = Object.create({ longestOnly: true });
    eqJ(spans(new MultiMatcher(["a", "aa"]), enc.encode("aa"), inherited).n, 1, "inherited longestOnly is read like any option");
    eqJ(spans(m, buf, { longestOnly: undefined }).n, 2, "longestOnly undefined is the default");
    const ab = new ArrayBuffer(4);
    const b2 = new Uint8Array(ab); b2.set(enc.encode("abab"));
    const getter = { get longestOnly() { ab.transfer(); return true; } };
    Object.defineProperty(getter, "longestOnly", { enumerable: true });
    throws(() => m.matchSpansInto(b2, o(), o(), o(), getter), TypeError, /detached/, "a getter detaching buf is seen (options read before views)");
    const rab = new ArrayBuffer(16, { maxByteLength: 64 });
    const outS = new Int32Array(rab);
    const grow = { get longestOnly() { rab.resize(64); return false; } };
    Object.defineProperty(grow, "longestOnly", { enumerable: true });
    throws(() => m.matchSpansInto(buf, outS, o(), o(), grow), TypeError, /same length/, "a getter growing outStarts is seen (16 vs 4)");
});

__kitS.section("validation: detached, resizable, immutable, shared memory", () => {
    const m = new MultiMatcher(["ab"]);
    const o = () => new Int32Array(4);
    const det = enc.encode("abab"); det.buffer.transfer();
    throws(() => m.matchSpansInto(det, o(), o(), o()), TypeError, /buf is detached/, "detached buf");
    const dOut = o(); dOut.buffer.transfer();
    throws(() => m.matchSpansInto(enc.encode("ab"), o(), dOut, o()), TypeError, /outEnds is detached/, "detached outEnds");
    const rab = new ArrayBuffer(8, { maxByteLength: 64 });
    const fixed = new Uint8Array(rab, 0, 8); fixed.set(enc.encode("abababab"));
    rab.resize(4);
    throws(() => m.matchSpansInto(fixed, o(), o(), o()), TypeError, /out of bounds/, "fixed view past a shrunk buffer");
    const track = new Uint8Array(rab);
    ok(m.matchSpansInto(track, o(), o(), o()) === 2, "length-tracking buf is read at its current length");
    rab.resize(8); track.set(enc.encode("abababab"));
    ok(m.matchSpansInto(track, o(), o(), o()) === 4, "and after growing");
    const ro = new ArrayBuffer(32, { maxByteLength: 64 });
    const tS = new Int32Array(ro, 0), tE = new Int32Array(4), tI = new Int32Array(4);
    ro.resize(16);
    ok(m.matchSpansInto(enc.encode("ababab"), tS, tE, tI) === 3 && tS[2] === 4 && tE[2] === 6, "length-tracking out arrays at their current length");
    const outsShrunk = new Int32Array(new ArrayBuffer(16, { maxByteLength: 64 }), 0, 4);
    outsShrunk.buffer.resize(8);
    throws(() => m.matchSpansInto(enc.encode("ab"), outsShrunk, o(), o()), TypeError, /outStarts.*out of bounds/, "fixed out view past a shrunk buffer");
    if (typeof ArrayBuffer.prototype.transferToImmutable === "function") {
        const im = new Int32Array(new ArrayBuffer(16).transferToImmutable());
        throws(() => m.matchSpansInto(enc.encode("ab"), o(), o(), im), TypeError, null, "immutable outIds");
        const imBuf = new Uint8Array(enc.encode("abab").buffer.transferToImmutable());
        ok(m.matchSpansInto(imBuf, o(), o(), o()) === 2, "immutable buf is fine (read only)");
    }
    const shared = new ArrayBuffer(48);
    const s0 = new Int32Array(shared, 0, 4), s1 = new Int32Array(shared, 16, 4), s2 = new Int32Array(shared, 32, 4);
    ok(m.matchSpansInto(enc.encode("abab"), s0, s1, s2) === 2 && s1[1] === 4, "disjoint views of one buffer are fine");
    throws(() => m.matchSpansInto(enc.encode("abab"), s0, s0, s2), TypeError, /overlap|share/, "the same array twice");
    throws(() => m.matchSpansInto(enc.encode("abab"), new Int32Array(shared, 0, 4), new Int32Array(shared, 12, 4), s2), TypeError, /overlap|share/, "partially overlapping outs");
    const ub = new Uint8Array(shared, 32, 16); ub.set(enc.encode("abababababababab"));
    throws(() => m.matchSpansInto(ub, s0, s1, s2), TypeError, /overlap|share/, "buf sharing memory with outIds");
    const ub2 = new Uint8Array(shared, 0, 16); ub2.fill(0); ub2.set(enc.encode("abab"));
    ok(m.matchSpansInto(ub2, new Int32Array(shared, 4, 0), new Int32Array(shared, 8, 0), new Int32Array(shared, 12, 0)) === 2, "empty out views inside buf are not an overlap");
    ok(m.matchSpansInto(new Uint8Array(shared, 0, 0), new Int32Array(shared, 0, 0), new Int32Array(shared, 0, 0), new Int32Array(shared, 0, 0)) === 0,
        "empty views never overlap");
});

__kitS.section("count beyond int32 capacity: totals are numbers, not truncated", () => {
    const m = new MultiMatcher(["a", "aa", "aaa", "aaaa"]);
    const text = "a".repeat(300000);
    const n = m.matchSpansInto(enc.encode(text), new Int32Array(1), new Int32Array(1), new Int32Array(1));
    ok(n === 4 * 300000 - 6, "dense total", String(n));
    const nl = m.matchSpansInto(enc.encode(text), new Int32Array(1), new Int32Array(1), new Int32Array(1), { longestOnly: true });
    ok(nl === 75000, "dense longestOnly total", String(nl));
});

__kitS.section("step budget: the same RangeError as allIn/countIn in both modes", () => {
    const pats = [];
    for (let i = 1; i <= 64; i++) pats.push("a".repeat(i));
    const m = new MultiMatcher(pats);
    const buf = new Uint8Array(16 << 20).fill(97);
    const z = () => new Int32Array(1);
    throws(() => m.matchSpansInto(buf, z(), z(), z()), RangeError, /byte-step budget/, "all hits past 1e9 steps");
    throws(() => m.matchSpansInto(buf, z(), z(), z(), { longestOnly: true }), RangeError, /byte-step budget/, "longestOnly past 1e9 steps");
});

__kitS.end();
print("");
print("test_matcher_multi_matcher_spans: " + (fail ? "FAILURES: " + fail + " / " : "all ") + (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_matcher_multi_matcher_spans: " + fail + " of " + (pass + fail) + " assertions FAILED");
