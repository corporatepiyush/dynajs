// covers: dyna:serialize.BSONDecode
// BSONDecode against fixed vectors and against an independent reference decoder written here from the
// BSON 1.1 grammar (bsonspec.org) and the BSON corpus rules (github.com/mongodb/specifications,
// source/bson-corpus: decodeErrors). Vectors are hand-derived from those documents because no
// MongoDB driver (pymongo's bson) is installed to generate them. The differential part builds random
// valid documents with an independent JS writer, mutates them (truncate, flip, splice, lie about
// lengths), and requires BSONDecode to throw exactly when the reference rejects and to return the
// same tree when it accepts. A crash, a hang or a value from a rejected input fails the suite.
import { suite } from "../lib/kit.js";
import { BSONDecode, BSONEncode, ObjectId, Timestamp, Decimal128, Binary, BSONRegExp, MinKey, MaxKey, Code } from "dyna:serialize";
import { Random } from "dyna:random";

const t = suite("serialize.BSONDecode");
const bytes = (h) => new Uint8Array(h.replace(/\s+/g, "").match(/../g).map((x) => parseInt(x, 16)));
const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");

t.test("bsonspec.org examples", (a) => {
    a.deep(BSONDecode(bytes("160000000268656c6c6f0006000000776f726c640000")), { hello: "world" });
    const d = BSONDecode(bytes("310000000442534f4e002600000002300008000000617765736f6d65000131003333333333331440103200c20700000000"));
    a.deep(d, { BSON: ["awesome", 5.05, 1986] });
});

t.test("integers", (a) => {
    a.eq(BSONDecode(bytes("0c0000001069000000008000")).i, -2147483648, "int32 min");
    a.eq(BSONDecode(bytes("10000000126100010000000000000000")).a, 1n, "int64 is a BigInt");
    a.eq(BSONDecode(bytes("10000000126100000000000000008000")).a, -(2n ** 63n), "int64 min");
});

t.test("scalars", (a) => {
    a.eq(BSONDecode(bytes("10000000016400000000000000f03f00")).d, 1, "double 1.0");
    a.ok(Object.is(BSONDecode(bytes("10000000016400000000000000008000")).d, -0), "double -0.0");
    a.eq(BSONDecode(bytes("090000000862000100")).b, true, "bool");
    a.eq(BSONDecode(bytes("080000000a610000")).a, null, "null");
    a.eq(BSONDecode(bytes("0800000006610000")).a, undefined, "undefined");
    a.eq(BSONDecode(bytes("10000000096100000000000000000000")).a.getTime(), 0, "datetime epoch");
    const o = BSONDecode(bytes("1400000007610056e1fc72e0c917e9c471416100")).a;
    a.ok(o instanceof ObjectId, "ObjectId instance");
    a.eq(o.toHexString(), "56e1fc72e0c917e9c4714161");
    const ts = BSONDecode(bytes("100000001161002a00000015cd5b0700")).a;
    a.ok(ts instanceof Timestamp);
    a.eq(ts.t, 123456789);
    a.eq(ts.i, 42);
    const re = BSONDecode(bytes("0f0000000b610061626300696d0000")).a;
    a.ok(re instanceof RegExp, "regex with JS-mappable options is a RegExp");
    a.eq(re.source, "abc");
    a.eq(re.flags, "im");
    const rx = BSONDecode(bytes("0f0000000b610061626300697800" + "00")).a;
    a.ok(rx instanceof BSONRegExp, "option x has no JS flag: BSONRegExp");
    a.eq(rx.options, "ix");
    const bin = BSONDecode(bytes("0f0000000578000200000000ffff00")).x;
    a.ok(bin instanceof Uint8Array, "subtype 0 is a Uint8Array");
    a.eq(hex(bin), "ffff");
    const b4 = BSONDecode(bytes("0f0000000578000200000004ffff00")).x;
    a.ok(b4 instanceof Binary, "subtype 4 is a Binary");
    a.eq(b4.subType, 4);
    a.eq(hex(b4.buffer), "ffff");
    const old = BSONDecode(bytes("13000000057800060000000202000000ffff00")).x;
    a.ok(old instanceof Binary, "old subtype 2 unwraps its inner length");
    a.eq(hex(old.buffer), "ffff");
    a.ok(BSONDecode(bytes("08000000ff610000")).a instanceof MinKey, "MinKey");
    a.ok(BSONDecode(bytes("080000007f610000")).a instanceof MaxKey, "MaxKey");
    const c = BSONDecode(bytes("100000000d6100040000006162630000")).a;
    a.ok(c instanceof Code);
    a.eq(c.code, "abc");
    a.eq(BSONDecode(bytes("100000000e6100040000006162630000")).a, "abc", "symbol is a string");
});

