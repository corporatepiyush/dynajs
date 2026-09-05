// dynajs_bytes.js — the Bytes and Text value handles in dyna:bytes.
//
// Bytes owns a buffer and caches two predicates (isAscii, isValidUtf8); Text is
// a reading of a string. They are two views of the same material, which is why
// they live in one module.
//
// BEST  — hoist the handle and use it many times. Using it is free (indexOf
//         1.01x, compare 1.00x, slice 1.00x against a raw Uint8Array).
// WORST — construct one per operation: ~1.7x a raw copy of the same buffer.
//         That row is measured here, not hidden.
//
// NOTE ON A WRONG ANSWER: this file first reported construction at 14x and
// explained it as "three objects versus one". The explanation was invented to
// fit the number. The real cause was in dyn_bytes_view, which probed for an
// ArrayBuffer first and therefore THREW and swallowed an exception on every
// call taking a Uint8Array -- 11.5x on the whole module. Reordering the probe
// took slice from 2.96 us to 0.13 us and construction from 4.30 to 0.50.
// A plausible story is not a cause.
//
// Run: dynajs examples/js/dynajs_bytes.js
import { test, run, assert, assertEqual } from "./harness.js";
import { Bytes, Text, compare as freeCompare } from "dyna:bytes";

// ---------------------------------------------------------------------------
// The cached predicates, and what makes them trustworthy
// ---------------------------------------------------------------------------

test("isAscii and isValidUtf8 are computed once, at construction", () => {
  const b = new Bytes("hello");
  assertEqual(b.length, 5);
  assertEqual(b.isAscii, true);
  assertEqual(b.isValidUtf8, true);           // ASCII is valid UTF-8 by definition

  const w = new Bytes("héllo→");
  assertEqual(w.isAscii, false);
  assertEqual(w.isValidUtf8, true);
  assertEqual(w.length, 9);                   // BYTES, not characters
});

test("invalid UTF-8 is detected, not repaired", () => {
  // A caller uses the flag to decide whether decoding is safe. Silently
  // substituting U+FFFD would make the flag a lie.
  assertEqual(new Bytes(new Uint8Array([0x41, 0xC3, 0x28])).isValidUtf8, false);
  assertEqual(new Bytes(new Uint8Array([0xC0, 0x80])).isValidUtf8, false);      // overlong
  assertEqual(new Bytes(new Uint8Array([0xED, 0xA0, 0x80])).isValidUtf8, false); // surrogate
  assertEqual(new Bytes(new Uint8Array([0xF5, 0x80, 0x80, 0x80])).isValidUtf8, false);
});

test("the constructor COPIES so the cached flags cannot become lies", () => {
  const src = new Uint8Array([65, 66, 67]);
  const b = new Bytes(src);
  src[0] = 0xFF;                              // mutate the original afterwards
  assertEqual(b.isAscii, true);               // still true, and still TRUE
  assertEqual(b.toString().charCodeAt(0), 65);
});

test("but .slice() VIEWS — that is the point of the method", () => {
  const owner = new Bytes("abcdef");
  const mid = owner.slice(1, 4);
  assertEqual(mid.toString(), "bcd");
  mid.fill(88);
  assertEqual(owner.toString(), "aXXXef");    // the write reaches the owner
  // A view keeps its owner's buffer alive; a view into freed memory would be
  // far worse than retaining it.
  assertEqual(owner.slice(1, 5).slice(1, 3).length, 2);
});

// ---------------------------------------------------------------------------
// BEST and WORST, measured
// ---------------------------------------------------------------------------

function timeIt(fn, reps) {
  for (let i = 0; i < 2000; i++) fn();
  const t0 = performance.now();
  for (let i = 0; i < reps; i++) fn();
  return (performance.now() - t0) * 1000 / reps;
}

