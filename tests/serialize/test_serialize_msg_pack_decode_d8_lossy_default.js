// covers: dyna:serialize.MsgPackDecode
// Owner decision D8: MsgPackDecode is lossy by default like JSON.parse (uint64/int64 beyond +-(2^53-1) decode to
// the nearest Number); { bigint: true } restores the exact BigInt. A non-object options value, an unknown key or
// a non-boolean bigint is a TypeError.
import { suite } from "../lib/kit.js";
import { MsgPackDecode, MsgPackEncode } from "dyna:serialize";

const t = suite("test_serialize_msg_pack_decode_d8_lossy_default");
const u8 = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));
const B = { bigint: true };

t.test("default: ints beyond 2^53 are rounded Numbers", (a) => {
    a.eq(MsgPackDecode(u8("cf0020000000000001")), 9007199254740992, "u64 2^53+1 -> 2^53");
    a.eq(MsgPackDecode(u8("cf0020000000000003")), 9007199254740996, "u64 2^53+3 ties to even");
    a.eq(MsgPackDecode(u8("cfffffffffffffffff")), 18446744073709551615, "u64 max");
    a.eq(MsgPackDecode(u8("d3ffdfffffffffffff")), -9007199254740992, "i64 -(2^53+1)");
    a.eq(MsgPackDecode(u8("d38000000000000000")), -9223372036854775808, "i64 min");
    a.eq(MsgPackDecode(u8("cf001fffffffffffff")), 9007199254740991, "2^53-1 exact");
    a.deep(MsgPackDecode(u8("81a161cf0020000000000001")), { a: 9007199254740992 }, "inside a map");
});

t.test("opt-in { bigint: true } is exact", (a) => {
    a.eq(MsgPackDecode(u8("cf0020000000000001"), B), 9007199254740993n);
    a.eq(MsgPackDecode(u8("cfffffffffffffffff"), B), 18446744073709551615n);
    a.eq(MsgPackDecode(u8("d3ffdfffffffffffff"), B), -9007199254740993n);
    a.eq(MsgPackDecode(u8("cf001fffffffffffff"), B), 9007199254740991, "a safe int is still a Number");
});

t.test("round trip", (a) => {
    for (const v of [2n ** 53n + 1n, -(2n ** 63n), 2n ** 64n - 1n]) {
        a.eq(MsgPackDecode(MsgPackEncode(v), B), v, "exact BigInt " + v);
        a.eq(MsgPackDecode(MsgPackEncode(v)), Number(v), "lossy BigInt " + v);
    }
    for (const v of [2 ** 60, -(2 ** 63), 2 ** 63, 2 ** 64, 1e300, -0, NaN])
        for (const o of [undefined, B])
            a.eq(MsgPackDecode(MsgPackEncode(v, o), o), v, "Number " + v + " " + JSON.stringify(o));
});

t.test("bad options are a TypeError", (a) => {
    const one = u8("01");
    a.throws(() => MsgPackDecode(one, { bigint: 0 }), TypeError, "number");
    a.throws(() => MsgPackDecode(one, 42), TypeError, "a number bag");
    a.throws(() => MsgPackDecode(one, { exact: true }), TypeError, "an unknown key");
    a.eq(MsgPackDecode(one, null), 1, "null bag");
});

await t.run();
