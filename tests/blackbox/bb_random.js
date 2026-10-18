// Parametric black-box contract test for dyna:random, generated from dynajs.d.ts lines 4036-4109. Engine sources not consulted.
import { Random } from "dyna:random";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
const draws = (r, f, k) => { const out = []; for (let i = 0; i < k; i++) out.push(f(r)); return out; };
const snapshot = (r) => new Uint8Array(r.getState());

// ============================ constructor seed coercion (d.ts L4044-4047) ============================
// "seed is coerced via ToInt64 ... '42' gives the 42 stream, 'pigs'/null/{} coerce to 0, 42n gives the same stream as 42 ... a Symbol throws"
{
  const cases = [
    ["seed 42 (number door)", () => new Random(42)],
    ["seed 42n (bigint door)", () => new Random(42n)],
    ["seed '42' (string door)", () => new Random("42")],
  ];
  const seqs = [];
  for (const [label, make] of cases) {
    const r = make();
    seqs.push(draws(r, (g) => g.nextFloat(), 8));
  }
  for (let i = 1; i < seqs.length; i++) assert(eqArr(seqs[0], seqs[i]), cases[i][0] + " matches the 42 stream");
  const zeroDoors = [
    ["seed 'pigs' -> 0 stream", () => new Random("pigs")],
    ["seed null -> 0 stream", () => new Random(null)],
    ["seed {} -> 0 stream", () => new Random({})],
    ["seed 0", () => new Random(0)],
  ];
  const zseqs = zeroDoors.map(([, make]) => draws(make(), (g) => g.nextFloat(), 8));
  for (let i = 1; i < zseqs.length; i++) assert(eqArr(zseqs[0], zseqs[i]), zeroDoors[i][0]);
  assertThrows(() => new Random(Symbol("s")), "non-coercible seed (Symbol) throws");
  const entropy = new Random();
  assert(typeof entropy.nextU64() === "bigint", "omitted seed draws from OS entropy but nextU64 stays bigint");
}

// ============================ seeded reproducibility / stream separation ============================
// "A seedable xoshiro256** PRNG; a given seed is deterministic and reproducible."
{
  const cases = [
    ["seed 7 vs 7 identical", () => new Random(7), () => new Random(7), true],
    ["seed 7 vs 8 different", () => new Random(7), () => new Random(8), false],
  ];
  for (const [label, mkA, mkB, same] of cases) {
    const a = draws(mkA(), (g) => g.nextU64(), 4);
    const b = draws(mkB(), (g) => g.nextU64(), 4);
    assert(eqArr(a, b) === same, label);
  }
  // reproducibility per documented distribution (same seed -> same sequence)
  const distCases = [
    ["nextU53 reproducible", (g) => g.nextU53()],
    ["nextFloat reproducible", (g) => g.nextFloat()],
    ["nextBounded(9) reproducible", (g) => g.nextBounded(9)],
    ["normal() reproducible", (g) => g.normal()],
    ["exponential() reproducible", (g) => g.exponential()],
    ["poisson(5) reproducible", (g) => g.poisson(5)],
  ];
  for (const [label, f] of distCases) {
    const a = draws(new Random(31), f, 6);
    const b = draws(new Random(31), f, 6);
    assert(eqArr(a, b), label);
  }
}

// ============================ nextU64 / nextU53 / nextFloat (d.ts L4048-4053) ============================
{
  const r = new Random(1234);
  const u64s = draws(r, (g) => g.nextU64(), 50);
  for (const [i, v] of u64s.entries()) {
    assert(typeof v === "bigint" && 0n <= v && v < 4294967296n * 4294967296n, "nextU64 draw " + i + " is a bigint in [0, 2^64)");
  }
  const u53s = draws(r, (g) => g.nextU53(), 50);
  for (const [i, v] of u53s.entries()) {
    assert(Number.isInteger(v) && v >= 0 && v < Math.pow(2, 53), "nextU53 draw " + i + " is an exact integer in [0, 2^53)");
  }
  const fls = draws(r, (g) => g.nextFloat(), 100);
  for (const [i, v] of fls.entries()) {
    assert(typeof v === "number" && v >= 0 && v < 1, "nextFloat draw " + i + " is in [0, 1)");
  }
}

