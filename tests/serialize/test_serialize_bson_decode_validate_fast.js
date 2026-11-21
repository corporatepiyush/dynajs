// covers: dyna:serialize.BSONDecode
// BSON validation has a fast path (P5: Database.load spent ~55% of its CPU validating documents it had itself written):
// one pass over the common types (fixed-size ones, strings, binary, documents, arrays) that checks UTF-8 only where a
// key or string holds a byte >= 0x80; any other type, or anything out of place, falls back to the full validator, so
// the fast path may only ever accept what the full one accepts. (A first version fell back on any non-ASCII byte or
// binary field and so validated those documents twice: load +19-23% CPU on such corpora; now -25..-35%.) Oracle: an
// independent JS validator of the BSONDecode contract (lengths bounded by the container, terminators, UTF-8 of keys
// and strings with overlongs and surrogates refused, boolean 0/1, binary lengths incl. old subtype 2, nesting depth
// <= 200) answers valid, invalid or "not modelled" (regex and the deprecated types) for 30000 single-byte mutations
// of seeded documents mixing ASCII and non-ASCII keys and strings and binary; BSONDecode must accept exactly the
// documents it calls valid. Plus hand-made rows at the fast path's edges: a string whose NUL terminator moved, a key
// running into the terminator, a boolean 2, a nested length one past its container, an overlong key byte, a
// fixed-size last element short of bytes, a document at depth 200 (passes) and 201 (RangeError).
import { suite } from "../lib/kit.js";
import { BSONEncode, BSONDecode } from "dyna:serialize";

const t = suite("serialize.BSONDecode fast validation");

function utf8ok(b, s, e) {
    let i = s;
    while (i < e) {
        const c = b[i];
        if (c < 0x80) { i++; continue; }
        let n, lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) n = 1;
        else if (c >= 0xe0 && c <= 0xef) { n = 2; if (c === 0xe0) lo = 0xa0; if (c === 0xed) hi = 0x9f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; if (c === 0xf0) lo = 0x90; if (c === 0xf4) hi = 0x8f; }
        else return false;
        if (i + n >= e + 1 || i + n > e) return false;
        if (b[i + 1] < lo || b[i + 1] > hi) return false;
        for (let k = 2; k <= n; k++) if ((b[i + k] & 0xc0) !== 0x80) return false;
        i += n + 1;
    }
    return true;
}

const rd32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const FIXED = { 1: 8, 9: 8, 17: 8, 18: 8, 16: 4, 19: 16, 7: 12, 6: 0, 10: 0, 255: 0, 127: 0 };

function vdoc(b, s, avail, depth) {
    if (depth > 200) return false;
    if (avail < 5) return false;
    const n = rd32(b, s);
    if (n < 5 || n > avail || b[s + n - 1] !== 0) return false;
    const end = s + n - 1;
    let off = s + 4;
    for (;;) {
        const ty = b[off];
        if (ty === 0) return off === end;
        off++;
        let z = off;
        while (z < end && b[z] !== 0) z++;
        if (z >= end) return false;
        if (!utf8ok(b, off, z)) return false;
        off = z + 1;
        const av = end - off;
        let l;
        if (ty in FIXED) { l = FIXED[ty]; if (l > av) return false; }
        else if (ty === 8) { if (av < 1 || b[off] > 1) return false; l = 1; }
        else if (ty === 2 || ty === 13 || ty === 14) {
            if (av < 5) return false;
            const sl = rd32(b, off);
            if (sl < 1 || sl > av - 4 || b[off + 4 + sl - 1] !== 0) return false;
            if (!utf8ok(b, off + 4, off + 4 + sl - 1)) return false;
            l = 4 + sl;
        } else if (ty === 5) {
            if (av < 5) return false;
            const bl = rd32(b, off);
            if (bl > av - 5 || (b[off + 4] === 2 && (bl < 4 || rd32(b, off + 5) !== bl - 4))) return false;
            l = 5 + bl;
        } else if (ty === 3 || ty === 4) {
            const r = vdoc(b, off, av, depth + 1);
            if (r !== true) return r;
            l = rd32(b, off);
        } else return null;
        off += l;
    }
}

function valid(b) {
    return b.length >= 5 && rd32(b, 0) === b.length ? vdoc(b, 0, b.length, 0) : false;
}

function decodes(b) {
    try { BSONDecode(b); return true; } catch (e) { return false; }
}

function rng(seed) {
    let s = seed >>> 0;
    return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
}

