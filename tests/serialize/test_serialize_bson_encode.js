// covers: dyna:serialize.BSONEncode
// BSONEncode: exact bytes for the bsonspec.org examples and the corpus' canonical vectors (hand-derived:
// no MongoDB driver is installed to generate them), the documented JS -> BSON type mapping, every refusal,
// and a seeded property run: BSONDecode(BSONEncode(x)) rebuilds x for random trees of every type.
import { suite } from "../lib/kit.js";
import { BSONEncode, BSONDecode, ObjectId, Timestamp, Decimal128, Binary, BSONRegExp, MinKey, MaxKey, Code } from "dyna:serialize";
import { Random } from "dyna:random";

const t = suite("serialize.BSONEncode");
const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");

t.test("bsonspec.org examples, byte for byte", (a) => {
    a.eq(hex(BSONEncode({ hello: "world" })), "160000000268656c6c6f0006000000776f726c640000");
    a.eq(hex(BSONEncode({ BSON: ["awesome", 5.05, 1986] })),
        "310000000442534f4e002600000002300008000000617765736f6d65000131003333333333331440103200c20700000000");
});

t.test("number mapping: int32 when it fits, double otherwise, BigInt is int64", (a) => {
    a.eq(hex(BSONEncode({ i: -2147483648 })), "0c0000001069000000008000");
    a.eq(hex(BSONEncode({ i: 2147483647 })), "0c000000106900ffffff7f00");
    a.eq(hex(BSONEncode({ d: 2147483648 })), "10000000016400000000000000e04100");
    a.eq(hex(BSONEncode({ d: 1.0 })), "0c000000106400" + "01000000" + "00", "1.0 is the integer 1 in JS");
    a.eq(hex(BSONEncode({ d: -0 })), "10000000016400000000000000008000", "-0 stays a double");
    a.eq(hex(BSONEncode({ d: 0.5 })), "10000000016400000000000000e03f00");
    a.eq(hex(BSONEncode({ a: 1n })), "10000000126100010000000000000000");
    a.eq(hex(BSONEncode({ a: -(2n ** 63n) })), "10000000126100000000000000008000");
    a.throws(() => BSONEncode({ a: 2n ** 63n }), RangeError);
    a.throws(() => BSONEncode({ a: -(2n ** 63n) - 1n }), RangeError);
    a.ok(Number.isNaN(BSONDecode(BSONEncode({ n: NaN })).n));
    a.eq(BSONDecode(BSONEncode({ n: -Infinity })).n, -Infinity);
});

t.test("the special types", (a) => {
    a.eq(hex(BSONEncode({ a: new ObjectId("56e1fc72e0c917e9c4714161") })), "1400000007610056e1fc72e0c917e9c471416100");
    a.eq(hex(BSONEncode({ a: new Timestamp({ t: 123456789, i: 42 }) })), "100000001161002a00000015cd5b0700");
    a.eq(hex(BSONEncode({ a: new Date(0) })), "10000000096100000000000000000000");
    a.eq(hex(BSONEncode({ a: /abc/mi })), "0f0000000b610061626300696d0000", "flags sorted into BSON order");
    a.eq(hex(BSONEncode({ a: /a/gy })), "0b0000000b610061000000", "g and y have no BSON option");
    a.eq(hex(BSONEncode({ a: new BSONRegExp("x", "lx") })), "0d0000000b610078006c780000");
    a.eq(hex(BSONEncode({ x: new Uint8Array([255, 255]) })), "0f0000000578000200000000ffff00");
    a.eq(hex(BSONEncode({ x: new Uint16Array([0xffff]) })), "0f0000000578000200000000ffff00", "any view is its bytes");
    a.eq(hex(BSONEncode({ x: new Binary(new Uint8Array([255, 255]), 4) })), "0f0000000578000200000004ffff00");
    a.eq(hex(BSONEncode({ x: new Binary(new Uint8Array([255, 255]), 2) })), "13000000057800060000000202000000ffff00", "old subtype 2 writes its inner length");
    a.eq(hex(BSONEncode({ a: new MinKey() })), "08000000ff610000");
    a.eq(hex(BSONEncode({ a: new MaxKey() })), "080000007f610000");
    a.eq(hex(BSONEncode({ a: new Code("abc") })), "100000000d6100040000006162630000");
    a.eq(hex(BSONEncode({ d: new Decimal128("1E+3") })), "18000000136400" + "01000000000000000000000000004630" + "00");
    a.eq(hex(BSONEncode({ d: new Decimal128("-0") })), "18000000136400" + "000000000000000000000000000040b0" + "00");
    a.eq(hex(BSONEncode({ d: new Decimal128("0.0001") })), "18000000136400" + "01000000000000000000000000003830" + "00");
});

t.test("undefined: skipped in a document, null in an array; holes are null", (a) => {
    a.deep(BSONDecode(BSONEncode({ a: undefined, b: 1 })), { b: 1 });
    a.deep(BSONDecode(BSONEncode({ a: [undefined, , 3] })), { a: [null, null, 3] });
});

t.test("strings are UTF-8; a lone surrogate becomes U+FFFD; NUL inside a string is kept", (a) => {
    a.eq(hex(BSONEncode({ s: "\ud800" })), "1000000002730004000000efbfbd0000");
    a.eq(BSONDecode(BSONEncode({ s: "a\u0000b" })).s, "a\u0000b");
    a.eq(BSONDecode(BSONEncode({ s: "😀 naïve" })).s, "😀 naïve");
    a.eq(Object.keys(BSONDecode(BSONEncode({ ["k\udc00"]: 1 })))[0], "k�", "keys too");
});

