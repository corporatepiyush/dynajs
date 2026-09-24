import { PutUvarint, Uvarint } from "dyna:encoding";
import { eq as EQ, ok as OK, throws as TH, done as DONE } from "./harness.js";

function enc(v) { return Array.from(PutUvarint(v)); }
// basics round-trip (Numbers)
for (const v of [0, 1, 127, 128, 300, 16384, 4294967295, 9007199254740991]) {
  const e = PutUvarint(v);
  const [dec, n] = Uvarint(e);
  EQ(dec, v, "roundtrip " + v);
  EQ(n, e.length, "bytesRead " + v);
}
EQ(enc(0), [0], "0 -> single zero byte");
EQ(enc(300), [0xAC, 0x02], "300 canonical LEB128");
EQ(enc(2n**53n), [128,128,128,128,128,128,128,16], "2^53 as BigInt -> 8 bytes");
// boundary: Number path caps at 2^53-1
TH(function () { PutUvarint(9007199254740992); }, "RangeError", "2^53 as Number throws");
TH(function () { PutUvarint(-1); }, "RangeError", "negative Number throws");
TH(function () { PutUvarint(1.5); }, "RangeError", "fractional Number throws");
// BigInt path: full uvarint range now honored (was mod-2^64 silent wrap)
EQ(enc(2n**64n - 1n), [255,255,255,255,255,255,255,255,255,1], "2^64-1 -> 10 bytes");
{
  const [d64, n64] = Uvarint(PutUvarint(2n**64n - 1n));
  OK(d64 === 2n**64n - 1n, "2^64-1 roundtrips exactly (typeof " + typeof d64 + ")");
  EQ(n64, 10, "2^64-1 bytesRead");
}
EQ(PutUvarint(0n).length, 1, "0n");
EQ(PutUvarint(-0n).length, 1, "-0n is 0n");
// previously-silent-wrap values must now THROW RangeError
TH(function () { PutUvarint(2n**64n); }, "RangeError", "2^64 BigInt throws (was [0x00])");
TH(function () { PutUvarint(2n**64n + 1n); }, "RangeError", "2^64+1 BigInt throws");
TH(function () { PutUvarint(-1n); }, "RangeError", "-1n BigInt throws (was ten 0xFF)");
TH(function () { PutUvarint(-(2n**100n)); }, "RangeError", "huge negative throws");
TH(function () { PutUvarint(2n**1000n); }, "RangeError", "2^1000 throws");
// sweep: every power of two up to 2^63 round-trips exactly
let sweepOK = true;
for (let k = 0n; k <= 63n; k++) {
  const v = 2n ** k;
  const [d] = Uvarint(PutUvarint(v));
  if (BigInt(d) !== v) { sweepOK = false; OK(false, "2^" + k + " roundtrip got " + d); }
}
OK(sweepOK, "powers of two 2^0..2^63 roundtrip swept");
DONE("p06_encoding_putuvarint");
