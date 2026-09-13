// encoding_fix5_uvarint_overflow.js -- FIX 5: Uvarint/Varint overflow (a value
// needing > 64 bits) must throw RangeError per the documented pair-API
// contract; the old behavior leaked the codec's internal sentinel as
// [0, -11] / [0, -10] bytesRead. Probes: 2^64, 2^100, 11-byte encodings,
// and the legal boundaries that must keep working (2^63 @ 10 bytes,
// 2^64-1 @ 10 bytes roundtrip, truncation [0,0]).
import { Uvarint, Varint, PutUvarint, PutVarint } from "dyna:encoding";
var __pass = 0, __fail = 0;
function js(v) {
  return JSON.stringify(v, (_, x) => (typeof x === "bigint" ? x + "n" : x));
}
function expect(msg, got, want) {
  var gs = js(got), ws = js(want === undefined ? null : want);
  if (gs === ws) { __pass++; return; }
  __fail++;
  print("FAIL " + msg + ": got " + gs + " want " + ws);
}
function throwsRangeError(msg, fn) {
  try { fn(); } catch (e) {
    var k = (e && e.constructor && e.constructor.name) || String(e);
    if (k === "RangeError") { __pass++; return; }
    __fail++; print("FAIL " + msg + ": got " + k + ", want RangeError");
    return;
  }
  __fail++; print("FAIL " + msg + ": no throw, want RangeError");
}

function u8bytes(fillByte, count, last) {
  const a = new Uint8Array(count + 1);
  a.fill(fillByte, 0, count);
  a[count] = last;
  return a;
}

// 2^64: 9 continuation bytes + final byte 2 (bit 64) -> overflow
throwsRangeError("Uvarint(2^64 encoding)", () => Uvarint(u8bytes(0x80, 9, 0x02)));
// 2^100: bit 100 needs a 15th byte; the loop refuses at byte 10
throwsRangeError("Uvarint(2^100 encoding)", () => Uvarint(u8bytes(0x80, 14, 0x01)));
// 11 continuation bytes -> overflow
throwsRangeError("Uvarint(11 continuation bytes)",
  () => Uvarint(new Uint8Array(11).fill(0x80)));
// 9x0xFF + final 0x7F (10 bytes, final byte > 1 at the boundary) -> overflow
throwsRangeError("Uvarint(10-byte boundary overflow)",
  () => Uvarint(u8bytes(0xFF, 9, 0x7f)));
// Varint shares the contract (zigzag of an impossible >64-bit magnitude)
throwsRangeError("Varint(2^64-shape encoding)", () => Varint(u8bytes(0xFF, 9, 0x7f)));
throwsRangeError("Varint(11 continuation bytes)",
  () => Varint(new Uint8Array(11).fill(0x80)));

// legal boundaries unchanged:
// 2^63 at exactly 10 bytes is the largest unsigned roundtrip
expect("Uvarint(2^63) ok",
  Uvarint(u8bytes(0x80, 9, 0x01)), [9223372036854775808n, 10]);
// full-width roundtrip
{
  const enc = PutUvarint(18446744073709551615n);   // 2^64-1, 10 bytes
  expect("PutUvarint(2^64-1).length", enc.length, 10);
  expect("Uvarint(2^64-1) roundtrip", Uvarint(enc), [18446744073709551615n, 10]);
}
// truncation stays [0, 0] (distinct from overflow)
expect("Uvarint(truncated) -> [0,0]", Uvarint(new Uint8Array(3).fill(0x80)), [0, 0]);
expect("Varint(truncated) -> [0,0]", Varint(new Uint8Array(3).fill(0x80)), [0, 0]);
// ordinary roundtrips unchanged
expect("Uvarint(300)", Uvarint(PutUvarint(300)), [300, 2]);
expect("Varint(-2)", Varint(PutVarint(-2)), [-2, 1]);

print("SUMMARY encoding_fix5_uvarint_overflow pass=" + __pass + " fail=" + __fail + " // 11 cases");
print(__fail === 0 ? "RESULT PASS" : "RESULT FAIL");
if (__fail !== 0) throw new Error("probe failed");