// ============================ nextBounded (d.ts L4054-4059) ============================
// "Uniform in [0, bound); Number bound must be an integer in [1, 2^53]; a BigInt bound may span the full u64 range ... result type mirrors the argument"
{
  const r = new Random(5);
  const ones = draws(r, (g) => g.nextBounded(1), 20);
  for (const [i, v] of ones.entries()) assertEq(v, 0, "nextBounded(1) draw " + i + " is 0 (only value in [0,1))");
  const twos = draws(new Random(6), (g) => g.nextBounded(2), 50);
  for (const [i, v] of twos.entries()) assert(v === 0 || v === 1, "nextBounded(2) draw " + i + " in {0,1}");
  const big = new Random(7).nextBounded(Math.pow(2, 53)); // cap is inclusive
  assert(Number.isInteger(big) && big >= 0 && big < Math.pow(2, 53), "nextBounded(2^53) accepted at the documented cap");
  const b = new Random(8).nextBounded(10n);
  assert(typeof b === "bigint" && 0n <= b && b < 10n, "nextBounded(10n) mirrors the bigint type");
  const refusals = [
    ["nextBounded(0) below [1,2^53]", () => new Random(1).nextBounded(0)],
    ["nextBounded(-3) below [1,2^53]", () => new Random(1).nextBounded(-3)],
    ["nextBounded(1.5) non-integer", () => new Random(1).nextBounded(1.5)],
    // NB: Math.pow(2,53)+1 rounds to exactly 2^53 as a double (inside the
    // inclusive cap, correctly accepted above) -- go clearly past it.
    ["nextBounded(2^53*2) above the cap", () => new Random(1).nextBounded(Math.pow(2, 53) * 2)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ fill (d.ts L4060-4065) ============================
// "Fills any byte-width typed array ... returns this ... DataView or bare ArrayBuffer refused (TypeError) ...
//  window in ELEMENTS ... out-of-bounds windows throw RangeError ... a refused call consumes no draws."
{
  const r = new Random(11);
  const u = new Uint16Array(4);
  u[0] = 7; u[3] = 9;
  assertEq(r.fill(u, 1, 2), r, "fill returns this");
  assertEq(u[0], 7, "fill window: element 0 outside the window untouched");
  assertEq(u[3], 9, "fill window: last element outside the window untouched");
  const full = new Uint8Array(8);
  // doc: fill "returns this" (the generator), so the ARRAY's contents are the
  // subject, not the return value; byte values are pinned via seeded
  // determinism (same seed -> same bytes), not literals.
  const g12 = new Random(12);
  assertEq(g12.fill(full), g12, "fill defaults to the whole array and returns this");
  const twin = new Uint8Array(8);
  new Random(12).fill(twin);
  assert(eqArr(Array.from(full), Array.from(twin)),
      "fill is seed-deterministic (same seed, same bytes)");
  const rangeCases = [
    ["fill offset 3 length 5 on length-4 array", () => new Random(1).fill(new Uint8Array(4), 3, 5)],
    ["fill offset -1", () => new Random(1).fill(new Uint8Array(4), -1, 2)],
    ["fill length beyond the rest", () => new Random(1).fill(new Uint8Array(4), 0, 5)],
  ];
  for (const [label, fn] of rangeCases) assertThrows(fn, label, RangeError);
  const refuseCases = [
    ["fill DataView refused", () => new Random(1).fill(new DataView(new ArrayBuffer(8)))],
    ["fill bare ArrayBuffer refused", () => new Random(1).fill(new ArrayBuffer(8))],
  ];
  for (const [label, fn] of refuseCases) assertThrows(fn, label, TypeError);
  const r2 = new Random(13);
  const s1 = snapshot(r2);
  assertThrows(() => r2.fill(new DataView(new ArrayBuffer(8))), "refused fill throws");
  assert(eqArr(snapshot(r2), s1), "a refused fill consumes no draws (state unchanged)"); // documented verbatim
}

// ============================ bytes (d.ts L4066-4069) ============================
// "n fresh random bytes as a new Uint8Array; n must be an integer in [0, 2^30] ... bytes(0) consumes no draws."
{
  const r = new Random(21);
  const b8 = r.bytes(8);
  assert(b8 instanceof Uint8Array && b8.length === 8, "bytes(8) is a fresh Uint8Array of length 8");
  const s1 = snapshot(r);
  assertEq(r.bytes(0).length, 0, "bytes(0) is empty");
  assert(eqArr(snapshot(r), s1), "bytes(0) consumes no draws (state unchanged)");
  const refusals = [
    ["bytes(-1)", () => new Random(1).bytes(-1)],
    ["bytes(1.5)", () => new Random(1).bytes(1.5)],
    ["bytes(2^30 + 1) above the cap", () => new Random(1).bytes(Math.pow(2, 30) + 1)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ normal (d.ts L4070-4075) ============================
// "sigma must be finite >= 0 (sigma = 0 is the point mass at mu and consumes no draws)."
{
  const r = new Random(41);
  const zeros = draws(r, (g) => g.normal(5, 0), 5);
  for (const [i, v] of zeros.entries()) assertEq(v, 5, "normal(5, 0) draw " + i + " is the point mass 5");
  const s1 = snapshot(r);
  r.normal(5, 0);
  assert(eqArr(snapshot(r), s1), "sigma = 0 consumes no draws (state unchanged)");
  const refusals = [
    ["normal sigma -1 (must be >= 0)", () => new Random(1).normal(0, -1)],
    ["normal sigma Infinity (must be finite)", () => new Random(1).normal(0, Infinity)],
    ["normal sigma NaN", () => new Random(1).normal(0, NaN)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
  const d = draws(new Random(42), (g) => g.normal(), 200); // defaults mu=0, sigma=1
  for (const [i, v] of d.entries()) {
    assert(Number.isFinite(v) && Math.abs(v) < 1e6, "normal() default draw " + i + " is finite");
  }
}

// ============================ exponential (d.ts L4076-4078) ============================
// "lambda must be a finite number > 0, default 1."
{
  const refusals = [
    ["exponential(0)", () => new Random(1).exponential(0)],
    ["exponential(-1)", () => new Random(1).exponential(-1)],
    ["exponential(Infinity)", () => new Random(1).exponential(Infinity)],
    ["exponential(NaN)", () => new Random(1).exponential(NaN)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
  const d = draws(new Random(51), (g) => g.exponential(), 100);
  for (const [i, v] of d.entries()) {
    assert(typeof v === "number" && v >= 0, "exponential() draw " + i + " is non-negative (inverse CDF of u in [0,1))");
  }
  const d2 = draws(new Random(52), (g) => g.exponential(2.5), 10);
  for (const [i, v] of d2.entries()) assert(v >= 0, "exponential(2.5) draw " + i + " non-negative");
}

// ============================ poisson (d.ts L4079-4083) ============================
// "Returns a non-negative integer; lambda is required and outside [0, 2^31-1] throws RangeError."
{
  assertEq(new Random(61).poisson(0), 0, "poisson(0) is the point mass at 0 (lambda 0 is in range)");
  const d = draws(new Random(62), (g) => g.poisson(3), 20);
  for (const [i, v] of d.entries()) {
    assert(Number.isInteger(v) && v >= 0, "poisson(3) draw " + i + " is a non-negative integer");
  }
  const refusals = [
    ["poisson(-1) outside [0, 2^31-1]", () => new Random(1).poisson(-1), RangeError],
    ["poisson(2^31) outside [0, 2^31-1]", () => new Random(1).poisson(Math.pow(2, 31)), RangeError],
    // The doc's RangeError sentence is about the RANGE; a MISSING lambda is a
    // required-argument violation -> TypeError (engine).
    ["poisson() lambda required", () => new Random(1).poisson(), TypeError],
  ];
  for (const [label, fn, T] of refusals) assertThrows(fn, label, T);
}

// ============================ shuffle (d.ts L4084-4089) ============================
// "In-place Fisher-Yates over the INSTANCE stream; returns this ... holes move as holes ...
//  frozen/sealed collections are refused with a TypeError. Exactly len-1 draws for len >= 2."
{
  const r = new Random(71);
  const arr = [3, 1, 2];
  assertEq(r.shuffle(arr), r, "shuffle returns this");
  assert(eqArr([...arr].sort(), [1, 2, 3]), "shuffle preserves the multiset of elements");
  const ta = Int32Array.from([4, 2, 6]);
  r.shuffle(ta);
  assert(eqArr(Array.from(ta).sort(), [2, 4, 6]), "shuffle accepts typed arrays");
  const holey = [1, , 3];
  r.shuffle(holey);
  let holes = 0;
  for (let i = 0; i < 3; i++) if (!(i in holey)) holes++;
  assertEq(holes, 1, "shuffle moves holes as holes (never densifies)");
  assert(eqArr(holey.filter((x) => x !== undefined).sort(), [1, 3]), "shuffle keeps the defined values {1,3}");
  assertThrows(() => new Random(1).shuffle(Object.freeze([1, 2, 3])), "shuffle refuses frozen arrays", TypeError);
  // draw-count parity: shuffle of len 4 consumes exactly 3 draws of the instance stream
  const rA = new Random(123);
  rA.shuffle([1, 2, 3, 4]);
  const rB = new Random(123);
  rB.nextU64(); rB.nextU64(); rB.nextU64();
  assert(eqArr(snapshot(rA), snapshot(rB)), "shuffle of len 4 consumes exactly len-1 = 3 draws");
  const rC = new Random(77);
  const sC = snapshot(rC);
  rC.shuffle([9]);
  assert(eqArr(snapshot(rC), sC), "shuffle of len 1 consumes 0 draws");
  rC.shuffle([]);
  assert(eqArr(snapshot(rC), sC), "shuffle of len 0 consumes 0 draws");
}

// ============================ sample (d.ts L4090-4093) ============================
// "n elements drawn WITHOUT replacement ... the source is never mutated, exactly n draws.
//  n must be an integer in [0, len]. A typed array yields its own type."
{
  const src = [10, 20, 30, 40];
  const r = new Random(81);
  const s = r.sample(src, 2);
  assertEq(s.length, 2, "sample length is n");
  assert(eqArr(src, [10, 20, 30, 40]), "sample never mutates the source");
  assert(new Set(s).size === 2, "sample draws without replacement (all distinct)");
  for (const v of s) assert(src.includes(v), "sample element " + v + " comes from the source");
  assert(eqArr(r.sample(src, 0), []), "sample n=0 is empty");
  assert(eqArr([...r.sample(src, 4)].sort(), [10, 20, 30, 40]), "sample n=len is a permutation of the source");
  const ts = r.sample(Int32Array.from([5, 6, 7]), 2);
  assert(ts instanceof Int32Array, "sample of a typed array yields its own type");
  assertEq(ts.length, 2, "typed-array sample length is n");
  const refusals = [
    ["sample n = len+1", () => new Random(1).sample([1, 2], 3)],
    ["sample n = -1", () => new Random(1).sample([1, 2], -1)],
    ["sample n = 1.5", () => new Random(1).sample([1, 2], 1.5)],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ choice (d.ts L4094-4096) ============================
// "One uniform element ... an empty array is a RangeError."
{
  assertEq(new Random(91).choice([7]), 7, "choice of a single-element array returns that element");
  assertEq(new Random(92).choice(Int32Array.from([7])), 7, "choice accepts typed arrays");
  const r = new Random(93);
  for (let i = 0; i < 20; i++) {
    const v = r.choice([1, 2, 3]);
    assert(v === 1 || v === 2 || v === 3, "choice draw " + i + " is one of the source elements");
  }
  assertThrows(() => new Random(1).choice([]), "choice of an empty array is a RangeError", RangeError);
}

// ============================ jump / longJump (d.ts L4097-4103) ============================
// "Two same-seed generators separated by a jump produce disjoint streams ... longJump: the 2^192-advance variant."
{
  const r = new Random(101);
  assertEq(r.jump(), r, "jump returns this (mutates in place)");
  const rL = new Random(102);
  assertEq(rL.longJump(), rL, "longJump returns this");
  const j1 = new Random(103).jump().nextU64();
  const j2 = new Random(103).jump().nextU64();
  assertEq(j1, j2, "jump is a deterministic state transform (same seed -> same post-jump draw)");
  const l1 = new Random(104).longJump().nextU64();
  const l2 = new Random(104).longJump().nextU64();
  assertEq(l1, l2, "longJump is deterministic too");
  const a = new Random(9);
  const b = new Random(9);
  b.jump();
  assert(!Object.is(a.nextU64(), b.nextU64()), "jump separates same-seed generators into disjoint streams");
  const c = new Random(9).jump();
  const d = new Random(9).longJump();
  assert(!Object.is(c.nextU64(), d.nextU64()), "longJump lane differs from the jump lane (2^64 independent lanes)");
}

// ============================ getState / setState (d.ts L4104-4107) ============================
// "A 32-byte opaque snapshot of the generator state (four LE u64 words) ... an all-zero state is the xoshiro fixed point and is refused."
{
  const r = new Random(55);
  const s = r.getState();
  assert(s instanceof Uint8Array && s.length === 32, "getState is a 32-byte Uint8Array snapshot");
  const v2 = r.nextU64();
  const fresh = new Random(0); // any generator: setState fully replaces the state
  fresh.setState(s);
  assertEq(fresh.nextU64(), v2, "setState restores the captured sequence exactly (round-trip)");
  const r2 = new Random(56);
  r2.nextU64();                 // advance once
  const mid = snapshot(r2);     // capture mid-stream state
  const vNext = r2.nextU64();   // the draw that follows mid
  r2.setState(mid);             // rewind the same instance
  assertEq(r2.nextU64(), vNext, "setState on the same instance resumes its own sequence");
  assertThrows(() => new Random(1).setState(new Uint8Array(32)), "the all-zero state (xoshiro fixed point) is refused");
}

print("bb_random: all tests passed (" + n + " assertions)");
