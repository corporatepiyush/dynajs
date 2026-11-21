// covers: dyna:serialize.Decimal128
// Decimal128 string <-> BID conversions against the BSON corpus' decimal128-1.json canonical strings
// (github.com/mongodb/specifications, source/bson-corpus/tests): parse, re-print, exact bytes, refusals.
import { suite } from "../lib/kit.js";
import { Decimal128, BSONEncode, BSONDecode } from "dyna:serialize";

const t = suite("serialize.Decimal128");
const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
const bid = (s) => hex(BSONEncode({ d: new Decimal128(s) })).slice(14, 46);

t.test("canonical strings print back unchanged", (a) => {
    for (const s of ["0", "-0", "1", "-1", "0.1", "0.001234", "123456789012", "0.00001234567890123456789012345678901234",
        "1E+3", "1.000000000000000000000000000000000E+6144", "1E-6176", "-1E-6176", "Infinity", "-Infinity", "NaN",
        "9.999999999999999999999999999999999E+6144", "1.23E-7", "0E-6176", "0E+6111", "12345678901234567890123456789012.34"])
        a.eq(String(new Decimal128(s)), s, s);
});

t.test("non-canonical input normalises as the specification says", (a) => {
    a.eq(String(new Decimal128("+1")), "1");
    a.eq(String(new Decimal128("1e3")), "1E+3");
    a.eq(String(new Decimal128("0.0000001")), "1E-7");
    a.eq(String(new Decimal128("inf")), "Infinity");
    a.eq(String(new Decimal128("1.50")), "1.50", "trailing zeros are significant");
    a.eq(String(new Decimal128("1E+6144")), "1.000000000000000000000000000000000E+6144", "clamped by padding the coefficient");
    a.eq(String(new Decimal128("10E-6177")), "1E-6176", "clamped by dropping a trailing zero");
});

t.test("exact BID bytes", (a) => {
    a.eq(bid("1"), "01000000000000000000000000004030");
    a.eq(bid("-1"), "010000000000000000000000000040b0");
    a.eq(bid("0.1"), "01000000000000000000000000003e30");
    a.eq(bid("Infinity"), "00000000000000000000000000000078");
    a.eq(bid("NaN"), "0000000000000000000000000000007c");
    a.eq(bid("9.999999999999999999999999999999999E+6144"), "ffffffff638e8d37c087adbe09edff5f");
});

t.test("a non-canonical coefficient (above 10^34 - 1) reads as zero, as the specification requires", (a) => {
    const raw = Uint8Array.from([24, 0, 0, 0, 0x13, 0x64, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0x41, 0x30, 0]);
    a.eq(String(BSONDecode(raw).d), "0");
});

t.test("refusals", (a) => {
    a.throws(() => new Decimal128("1.2.3"), TypeError);
    a.throws(() => new Decimal128("abc"), TypeError);
    a.throws(() => new Decimal128(""), TypeError);
    a.throws(() => new Decimal128(5), TypeError);
    a.throws(() => new Decimal128("1E"), TypeError);
    a.throws(() => new Decimal128("12345678901234567890123456789012345"), RangeError, "35 significant digits");
    a.throws(() => new Decimal128("1E+7000"), RangeError);
    a.throws(() => new Decimal128("1E-7000"), RangeError);
});

t.test("toNumber, toJSON and the BSON round trip", (a) => {
    const d = new Decimal128("-12.5");
    a.eq(d.toNumber(), -12.5);
    a.eq(JSON.stringify(d), '{"$numberDecimal":"-12.5"}');
    a.eq(String(BSONDecode(BSONEncode({ d })).d), "-12.5");
});

await t.run();
