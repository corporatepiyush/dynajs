// covers: dyna:serialize.MsgPackEncode
// P15 policy, opt-in since owner decision D8: every call here passes { bigint: true } (the lossy
// default is pinned in test_serialize_cbor_decode_d8_lossy_default.js and test_serialize_msg_pack_decode_d8_lossy_default.js). Numbers round-trip as Numbers (ints only within +-(2^53-1), float64 beyond),
// BigInt encodes in int64/uint64 and decodes back as BigInt beyond 2^53, a lone surrogate
// encodes as U+FFFD and encoded surrogates (ED A0..BF) decode as U+FFFD. Same for CBOR.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_msg_pack_encode_int_utf8_policy");
import * as S from "dyna:serialize";
const B = { bigint: true };
const MsgPackEncode = (v) => S.MsgPackEncode(v, B), MsgPackDecode = (b) => S.MsgPackDecode(b, B);
const CBOREncode = (v) => S.CBOREncode(v, B), CBORDecode = (b) => S.CBORDecode(b, B);

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
const show = (v) => typeof v === "bigint" ? v + "n" : typeof v === "string" ? JSON.stringify(v) : String(v);
function same(a, b, msg) { assert(Object.is(a, b), msg + " (got " + show(a) + " " + typeof a + ", want " + show(b) + " " + typeof b + ")"); }
function throwsRange(fn, msg) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    assert(e instanceof RangeError, msg + " (got " + e + ")");
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const u8 = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));
const codecs = [["msgpack", MsgPackEncode, MsgPackDecode], ["cbor", CBOREncode, CBORDecode]];

__kitS.section("bigint encodes as an int and decodes back as a BigInt beyond 2^53", () => {
    for (const [name, E, D] of codecs) {
        for (const v of [2n ** 53n + 1n, -(2n ** 53n) - 1n, 2n ** 63n - 1n, -(2n ** 63n), 2n ** 63n, 2n ** 64n - 1n])
            same(D(E(v)), v, name + " BigInt " + v);
        same(D(E(5n)), 5, name + " a safe BigInt decodes as a Number");
        same(D(E(-(2n ** 53n) + 1n)), -(2 ** 53) + 1, name + " -(2^53-1) stays a Number");
        if (name === "msgpack") {
            throwsRange(() => E(2n ** 64n), name + " 2^64 is outside uint64");
            throwsRange(() => E(-(2n ** 63n) - 1n), name + " -2^63-1 is outside int64");
        }
    }
    same(hex(MsgPackEncode(2n ** 64n - 1n)), "cfffffffffffffffff", "msgpack uint64 max is 0xcf");
    same(hex(CBOREncode(2n ** 64n - 1n)), "1bffffffffffffffff", "cbor uint64 max is major 0");
    same(hex(MsgPackEncode(-(2n ** 63n))), "d38000000000000000", "msgpack int64 min is 0xd3");
});

__kitS.section("numbers stay numbers", () => {
    for (const [name, E, D] of codecs) {
        for (const v of [2 ** 53, -(2 ** 53), 2 ** 63, -(2 ** 63), 2 ** 64, 1e300, -0, NaN, Infinity, 2 ** 53 - 1])
            same(D(E(v)), v, name + " Number " + v);
    }
    same(hex(MsgPackEncode(2 ** 63)), "cb43e0000000000000", "msgpack 2^63 is float64, no int64 cast overflow");
    same(hex(CBOREncode(2 ** 53 - 1)), "1b001fffffffffffff", "cbor 2^53-1 is still an int");
});

__kitS.section("int decode beyond 2^53", () => {
    same(MsgPackDecode(u8("cf0020000000000001")), 2n ** 53n + 1n, "msgpack u64 2^53+1");
    same(MsgPackDecode(u8("cfffffffffffffffff")), 2n ** 64n - 1n, "msgpack u64 max");
    same(MsgPackDecode(u8("d3ffdfffffffffffff")), -(2n ** 53n) - 1n, "msgpack i64 -2^53-1");
    same(MsgPackDecode(u8("cf001fffffffffffff")), 2 ** 53 - 1, "msgpack 2^53-1 is a Number");
    same(CBORDecode(u8("1b0020000000000001")), 2n ** 53n + 1n, "cbor u64 2^53+1");
    same(CBORDecode(u8("3b8000000000000000")), -(2n ** 63n) - 1n, "cbor major 1 below -2^63");
    same(CBORDecode(u8("3bffffffffffffffff")), -(2n ** 64n), "cbor major 1 -2^64");
});

__kitS.section("lone surrogates encode as U+FFFD", () => {
    for (const [name, E, D] of codecs) {
        same(D(E("\ud800x")), "�x", name + " lone high");
        same(D(E("a\udc00\ud800b")), "a��b", name + " reversed pair");
        same(D(E("😀")), "😀", name + " a real pair is kept");
    }
    same(hex(MsgPackEncode("\ud800")), "a3efbfbd", "msgpack lone bytes are EF BF BD");
});

__kitS.section("encoded surrogates decode as U+FFFD", () => {
    same(MsgPackDecode(u8("a3eda080")), "���", "msgpack ED A0 80");
    same(MsgPackDecode(u8("a3eda041")), "��A", "msgpack truncated ED A0");
    same(CBORDecode(u8("63edbfbf")), "���", "cbor ED BF BF");
    same(MsgPackDecode(u8("a3ed9fbf")), "퟿", "msgpack U+D7FF is not a surrogate");
    same(MsgPackDecode(u8("aae4b8adeda08041eda080")), "中���A���", "msgpack wide text around encoded surrogates");
    same(MsgPackDecode(u8("a6c3bc41c3a942")), "üAéB", "msgpack Latin-1 text is untouched");
});

print("test_serialize_msg_pack_encode_int_utf8_policy: " + n + " assertions, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