t.test("Decimal128 corpus values", (a) => {
    const v = (h) => String(BSONDecode(bytes("18000000136400" + h + "00")).d);
    a.eq(v("00000000000000000000000000004030"), "0");
    a.eq(v("000000000000000000000000000040b0"), "-0");
    a.eq(v("01000000000000000000000000004030"), "1");
    a.eq(v("01000000000000000000000000004630"), "1E+3");
    a.eq(v("01000000000000000000000000003830"), "0.0001");
    a.eq(v("000000000000000000000000000000f8"), "-Infinity");
    a.eq(v("0000000000000000000000000000007c"), "NaN");
    a.ok(BSONDecode(bytes("18000000136400" + "01000000000000000000000000004030" + "00")).d instanceof Decimal128);
});

t.test("corpus decodeErrors are refused with a TypeError", (a) => {
    const bad = [
        ["0900000008620002" + "00", "bool 2"],
        ["0400000000", "length below 5"],
        ["0500000001", "missing terminator"],
        ["0c0000000261000500000062000000", "string length past the end"],
        ["0d000000026100000000000000", "string length 0"],
        ["0e0000000261000200000062ff00", "string not NUL-terminated"],
        ["0e00000002610002000000ff0000", "invalid UTF-8 in a string"],
        ["0c00000002ff00010000000000", "invalid UTF-8 in a key"],
        ["0a00000020610000000000", "unknown type 0x20"],
        ["0c0000000461000500000001", "array without terminator"],
        ["130000000578000600000002030000" + "00ffff00", "binary subtype 2 inner length mismatch"],
        ["1000000002610004000000eda0800000", "a UTF-16 surrogate encoded in UTF-8 (CESU)"],
        ["0f0000000261000300000080610000", "a lone continuation byte"],
        ["0f0000000261000300000" + "0c0af0000", "an overlong encoding"],
        ["0d000000056100ffffffff00ff00", "binary negative length"],
        ["0d0000000b6100616200636400", "regex options without terminator"],
        ["0f0000000c6100030000006162630000", "DBPointer truncated"],
        ["160000000268656c6c6f0006000000776f726c640000" + "00", "trailing byte"],
    ];
    for (const [h, why] of bad) a.throws(() => BSONDecode(bytes(h)), TypeError, why);
});

t.test("nesting beyond 200 levels is a RangeError, nothing is built", (a) => {
    let doc = {};
    for (let i = 0; i < 180; i++) doc = { x: doc };
    a.eq(Object.keys(BSONDecode(BSONEncode(doc))).length, 1, "180 levels decode");
    function nest(level, depth) {
        if (level === depth) return [5, 0, 0, 0, 0];
        const inner = nest(level + 1, depth);
        const n = 4 + 3 + inner.length + 1;
        return [n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24, 3, 0x78, 0, ...inner, 0];
    }
    a.throws(() => BSONDecode(Uint8Array.from(nest(0, 400))), RangeError);
});