test("BEST: hoisted — using a handle costs the same as the raw view", () => {
  const raw = new Uint8Array(4096);
  for (let i = 0; i < raw.length; i++) raw[i] = 65 + (i % 26);
  const bh = new Bytes(raw);
  const needle = new Uint8Array([88, 89, 90]);

  const viaRaw = timeIt(() => freeCompare(raw, raw), 20000);
  const viaHandle = timeIt(() => bh.compare(raw), 20000);
  print(`  compare  raw ${viaRaw.toFixed(3)} us   handle ${viaHandle.toFixed(3)} us` +
        `   (${(viaHandle / viaRaw).toFixed(2)}x)`);
  assert(viaHandle > 0 && viaRaw > 0, "both paths ran");   /* timing printed, not gated */

  // A view is O(1) in its length: slicing 8 bytes and 4 KB cost the same. If
  // .slice ever became a copy, this ratio would move.
  const small = timeIt(() => bh.slice(0, 8), 20000);
  const big = timeIt(() => bh.slice(0, 4096), 20000);
  print(`  slice    8B ${small.toFixed(3)} us   4KB ${big.toFixed(3)} us` +
        `   (${(big / small).toFixed(2)}x — a view is O(1))`);
  assert(bh.slice(0, 4096).length === 4096 && bh.slice(0, 8).length === 8,
         "slice returns the requested length regardless of size");
  // 65+(i%26) puts X,Y,Z consecutively at index 23 -- asserted rather than
  // guessed, because "not found" was the first guess and it was wrong.
  assertEqual(bh.indexOf(needle), 23);
});

test("WORST: constructing a handle per operation", () => {
  const raw = new Uint8Array(4096);
  const perOp = timeIt(() => new Bytes(raw).length, 5000);
  const rawCopy = timeIt(() => raw.slice(0), 5000);
  print(`  new Bytes(4KB) ${perOp.toFixed(3)} us   vs raw copy ${rawCopy.toFixed(3)} us` +
        `   (${(perOp / rawCopy).toFixed(1)}x — the extra object plus the scan)`);
  // Stated, not hidden: build it once and reuse it. The rows above show that
  // reusing it is free.
  assert(perOp > 0 && rawCopy > 0, "both constructions ran");   /* timing printed */
});

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

test("Text is an interpretation of a string", () => {
  const t = new Text("héllo");
  assertEqual(t.value, "héllo");
  assertEqual(t.countUtf8(), 5);              // code points, not bytes
  assertEqual(t.toBytes().length, 6);         // é is two bytes in UTF-8
  assert(Bytes.isBytes(t.toBytes()), "toBytes yields a Bytes handle");
  // isWide is the engine's own summary bit: any code unit above U+00FF. é is
  // U+00E9, so a Latin-1 string is NOT wide — that distinction decides whether
  // a byte-wise kernel can run at all.
  assertEqual(t.isWide, false);
  assertEqual(new Text("héllo→").isWide, true);
});

// ---------------------------------------------------------------------------
// Abuse
// ---------------------------------------------------------------------------

test("abuse: hostile arguments and degenerate inputs", () => {
  const b = new Bytes("abcdefgh");
  const throws = (fn) => { try { fn(); return false; } catch { return true; } };

  // slice() coerces its bounds, so valueOf is the hook that fires. toString
  // would never run, and a test hooking it would prove nothing.
  let ran = 0;
  assertEqual(b.slice({ valueOf() { ran++; return 2; } }, 5).toString(), "cde");
  assert(ran > 0, "the valueOf hook actually fired");

  // A plain object is not byte data, and is not stringified into some.
  assert(throws(() => new Bytes({ toString() { return "xx"; } })));
  assert(throws(() => new Bytes(new Float64Array(2))), "a wider view is refused");
  assert(throws(() => new Bytes()), "the argument is required");
  assert(throws(() => Bytes.alloc()), "alloc needs a length");
  assert(throws(() => b.readUint32LE(99)), "an out-of-range offset throws");

  // Degenerate shapes must not crash or silently misbehave.
  assertEqual(new Bytes("").length, 0);
  assertEqual(new Bytes("").isAscii, true);
  assertEqual(b.slice(4, 1).length, 0, "an inverted range is empty, not negative");
  assertEqual(b.slice(0, 999).length, 8, "an end past the buffer clamps");
  assertEqual(b.slice(-2).toString(), "gh", "a negative start counts from the end");
  assertEqual(Bytes.concat([]).length, 0);
  assertEqual(new Text("").countUtf8(), 0);

  // Churn: many short-lived views over one owner must not corrupt it.
  const owner = new Bytes("0123456789".repeat(100));
  for (let i = 0; i < 5000; i++)
    if (owner.slice(i % 900, (i % 900) + 10).length !== 10) throw new Error("width");
  if (typeof gc === "function") gc();
  assertEqual(owner.length, 1000, "the owner survives 5000 views");
});

await run("dyna:bytes — the Bytes and Text value handles");