t.test("key order is insertion order; integer-like keys follow JS own-key order", (a) => {
    a.deep(Object.keys(BSONDecode(BSONEncode({ z: 1, a: 2, m: 3 }))), ["z", "a", "m"]);
    a.deep(Object.keys(BSONDecode(BSONEncode({ b: 1, 2: 2, 1: 3 }))), ["1", "2", "b"]);
});

t.test("refusals", (a) => {
    a.throws(() => BSONEncode(), TypeError);
    a.throws(() => BSONEncode(5), TypeError);
    a.throws(() => BSONEncode("x"), TypeError);
    a.throws(() => BSONEncode([1, 2]), TypeError, "top-level array");
    a.throws(() => BSONEncode(new Date()), TypeError);
    a.throws(() => BSONEncode(new Uint8Array(3)), TypeError);
    a.throws(() => BSONEncode(new ObjectId()), TypeError);
    a.throws(() => BSONEncode({ f() {} }), TypeError, "function value");
    a.throws(() => BSONEncode({ s: Symbol("x") }), TypeError);
    a.throws(() => BSONEncode({ ["a\u0000b"]: 1 }), TypeError, "NUL in a key");
    a.throws(() => BSONEncode({ r: new RegExp("a\u0000") }), TypeError, "NUL in a pattern");
    a.throws(() => BSONEncode({ d: new Date(NaN) }), RangeError);
    const cyc = {};
    cyc.self = cyc;
    a.throws(() => BSONEncode(cyc), RangeError, "a cycle hits the nesting cap");
    let deep = {};
    for (let i = 0; i < 250; i++) deep = { x: deep };
    a.throws(() => BSONEncode(deep), RangeError);
});

t.test("getters run once, in order, and a throwing getter propagates", (a) => {
    const seen = [];
    const o = { get a() { seen.push("a"); return 1; }, get b() { seen.push("b"); return 2; } };
    a.deep(BSONDecode(BSONEncode(o)), { a: 1, b: 2 });
    a.deep(seen, ["a", "b"]);
    a.throws(() => BSONEncode({ get x() { throw new SyntaxError("boom"); } }), SyntaxError);
});

function randomValue(rng, depth) {
    const k = rng.nextBounded(depth > 3 ? 12 : 14);
    switch (k) {
        case 0: return rng.nextBounded(2) ? rng.nextBounded(1000) - 500 : (rng.nextBounded(1e6) - 5e5) / 7;
        case 1: return ["", "x", "naïve", "😀", "a\u0000b", "$k", "a.b"][rng.nextBounded(7)];
        case 2: return rng.nextBounded(2) === 1;
        case 3: return null;
        case 4: return BigInt(rng.nextBounded(1 << 30)) * 1000003n - 7n;
        case 5: return new Date(rng.nextBounded(1 << 30) * 1000);
        case 6: return new ObjectId();
        case 7: return Uint8Array.from({ length: rng.nextBounded(5) }, () => rng.nextBounded(256));
        case 8: return new Timestamp({ t: rng.nextBounded(1000), i: rng.nextBounded(1000) });
        case 9: return [/ab+c/i, /^x/m, /a|b/s][rng.nextBounded(3)];
        case 10: return new Decimal128(String(rng.nextBounded(100000)) + "E-" + rng.nextBounded(30));
        case 11: return new Binary(Uint8Array.of(1, 2, 3), 5);
        case 12: return Array.from({ length: rng.nextBounded(4) }, () => randomValue(rng, depth + 1));
        default: {
            const o = {};
            for (let i = 0, n = rng.nextBounded(4); i < n; i++) o["k" + rng.nextBounded(9)] = randomValue(rng, depth + 1);
            return o;
        }
    }
}

function same(x, y) {
    if (typeof x === "bigint" || typeof x !== "object" || x === null) return Object.is(x, y) || (x === 0 && y === 0);
    if (x instanceof Date) return y instanceof Date && x.getTime() === y.getTime();
    if (x instanceof ObjectId) return y instanceof ObjectId && x.equals(y);
    if (x instanceof Uint8Array) return y instanceof Uint8Array && hex(x) === hex(y);
    if (x instanceof Timestamp) return y instanceof Timestamp && x.t === y.t && x.i === y.i;
    if (x instanceof RegExp) return y instanceof RegExp && x.source === y.source && x.flags === y.flags;
    if (x instanceof Decimal128) return y instanceof Decimal128 && String(x) === String(y);
    if (x instanceof Binary) return y instanceof Binary && x.subType === y.subType && hex(x.buffer) === hex(y.buffer);
    if (Array.isArray(x)) return Array.isArray(y) && x.length === y.length && x.every((e, i) => same(e, y[i]));
    const kx = Object.keys(x), ky = Object.keys(y);
    return kx.length === ky.length && kx.every((k, i) => ky[i] === k && same(x[k], y[k]));
}

t.test("property: decode(encode(x)) rebuilds 2000 random trees", (a) => {
    const rng = new Random(4242);
    let good = 0;
    for (let i = 0; i < 2000; i++) {
        const doc = {};
        for (let j = 0, n = 1 + rng.nextBounded(6); j < n; j++) doc["f" + j] = randomValue(rng, 0);
        const back = BSONDecode(BSONEncode(doc));
        if (same(doc, back)) good++;
        else if (good - i > -3) a.ok(false, "round trip differs at #" + i);
    }
    a.eq(good, 2000);
});

await t.run();