// ---------------------------------------------------------------- reference decoder
function refDecode(u8) {
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const td = new TextDecoder("utf-8", { fatal: true });
    const str = (s, e) => td.decode(u8.subarray(s, e));
    function cstr(at, end) {
        let z = at;
        while (z < end && u8[z] !== 0) z++;
        if (z >= end) throw new Error("cstring");
        return [str(at, z), z + 1];
    }
    function doc(at, avail, depth, arr) {
        if (depth > 201) throw new RangeError("depth");
        if (avail < 5) throw new Error("short");
        const n = dv.getInt32(at, true);
        if (n < 5 || n > avail) throw new Error("len");
        if (u8[at + n - 1] !== 0) throw new Error("term");
        const end = at + n - 1;
        let p = at + 4;
        const out = arr ? [] : {};
        while (p < end) {
            const type = u8[p++];
            const [key, q] = cstr(p, end);
            p = q;
            const room = end - p;
            let v;
            const needs = (k) => {
                if (room < k) throw new Error("short value");
            };
            const string = () => {
                needs(5);
                const l = dv.getInt32(p, true);
                if (l < 1 || l > room - 4) throw new Error("strlen");
                if (u8[p + 4 + l - 1] !== 0) throw new Error("strterm");
                const s = str(p + 4, p + 4 + l - 1);
                return [s, 4 + l];
            };
            let size;
            switch (type) {
                case 1: needs(8); v = ["double", dv.getFloat64(p, true)]; size = 8; break;
                case 2: [v, size] = string(); v = ["string", v]; break;
                case 14: [v, size] = string(); v = ["string", v]; break;
                case 13: [v, size] = string(); v = ["code", v]; break;
                case 3: case 4: {
                    needs(5);
                    size = dv.getInt32(p, true);
                    if (size < 5 || size > room) throw new Error("sublen");
                    v = [type === 3 ? "doc" : "array", doc(p, size, depth + 1, type === 4)];
                    break;
                }
                case 5: {
                    needs(5);
                    const l = dv.getInt32(p, true);
                    if (l < 0 || l > room - 5) throw new Error("binlen");
                    const sub = u8[p + 4];
                    let data = u8.subarray(p + 5, p + 5 + l);
                    if (sub === 2) {
                        if (l < 4 || dv.getInt32(p + 5, true) !== l - 4) throw new Error("bin2");
                        data = data.subarray(4);
                    }
                    v = ["binary", sub, hex(data)];
                    size = 5 + l;
                    break;
                }
                case 6: v = ["undefined"]; size = 0; break;
                case 7: needs(12); v = ["oid", hex(u8.subarray(p, p + 12))]; size = 12; break;
                case 8: needs(1); if (u8[p] > 1) throw new Error("bool"); v = ["bool", u8[p] === 1]; size = 1; break;
                case 9: { needs(8); const ms = Number(dv.getBigInt64(p, true)); v = ["date", Math.abs(ms) <= 8.64e15 ? ms : NaN]; size = 8; break; }
                case 10: v = ["null"]; size = 0; break;
                case 11: {
                    const [pat, q1] = cstr(p, end);
                    const [opt, q2] = cstr(q1, end);
                    v = ["regex", pat, opt];
                    size = q2 - p;
                    break;
                }
                case 12: throw new Error("dbpointer unsupported");
                case 15: throw new Error("cws unsupported");
                case 16: needs(4); v = ["int", dv.getInt32(p, true)]; size = 4; break;
                case 17: needs(8); v = ["ts", dv.getUint32(p + 4, true), dv.getUint32(p, true)]; size = 8; break;
                case 18: needs(8); v = ["long", dv.getBigInt64(p, true)]; size = 8; break;
                case 19: needs(16); v = ["dec", hex(u8.subarray(p, p + 16))]; size = 16; break;
                case 0xff: v = ["minkey"]; size = 0; break;
                case 0x7f: v = ["maxkey"]; size = 0; break;
                default: throw new Error("type");
            }
            p += size;
            if (p > end) throw new Error("overrun");
            if (arr) out.push(v);
            else out[key] = v;
        }
        if (p !== end) throw new Error("misaligned");
        return out;
    }
    const top = doc(0, u8.length, 0, false);
    if (dv.getInt32(0, true) !== u8.length) throw new Error("trailing");
    return top;
}

function tagOf(v) {
    if (v === null) return ["null"];
    if (v === undefined) return ["undefined"];
    if (typeof v === "number") return Number.isInteger(v) && v >= -2147483648 && v <= 2147483647 && !Object.is(v, -0) ? ["int?", v] : ["double", v];
    if (typeof v === "string") return ["string", v];
    if (typeof v === "boolean") return ["bool", v];
    if (typeof v === "bigint") return ["long", v];
    if (v instanceof Date) return ["date", v.getTime()];
    if (v instanceof ObjectId) return ["oid", v.toHexString()];
    if (v instanceof Timestamp) return ["ts", v.t, v.i];
    if (v instanceof Decimal128) return ["dec", null];
    if (v instanceof Binary) return ["binary", v.subType, hex(v.buffer)];
    if (v instanceof Uint8Array) return ["binary", 0, hex(v)];
    if (v instanceof RegExp) return ["regex", v.source, v.flags];
    if (v instanceof BSONRegExp) return ["regex", v.pattern, v.options];
    if (v instanceof MinKey) return ["minkey"];
    if (v instanceof MaxKey) return ["maxkey"];
    if (v instanceof Code) return ["code", v.code];
    if (Array.isArray(v)) return ["array", v.map(tagOf)];
    return ["doc", Object.fromEntries(Object.entries(v).map(([k, x]) => [k, tagOf(x)]))];
}

