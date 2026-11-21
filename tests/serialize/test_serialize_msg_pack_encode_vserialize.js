// covers: dyna:serialize.MsgPackEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_msg_pack_encode_vserialize");
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode,
         CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
const j = (v) => JSON.stringify(v);

__kitS.section("msgpack(", () => {
    const vectors = [
        [0, "00"], [127, "7f"], [128, "cc80"], [255, "ccff"], [256, "cd0100"],
        [-1, "ff"], [-32, "e0"], [-33, "d0df"], [-128, "d080"], [-129, "d1ff7f"],
        [true, "c3"], [false, "c2"], [null, "c0"],
        ["", "a0"], ["a", "a161"], ["abc", "a3616263"],
        [[], "90"], [[1, 2, 3], "9301 0203".replace(/ /g, "")],
        [{}, "80"], [{ a: 1 }, "81a16101"],
        [1.5, "cb3ff8000000000000"],
    ];
    let bad = 0;
    for (const [v, want] of vectors) {
        const got = hex(MsgPackEncode(v));
        if (got !== want) {
            bad++;
            if (bad < 5) print("  msgpack(" + j(v) + ") = " + got + ", want " + want);
        }
    }
    assert(bad === 0, "every MessagePack vector matches the spec's type bytes ("
                      + (vectors.length - bad) + "/" + vectors.length + ")");
});

eq(hex(MsgPackEncode(new Uint8Array([1, 2, 3]))), "c403010203", "bin8 for bytes");

__kitS.section("hello world", () => {
    const corpus = [
        null, true, false, 0, 1, -1, 127, 128, -128, 65535, -65536,
        2147483647, -2147483648, 4294967296, -4294967296,
        0.5, -0.5, 3.141592653589793, 1e100, -1e-100,
        "", "a", "hello world", "é你\u{1F600}", "x".repeat(1000),
        [], [1], [1, "two", null, [3, [4]]],
        {}, { a: 1 }, { a: { b: { c: [1, 2, { d: null }] } } },
        { "": "empty key" }, { "é": 1 },
        new Uint8Array([]), new Uint8Array([0, 255, 128]),
        [[[[[[[[[[1]]]]]]]]]],
    ];
    let bad = 0, checked = 0;
    for (const v of corpus) {
        for (const [enc, dec, name] of [[MsgPackEncode, MsgPackDecode, "msgpack"],
                                        [CBOREncode, CBORDecode, "cbor"]]) {
            checked++;
            const back = dec(enc(v));
            const same = v instanceof Uint8Array
                ? (back instanceof Uint8Array && hex(back) === hex(v))
                : j(back) === j(v);
            if (!same) {
                bad++;
                if (bad < 5) print("  " + name + " round trip differs for " + j(v)
                                   + " -> " + j(back));
            }
        }
    }
    assert(bad === 0, "every value round-trips through both formats (" +
                      (checked - bad) + "/" + checked + ")");
    assert(checked === corpus.length * 2, "the corpus ran through both");
});

throws(() => MsgPackEncode(), "a value is required");

throws(() => MsgPackEncode(() => 1), "a function has no encoding");

throws(() => MsgPackEncode(Symbol("x")), "nor a symbol");

__kitS.section("a cycle is refused: no wire format has one", () => {
    const cyc = { a: 1 };
    cyc.self = cyc;
    throws(() => MsgPackEncode(cyc), "a cycle is refused: no wire format has one");
    throws(() => CBOREncode(cyc), "in CBOR too");
    const shared = { x: 1 };
    eq(j(MsgPackDecode(MsgPackEncode({ a: shared, b: shared }))),
       '{"a":{"x":1},"b":{"x":1}}', "a repeated node is not a cycle");
});

eq(hex(MsgPackEncode({ 1: "a", 0: "b", z: "c" })), "83a130a162a131a161a17aa163",
   "integer keys still sort BEFORE string keys");

__kitS.section("300 keys is a map16 header", () => {
    const big = {};
    for (let i = 0; i < 300; i++) big["k" + i] = i;
    const b = MsgPackEncode(big);
    eq(b[0], 0xde, "300 keys is a map16 header");
    eq(hex(b.slice(0, 3)) + hex(b.slice(3, 6)), "de012ca2" + "6b30", "and starts at k0");
});

__kitS.section("a symbol key is not a string key", () => {
    const sym = { a: 1 };
    sym[Symbol("s")] = 2;
    eq(hex(MsgPackEncode(sym)), "81a16101", "a symbol key is not a string key");
});
__kitS.end();

eq(hex(MsgPackEncode(Object.defineProperty({ v: 1 }, "hidden",
    { value: 2, enumerable: false }))), "81a17601", "a non-enumerable key is skipped");

eq(hex(MsgPackEncode(new Proxy({ p: 1 }, {}))), "81a17001",
   "a proxy is exotic: the general path handles it");

eq(hex(MsgPackEncode({ "\u00e9": 1 })), "81a2c3a901",
   "a non-ASCII key is converted to UTF-8, not borrowed as Latin-1");

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
