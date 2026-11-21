// covers: dyna:serialize.CBORDecode
// Owner decision D8 (reverses the P15 BigInt default): CBORDecode is lossy by default like JSON.parse -- an int
// beyond +-(2^53-1), a major-1 value down to -2^64 and a tag 2/3 bignum decode to a Number rounded ONCE from the
// exact integer (Number(BigInt) semantics, never a double-rounded -(arg)-1); { bigint: true } restores the exact
// BigInt. A non-object options value, an unknown key or a non-boolean bigint is a TypeError.
import { suite } from "../lib/kit.js";
import { CBORDecode, CBOREncode } from "dyna:serialize";

const t = suite("test_serialize_cbor_decode_d8_lossy_default");
const u8 = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));
const B = { bigint: true };

t.test("default: ints beyond 2^53 are rounded Numbers", (a) => {
    a.eq(CBORDecode(u8("1b0020000000000001")), 9007199254740992, "2^53+1 -> 2^53");
    a.eq(CBORDecode(u8("1b001fffffffffffff")), 9007199254740991, "2^53-1 stays exact");
    a.eq(CBORDecode(u8("1bffffffffffffffff")), 18446744073709551615, "uint64 max");
    a.eq(CBORDecode(u8("3b0020000000000001")), -9007199254740994, "-1-(2^53+1) rounds once to -(2^53+2)");
    a.eq(CBORDecode(u8("3b0020000000000000")), -9007199254740992, "-(2^53+1) ties to even");
    a.eq(CBORDecode(u8("3b7fffffffffffffff")), -9223372036854775808, "-2^63");
    a.eq(CBORDecode(u8("3b8000000000000000")), Number(-9223372036854775809n), "below -2^63");
    a.eq(CBORDecode(u8("3bffffffffffffffff")), -18446744073709551616, "-2^64");
    a.eq(CBORDecode(u8("3b8000000000000bff")), Number(-(2n ** 63n + 3072n)), "below -2^63: -1-arg rounds once (a tie to even), not (double)arg then -1");
    a.eq(CBORDecode(u8("c249010000000000000001")), Number(18446744073709551617n), "tag 2 bignum is a Number");
    a.eq(CBORDecode(u8("c349010000000000000000")), Number(-18446744073709551617n), "tag 3 bignum is a Number");
    a.deep(CBORDecode(u8("821b00200000000000013b0020000000000001")), [9007199254740992, -9007199254740994], "inside an array");
});

t.test("opt-in { bigint: true } is exact", (a) => {
    a.eq(CBORDecode(u8("1b0020000000000001"), B), 9007199254740993n);
    a.eq(CBORDecode(u8("3b0020000000000001"), B), -9007199254740994n);
    a.eq(CBORDecode(u8("3bffffffffffffffff"), B), -18446744073709551616n);
    a.eq(CBORDecode(u8("c249010000000000000001"), B), 18446744073709551617n);
    a.eq(CBORDecode(u8("1b001fffffffffffff"), B), 9007199254740991, "a safe int is still a Number");
    a.eq(CBORDecode(u8("1b0020000000000001"), { bigint: false }), 9007199254740992, "bigint: false is the default");
});

t.test("round trip", (a) => {
    for (const v of [2n ** 53n + 1n, -(2n ** 63n), 2n ** 64n - 1n, 2n ** 80n + 1n, -(2n ** 80n) - 3n]) {
        a.eq(CBORDecode(CBOREncode(v), B), v, "exact BigInt " + v);
        a.eq(CBORDecode(CBOREncode(v)), Number(v), "lossy BigInt " + v);
    }
    for (const v of [2 ** 60, -(2 ** 63), 2 ** 63, 2 ** 64, 1e300, -0, 0.5]) {
        a.eq(CBORDecode(CBOREncode(v)), v, "Number " + v);
        a.eq(CBORDecode(CBOREncode(v, B), B), v, "Number " + v + " in bigint mode");
    }
});

t.test("bad options are a TypeError", (a) => {
    const one = u8("01");
    a.throws(() => CBORDecode(one, { bigint: 1 }), TypeError, "number");
    a.throws(() => CBORDecode(one, { bigint: "true" }), TypeError, "string");
    a.throws(() => CBORDecode(one, { bigint: null }), TypeError, "null");
    a.throws(() => CBORDecode(one, "bigint"), TypeError, "a string bag");
    a.throws(() => CBORDecode(one, true), TypeError, "a boolean bag");
    a.throws(() => CBORDecode(one, { bigInt: true }), TypeError, "an unknown key");
    a.eq(CBORDecode(one, undefined), 1, "undefined bag");
    a.eq(CBORDecode(one, null), 1, "null bag");
    a.eq(CBORDecode(one, {}), 1, "empty bag");
});

await t.run();
