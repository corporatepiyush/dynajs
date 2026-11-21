// covers: dyna:serialize.CBOREncode
// Owner decision D8 for the encoders: by default an integral Number in [-2^63, 2^64) is written as an int, as before
// policy P15 (1b10.. for 2^60), so the bytes and ValueHash match the pre-P15 ones; 2^63..2^64 is uint64, with the
// range checked before any cast (the old build saturated 2^63 to INT64_MAX: 1b7fff..ff, a wrong value).
// { bigint: true } keeps the P15 form: only +-(2^53-1) is an int, the rest float64, so a bigint-mode decoder can
// tell the Number from the equal BigInt. Lone surrogates encode as U+FFFD in both modes (WTF-8 ED A0 80 would not
// be valid CBOR text). MsgPackEncode and CBORCanonical follow the same split.
import { suite } from "../lib/kit.js";
import { CBOREncode, CBORCanonical, MsgPackEncode } from "dyna:serialize";

const t = suite("test_serialize_cbor_encode_d8_int_bytes");
const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
const B = { bigint: true };

t.test("default bytes are ints (pre-P15)", (a) => {
    a.eq(hex(CBOREncode(2 ** 53)), "1b0020000000000000");
    a.eq(hex(CBOREncode(2 ** 60)), "1b1000000000000000");
    a.eq(hex(CBOREncode(2 ** 63 - 1024)), "1b7ffffffffffffc00");
    a.eq(hex(CBOREncode(2 ** 63)), "1b8000000000000000", "2^63 exact, no saturating cast");
    a.eq(hex(CBOREncode(2 ** 64 - 2048)), "1bfffffffffffff800", "largest double below 2^64");
    a.eq(hex(CBOREncode(2 ** 64)), "fb43f0000000000000", "2^64 has no int: float64");
    a.eq(hex(CBOREncode(-(2 ** 53) - 2)), "3b0020000000000001");
    a.eq(hex(CBOREncode(-(2 ** 63))), "3b7fffffffffffffff", "-2^63");
    a.eq(hex(CBOREncode(-(2 ** 64))), "fbc3f0000000000000", "-2^64 stays float64");
    a.eq(hex(CBOREncode(-0)), "fb8000000000000000", "-0 stays float64");
    a.eq(hex(CBORCanonical({ a: 2 ** 60 })), "a161611b1000000000000000", "canonical");
    a.eq(hex(MsgPackEncode(2 ** 60)), "cf1000000000000000", "msgpack uint64");
    a.eq(hex(MsgPackEncode(2 ** 63)), "cf8000000000000000", "msgpack 2^63");
    a.eq(hex(MsgPackEncode(-(2 ** 63))), "d38000000000000000", "msgpack int64 min");
    a.eq(hex(MsgPackEncode(2 ** 64)), "cb43f0000000000000", "msgpack 2^64 float64");
});

t.test("{ bigint: true } bytes are float64 beyond 2^53", (a) => {
    a.eq(hex(CBOREncode(2 ** 53 - 1, B)), "1b001fffffffffffff", "2^53-1 is an int");
    a.eq(hex(CBOREncode(-(2 ** 53) + 1, B)), "3b001ffffffffffffe", "-(2^53-1) is an int");
    a.eq(hex(CBOREncode(2 ** 53, B)), "fb4340000000000000");
    a.eq(hex(CBOREncode(2 ** 60, B)), "fb43b0000000000000");
    a.eq(hex(CBOREncode(-(2 ** 53), B)), "fbc340000000000000");
    a.eq(hex(MsgPackEncode(2 ** 63, B)), "cb43e0000000000000");
    a.eq(hex(CBORCanonical(2 ** 60, B)), "fb43b0000000000000");
    a.eq(hex(CBOREncode(2n ** 60n, B)), "1b1000000000000000", "a BigInt is an int in both modes");
    a.eq(hex(CBOREncode(2n ** 60n)), "1b1000000000000000");
});

t.test("lone surrogates are U+FFFD in both modes", (a) => {
    a.eq(hex(CBOREncode("\ud800")), "63efbfbd");
    a.eq(hex(CBOREncode("\ud800", B)), "63efbfbd");
    a.eq(hex(MsgPackEncode("\udc00")), "a3efbfbd");
});

t.test("bad options are a TypeError", (a) => {
    a.throws(() => CBOREncode(1, { bigint: "yes" }), TypeError);
    a.throws(() => MsgPackEncode(1, { bigint: 1 }), TypeError);
    a.throws(() => CBORCanonical(1, "x"), TypeError);
    a.throws(() => CBOREncode(1, { sorted: true }), TypeError, "unknown key");
});

await t.run();
