// covers: dyna:serialize.CBORDecode
// RFC 8949 3.4.3: tag 2 (unsigned) and tag 3 (negative, -1 - n) bignums decode to BigInt (a
// Number when the value is a safe integer, like every other int); CBOREncode writes a BigInt
// beyond the 64-bit major-type range as the shortest tag 2/3 byte string. The magnitude is capped
// at 131072 bytes on both sides (near the engine BigInt limit, whose RangeError covers the rest);
// tag 2/3 around anything but a
// definite byte string is a SyntaxError. MessagePack has no bignum and keeps the RangeError.
// Owner decision D8: the BigInt results need { bigint: true }, so every decode here passes it; the
// lossy default (a rounded Number) is pinned in test_serialize_cbor_decode_d8_lossy_default.js and test_serialize_msg_pack_decode_d8_lossy_default.js.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_cbor_decode_bignum");
import { CBOREncode, CBORDecode as CBORDecodeRaw, CBORCanonical, MsgPackEncode } from "dyna:serialize";
const CBORDecode = (b) => CBORDecodeRaw(b, { bigint: true });

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
const show = (v) => typeof v === "bigint" ? v + "n" : String(v);
function same(a, b, msg) { assert(Object.is(a, b), msg + " (got " + show(a) + ", want " + show(b) + ")"); }
function throwsT(fn, T, msg) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    assert(e instanceof T, msg + " (got " + e + ")");
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const u8 = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));

__kitS.section("RFC 8949 appendix A vectors", () => {
    same(CBORDecode(u8("c249010000000000000000")), 18446744073709551616n, "2^64");
    same(CBORDecode(u8("c349010000000000000000")), -18446744073709551617n, "-2^64-1");
    same(hex(CBOREncode(18446744073709551616n)), "c249010000000000000000", "encode 2^64");
    same(hex(CBOREncode(-18446744073709551617n)), "c349010000000000000000", "encode -2^64-1");
});

__kitS.section("round trip and shortest form", () => {
    for (const v of [2n ** 64n, -(2n ** 64n) - 1n, 2n ** 200n + 12345n, -(2n ** 200n) - 7n, 2n ** 72n, -(2n ** 72n), (1n << 8191n) - 1n, -(1n << 8191n)])
        same(CBORDecode(CBOREncode(v)), v, "BigInt " + (v > 0n ? "+" : "-") + v.toString(16).length + " hex digits");
    same(hex(CBOREncode(2n ** 64n - 1n)), "1bffffffffffffffff", "uint64 max stays major 0");
    same(hex(CBOREncode(-(2n ** 64n))), "3bffffffffffffffff", "-2^64 stays major 1");
    same(hex(CBOREncode(-(2n ** 72n))), "c349ffffffffffffffffff", "tag 3 carries n = -1 - v");
    same(hex(CBORCanonical(2n ** 64n)), "c249010000000000000000", "canonical form");
});

__kitS.section("decode edge cases", () => {
    same(CBORDecode(u8("c24101")), 1, "a small tag 2 is a Number");
    same(CBORDecode(u8("c240")), 0, "an empty tag 2 is 0");
    same(CBORDecode(u8("c34100")), -1, "an empty-magnitude tag 3 is -1");
    same(CBORDecode(u8("c24a000000000000000000ff")), 255, "leading zero bytes are skipped");
    same(CBORDecode(u8("c248ffffffffffffffff")), 2n ** 64n - 1n, "8-byte tag 2 above 2^53");
    same(CBORDecode(u8("c348ffffffffffffffff")), -(2n ** 64n), "8-byte tag 3");
    same(CBORDecode(u8("82c24901000000000000000001")).length, 2, "inside an array");
    throwsT(() => CBORDecode(u8("c201")), SyntaxError, "tag 2 around an int");
    throwsT(() => CBORDecode(u8("c25f41004100ff")), SyntaxError, "tag 2 around an indefinite byte string");
    throwsT(() => CBORDecode(u8("c25a00020001")), RangeError, "magnitude over 131072 bytes");
    throwsT(() => CBORDecode(u8("c2490100")), Error, "truncated magnitude");
});

__kitS.section("caps and MessagePack", () => {
    const top = 1n << 1048000n;
    same(CBOREncode(top).length, 1 + 5 + 131001, "a 1048001-bit BigInt encodes as a 131001-byte magnitude");
    same(CBORDecode(CBOREncode(top)) === top, true, "and round-trips");
    const big = new Uint8Array(6 + 131072);
    big.set([0xc2, 0x5a, 0x00, 0x02, 0x00, 0x00]);
    big.fill(0xff, 6);
    throwsT(() => CBORDecode(big), RangeError, "a 131072-byte magnitude past the engine limit");
    throwsT(() => MsgPackEncode(2n ** 64n), RangeError, "MessagePack has no bignum");
});

print("test_serialize_cbor_decode_bignum: " + n + " assertions, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