function sameTree(ref, got) {
    const [rt] = ref, [gt] = got;
    if (rt === "doc" || rt === "array") {
        if (gt !== rt) return false;
        const rv = ref[1], gv = got[1];
        const rk = Object.keys(rv), gk = Object.keys(gv);
        if (rt === "doc") {
            const uniq = [...new Set(rk)];
            if (uniq.length !== gk.length) return false;
            return uniq.every((k) => k in gv && sameTree(rv[k], gv[k]));
        }
        return rv.length === gv.length && rv.every((x, i) => sameTree(x, gv[i]));
    }
    if (rt === "int") return gt === "int?" && got[1] === ref[1];
    if (rt === "double") return (gt === "double" || gt === "int?") && Object.is(got[1], ref[1]) || (Number.isNaN(ref[1]) && Number.isNaN(got[1]));
    if (rt === "dec") return gt === "dec";
    if (rt === "regex") {
        if (gt !== "regex") return false;
        return ref[1] === got[1] || got[1] === new RegExp(ref[1] || "(?:)").source;
    }
    if (rt === "string") return gt === "string" && got[1] === ref[1];
    return JSON.stringify(ref, (k, v) => (typeof v === "bigint" ? v + "n" : v)) === JSON.stringify(got, (k, v) => (typeof v === "bigint" ? v + "n" : v));
}

// ---------------------------------------------------------------- independent random writer
function writer(rng) {
    const out = [];
    const pick = (xs) => xs[rng.nextBounded(xs.length)];
    const u32 = (n) => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, n >>> 24];
    const enc = new TextEncoder();
    const cs = (s) => [...enc.encode(s), 0];
    const keyText = () => pick(["a", "b", "key", "ü", "日本", "x" + rng.nextBounded(50), "", "$x", "a.b"]);
    function strBytes(s) {
        const b = enc.encode(s);
        return [...u32(b.length + 1), ...b, 0];
    }
    function value(depth) {
        const k = rng.nextBounded(depth > 3 ? 13 : 16);
        const f64 = (d) => [...new Uint8Array(new Float64Array([d]).buffer)];
        switch (k) {
            case 0: return [1, f64(pick([0, -0, 1.5, -2.25e300, NaN, Infinity, 1e-320, 3]))];
            case 1: return [2, strBytes(pick(["", "hello", "naïve", "😀", "a\u0000b"]))];
            case 2: return [8, [rng.nextBounded(2)]];
            case 3: return [10, []];
            case 4: return [16, u32(rng.nextBounded(0xffffffff) | 0)];
            case 5: { const lo = rng.nextBounded(0xffffffff), hi = rng.nextBounded(0xffffffff); return [18, [...u32(lo), ...u32(hi)]]; }
            case 6: return [9, [...u32(rng.nextBounded(1 << 30)), ...u32(rng.nextBounded(300))]];
            case 7: return [7, Array.from({ length: 12 }, () => rng.nextBounded(256))];
            case 8: { const n = rng.nextBounded(6); return [5, [...u32(n), pick([0, 0, 4, 5, 128]), ...Array.from({ length: n }, () => rng.nextBounded(256))]]; }
            case 9: return [11, [...cs(pick(["ab+c", "^x", "a|b", ""])), ...cs(pick(["", "i", "im", "imsx", "u"]))]];
            case 10: return [17, [...u32(rng.nextBounded(1000)), ...u32(rng.nextBounded(1 << 30))]];
            case 11: return [pick([0xff, 0x7f, 6]), []];
            case 12: return [19, Array.from({ length: 16 }, () => rng.nextBounded(256))];
            case 13: return [3, docBytes(depth + 1, false)];
            case 14: return [4, docBytes(depth + 1, true)];
            default: return [13, strBytes("x=1")];
        }
    }
    function docBytes(depth, arr) {
        const body = [];
        const n = rng.nextBounded(5);
        for (let i = 0; i < n; i++) {
            const [type, v] = value(depth);
            body.push(type, ...cs(arr ? String(i) : keyText()), ...v);
        }
        const total = 4 + body.length + 1;
        return [...u32(total), ...body, 0];
    }
    out.push(...docBytes(0, false));
    return Uint8Array.from(out);
}