const WORDS = ["a", "sku", "qty", "é", "日本", "x\u{1F600}", "price", "_id", "naïve", "z"];
function gen(r, depth) {
    const o = {}, k = 1 + Math.floor(r() * 5);
    for (let i = 0; i < k; i++) {
        const key = WORDS[Math.floor(r() * WORDS.length)] + i, p = r();
        if (p < 0.2) o[key] = Math.floor(r() * 1e6) - 5e5;
        else if (p < 0.3) o[key] = r() * 100;
        else if (p < 0.55) o[key] = WORDS[Math.floor(r() * WORDS.length)].repeat(1 + Math.floor(r() * 4));
        else if (p < 0.62) o[key] = r() < 0.5;
        else if (p < 0.66) o[key] = null;
        else if (p < 0.7) o[key] = new Date(1700000000000 + Math.floor(r() * 1e9));
        else if (p < 0.74) o[key] = Uint8Array.from({ length: Math.floor(r() * 6) }, () => Math.floor(r() * 256));
        else if (depth < 3 && p < 0.85) o[key] = gen(r, depth + 1);
        else if (depth < 3) o[key] = Array.from({ length: Math.floor(r() * 4) }, () => (r() < 0.5 ? Math.floor(r() * 9) : gen(r, depth + 1)));
        else o[key] = "s";
    }
    return o;
}

const BYTES = [0, 1, 2, 3, 4, 5, 8, 0x10, 0x12, 0x7f, 0x80, 0xa0, 0xbf, 0xc0, 0xc3, 0xe0, 0xed, 0xf0, 0xf4, 0xff];

t.test("30000 single-byte mutations agree with the JS validator", (a) => {
    const r = rng(0xb5011);
    let compared = 0, acceptedMut = 0;
    for (let i = 0; i < 1000; i++) {
        const base = BSONEncode(gen(r, 0));
        if (!a.eq(decodes(base), true, "intact document " + i)) return;
        a.eq(valid(base), true, "the oracle accepts the intact document " + i);
        for (let m = 0; m < 30; m++) {
            const b = base.slice(), pos = Math.floor(r() * b.length);
            b[pos] = r() < 0.7 ? BYTES[Math.floor(r() * BYTES.length)] : Math.floor(r() * 256);
            const want = valid(b);
            if (want === null) continue;
            compared++;
            if (want) acceptedMut++;
            if (!a.eq(decodes(b), want, "doc " + i + " byte " + pos + " = " + b[pos])) return;
        }
    }
    a.ok(compared > 25000, "most mutations were modelled (" + compared + ")");
    a.ok(acceptedMut > 1000, "and both outcomes occurred (" + acceptedMut + " valid)");
});

t.test("hand-made edge rows", (a) => {
    const enc = (o) => BSONEncode(o);
    const s = enc({ k: "abc" });
    const moved = s.slice(); moved[moved.length - 2] = 0x41;
    a.eq(decodes(moved), false, "string terminator replaced");
    const keyRun = s.slice(); keyRun[5] = 0x6b; keyRun[6] = 0x6b;
    a.eq(decodes(keyRun), false, "key running past its type byte");
    const bool = enc({ b: true }); bool[bool.length - 2] = 2;
    a.eq(decodes(bool), false, "boolean 2");
    const nested = enc({ d: { x: 1 } }); nested[7] += 1;
    a.eq(decodes(nested), false, "nested length one past its container");
    const over = enc({ é: 1 }); over[5] = 0xc0; over[6] = 0xa9;
    a.eq(decodes(over), false, "overlong key byte");
    const short = enc({ a: 1, n: 1.5 }), cut = new Uint8Array(short.length - 3);
    cut.set(short.subarray(0, short.length - 4)); cut[cut.length - 1] = 0; cut[0] = cut.length;
    a.eq(decodes(cut), false, "a double as the last element with 5 of its 8 bytes");
    const bin = (sub, len, inner, body) => { const n = 4 + 1 + 2 + 4 + 1 + body.length + 1, o = new Uint8Array(n); o.set([n, 0, 0, 0, 5, 0x62, 0, len, 0, 0, 0, sub]); if (inner !== null) o.set([inner, 0, 0, 0], 12); o.set(body.slice(inner !== null ? 4 : 0), inner !== null ? 16 : 12); return o; };
    a.eq(decodes(bin(0, 3, null, [1, 2, 3])), true, "binary subtype 0, 3 bytes");
    a.eq(decodes(bin(2, 7, 3, [0, 0, 0, 0, 1, 2, 3])), true, "old binary subtype 2 with a matching inner length");
    a.eq(decodes(bin(2, 7, 2, [0, 0, 0, 0, 1, 2, 3])), false, "old binary subtype 2 with an inner length one short");
    a.eq(decodes(bin(2, 3, null, [1, 2, 3])), false, "old binary subtype 2 shorter than its inner length field");
    for (const extra of [1, 2, 3, 4, 5]) a.eq(decodes(bin(0, 3 + extra, null, [1, 2, 3])), false, "binary length " + extra + " past its bytes");
    const ok8 = enc({ k: "é日\u{1F600}" });
    a.eq(BSONDecode(ok8).k, "é日\u{1F600}", "non-ASCII strings take the full path and pass");
    const wrap = (inner) => { const o = new Uint8Array(inner.length + 8), n = o.length; o.set([n & 255, (n >> 8) & 255, n >> 16, 0, 3, 0x64, 0], 0); o.set(inner, 7); return o; };
    let deep = enc({});
    for (let i = 0; i < 200; i++) deep = wrap(deep);
    a.ok(decodes(deep), "the innermost document at depth 200 (201 levels) passes");
    a.throws(() => BSONDecode(wrap(deep)), RangeError, "depth 201");
});

await t.run();
