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

throwsRangeError("Uvarint(2^64 encoding)", () => Uvarint(u8bytes(0x80, 9, 0x02)));
throwsRangeError("Uvarint(2^100 encoding)", () => Uvarint(u8bytes(0x80, 14, 0x01)));
throwsRangeError("Uvarint(11 continuation bytes)",
  () => Uvarint(new Uint8Array(11).fill(0x80)));
throwsRangeError("Uvarint(10-byte boundary overflow)",
  () => Uvarint(u8bytes(0xFF, 9, 0x7f)));
throwsRangeError("Varint(2^64-shape encoding)", () => Varint(u8bytes(0xFF, 9, 0x7f)));
throwsRangeError("Varint(11 continuation bytes)",
  () => Varint(new Uint8Array(11).fill(0x80)));

expect("Uvarint(2^63) ok",
  Uvarint(u8bytes(0x80, 9, 0x01)), [9223372036854775808n, 10]);
{
  const enc = PutUvarint(18446744073709551615n);
  expect("PutUvarint(2^64-1).length", enc.length, 10);
  expect("Uvarint(2^64-1) roundtrip", Uvarint(enc), [18446744073709551615n, 10]);
}
expect("Uvarint(truncated) -> [0,0]", Uvarint(new Uint8Array(3).fill(0x80)), [0, 0]);
expect("Varint(truncated) -> [0,0]", Varint(new Uint8Array(3).fill(0x80)), [0, 0]);
expect("Uvarint(300)", Uvarint(PutUvarint(300)), [300, 2]);
expect("Varint(-2)", Varint(PutVarint(-2)), [-2, 1]);

print("SUMMARY encoding_fix5_uvarint_overflow pass=" + __pass + " fail=" + __fail + " // 11 cases");
print(__fail === 0 ? "RESULT PASS" : "RESULT FAIL");
if (__fail !== 0) throw new Error("probe failed");