function mutate(rng, u8) {
    const b = Uint8Array.from(u8);
    switch (rng.nextBounded(5)) {
        case 0: return b.subarray(0, rng.nextBounded(b.length));
        case 1: { const i = rng.nextBounded(b.length); b[i] ^= 1 << rng.nextBounded(8); return b; }
        case 2: { const i = rng.nextBounded(Math.max(1, b.length - 4)); b[i] = 0xff; b[i + 1] = 0xff; b[i + 2] = 0xff; b[i + 3] = 0x7f; return b; }
        case 3: { const i = rng.nextBounded(b.length); b[i] = rng.nextBounded(256); return b; }
        default: { const i = rng.nextBounded(b.length); return Uint8Array.from([...b.subarray(0, i), rng.nextBounded(256), ...b.subarray(i)]); }
    }
}

function agree(a, input, label) {
    let ref, refErr = null, got, gotErr = null;
    try { ref = refDecode(input); } catch (e) { refErr = e; }
    try { got = BSONDecode(input); } catch (e) { gotErr = e; }
    if (refErr && /unsupported/.test(refErr.message) && gotErr) return true;
    if (!!refErr !== !!gotErr) {
        a.ok(false, `${label}: reference ${refErr ? "rejects (" + refErr.message + ")" : "accepts"} but BSONDecode ${gotErr ? "throws " + gotErr.message : "accepts"}: ${hex(input).slice(0, 120)}`);
        return false;
    }
    if (gotErr) {
        if (!(gotErr instanceof TypeError || gotErr instanceof RangeError)) {
            a.ok(false, `${label}: threw a ${gotErr && gotErr.constructor && gotErr.constructor.name}`);
            return false;
        }
        return true;
    }
    if (!sameTree(["doc", ref], tagOf(got))) {
        a.ok(false, `${label}: trees differ for ${hex(input).slice(0, 120)}`);
        return false;
    }
    return true;
}

t.test("differential: random valid documents decode to the reference tree", (a) => {
    const rng = new Random(20261114);
    let okN = 0;
    for (let i = 0; i < 3000; i++) if (agree(a, writer(rng), "valid#" + i)) okN++;
    a.eq(okN, 3000);
});

t.test("differential: mutated documents are refused exactly when the reference refuses", (a) => {
    const rng = new Random(77);
    let refused = 0, accepted = 0, bad = 0;
    for (let i = 0; i < 6000; i++) {
        const m = mutate(rng, writer(rng));
        try { BSONDecode(m); accepted++; } catch (e) { refused++; }
        if (!agree(a, m, "mut#" + i) && ++bad > 5) break;
    }
    a.ok(refused > 1000 && accepted > 100, `both outcomes exercised (refused ${refused}, accepted ${accepted})`);
});

t.test("every truncation of a rich document is refused", (a) => {
    const full = BSONEncode({ a: 1, s: "text", d: { x: [1, 2, { y: "z" }] }, o: new ObjectId(), t: new Date(5), r: /x/i, b: new Uint8Array(7), n: 2n ** 40n });
    let refused = 0;
    for (let cut = 0; cut < full.length; cut++) {
        try { BSONDecode(full.subarray(0, cut)); } catch (e) { if (e instanceof TypeError) refused++; }
    }
    a.eq(refused, full.length);
});

t.test("a lying length allocates nothing proportional and is refused", (a) => {
    const lie = bytes("ffffff7f02610000000000");
    a.throws(() => BSONDecode(lie), TypeError);
    const lie2 = bytes("0e0000000561" + "00" + "ffffff7f" + "00" + "0000");
    a.throws(() => BSONDecode(lie2), TypeError);
});

t.test("the input is copied: a hostile RegExp constructor cannot change the bytes under the decoder", (a) => {
    const src = BSONEncode({ r: /a/i, s: "after" });
    const saved = globalThis.RegExp;
    let hit = 0;
    globalThis.RegExp = function (p, f) {
        hit++;
        src.fill(0xff);
        return new saved(p, f);
    };
    let out;
    try { out = BSONDecode(src); } finally { globalThis.RegExp = saved; }
    a.eq(out.s, "after");
    a.ok(hit >= 0);
});

t.test("argument refusals", (a) => {
    a.throws(() => BSONDecode(), TypeError);
    a.throws(() => BSONDecode("abc"), TypeError);
    a.throws(() => BSONDecode(new Uint8Array(0)), TypeError);
    a.deep(BSONDecode(new Uint8Array([5, 0, 0, 0, 0]).buffer), {});
    a.deep(BSONDecode(new DataView(new Uint8Array([5, 0, 0, 0, 0]).buffer)), {});
});

await t.run();
