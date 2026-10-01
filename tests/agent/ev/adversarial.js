function horner(s, r) {
  let v = 0n; const R = BigInt(r); let neg = false;
  for (const c of s) { if (c === "-") { neg = true; continue; } v = v * R + BigInt(parseInt(c, r)); }
  return neg ? -v : v;
}
let fails = 0, checks = 0;
function emit(s) { if (typeof print === "function") print(s); else console.log(s); }
function T(tag, v, radices) {
  checks++;
  for (const r of radices) {
    const s = v.toString(r);
    if (horner(s, r) !== v) { emit("FAIL " + tag + " roundtrip radix " + r); fails++; }
  }
  if (horner(v.toString(16), 16) !== v) { emit("FAIL " + tag + " hex horner"); fails++; }
}
function M(tag, a, b) {
  checks++;
  const p = a * b;
  const ok = (a !== 0n && p / a === b && p / b === a) || (a === 0n && p === 0n);
  if (!ok) { emit("FAIL " + tag + " divcheck"); fails++; }
  const r = p % a;
  if (a !== 0n && a * (p / a) + r !== p) { emit("FAIL " + tag + " mulmod"); fails++; }
  T(tag + ".p", p, [10, 16, 36, 3, 17]);
}
function A(tag, v) {
  checks++;
  for (const k of [1, 31, 32, 63, 64, 65, 127, 128, 1024, 8192]) {
    const u = BigInt.asUintN(k, v), s = BigInt.asIntN(k, v);
    if (BigInt.asUintN(k, s) !== u || BigInt.asUintN(k, BigInt.asIntN(k, u)) !== u) { emit("FAIL " + tag + " asN " + k); fails++; }
  }
}

const L = 64n;
for (const k of [1, 2, 3, 31, 32, 33, 63, 64, 65, 100, 128, 200, 333, 1000]) {
  const ff = (1n << (L * BigInt(k))) - 1n;
  const g = (1n << (L * BigInt(k))) - 2n;
  T("ff" + k, ff, [10, 16, 36]);
  T("-ff" + k, -ff, [10, 16, 36]);
  M("ffXg" + k, ff, g);
  M("ffX1" + k, ff, 1n);
  M("ffXff" + k + "b", ff, (1n << (L * BigInt(k - 1 || 1))) - 1n);
  M("negffXff", -ff, ff);
  A("ffA" + k, ff);
}
const edges = [1n << 31n, (1n << 32n) - 1n, 1n << 32n, 1n << 63n, (1n << 64n) - 1n, 1n << 64n,
  (1n << 64n) + 1n, -((1n << 63n) - 1n), -(1n << 63n), (1n << 127n), ((1n << 128n) - 1n)];
for (const e of edges) for (const f of edges) M("edge", e, f);
function mask(k) { return (1n << (L * BigInt(k))) - 1n; }
for (const nb of [31, 32, 33, 34, 64, 65, 127, 128, 129]) {
  const b = mask(nb) - 12345n;
  for (const na of [2 * nb - 1, 2 * nb, 2 * nb + 1, 3 * nb, 4 * nb + 7]) {
    const a = mask(na) - 999n;
    M("shape n" + na + "x" + nb, a, b);
    M("shapeS n" + na + "x" + nb, -a, b);
    M("shapeS2 n" + na + "x" + nb, a, -b);
    M("shapeS3 n" + na + "x" + nb, -a, -b);
  }
}
for (const k of [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024]) {
  for (const top of [1, 9, 18]) {
    const nd = k * 19 - 19 + top;
    if (nd < 1) continue;
    const v = 10n ** BigInt(nd - 1) + 1234567890123456789n;
    checks++;
    const s = v.toString();
    if (horner(s, 10) !== v) { emit("FAIL pow2chunk10 k=" + k + " top=" + top); fails++; }
  }
}
function sweepRadix(r, ndlo, ndhi) {
  for (let nd = ndlo; nd <= ndhi; nd++) {
    const v = (r === 10 ? 10n : 3n) ** BigInt(nd - 1) + 7n;
    checks++;
    const s = v.toString(r);
    if (horner(s, r) !== v) { emit("FAIL sweep r=" + r + " nd=" + nd); fails++; }
  }
}
sweepRadix(3, 1560, 1600);
sweepRadix(3, 1240, 1260);
sweepRadix(17, 900, 960);
sweepRadix(10, 380, 420);
const MAXB = 16384n * L;
let maxff = 0n, maxLimbsNote = "";
function guarded(tag, fn) { checks++; try { const r = fn(); emit("OK " + tag); return r; } catch (e) { emit("LIMIT " + tag + " " + (e instanceof RangeError)); fails++; return undefined; } }
maxff = guarded("max asUintN 1M", () => BigInt.asUintN(1048576, -1n));
if (maxff === undefined) maxff = guarded("max asUintN 1M-64", () => BigInt.asUintN(1048512, -1n));
if (maxff === undefined) maxff = 0n;
{
  let x = (1n << 524288n) - 1n;
  guarded("sq 8192x8192", () => { const big = x * x; T("big_squared16", big, [16]); T("big_squared10", big, [10]); M("big_squaredXsmall", big, 0xffffffffffffffffn); return 1; });
  let y7 = (1n << 524224n) - 1n;
  guarded("sq 8191x8191", () => { const big = y7 * y7; T("big7_squared16", big, [16]); T("big7_squared10", big, [10]); M("big7_sqXsmall", big, 0xffffffffffffffffn); M("big7_sqXself", big, big); return 1; });
}
const half = (1n << (MAXB / 2n)) - 1n;
T("maxff16", maxff, [16]);
T("maxff10", maxff, [10]);
T("maxff36", maxff, [36]);
T("maxff2", maxff, [2]);
guarded("maxhalf", () => { M("maxhalf", half, half); return 1; });
M("maxffXsmall", maxff, 0xffffffffffffffffn);
M("maxffXff64", maxff, mask(64));
A("maxA", maxff);
checks++;
let threw = false;
try { (1n << (MAXB + L)) - 1n; } catch (e) { threw = (e instanceof RangeError); }
if (!threw) { emit("FAIL maxsize no RangeError"); fails++; }
for (const bits of [4200, 8200, 16000]) {
  let v = (1n << BigInt(bits)) - 1n;
  T("dt" + bits, v, [10, 3, 17, 36]);
  T("dtn" + bits, -v, [10, 3, 17, 36]);
}
emit("ADV DONE checks=" + checks + " fails=" + fails);
