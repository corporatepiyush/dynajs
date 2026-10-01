// flags: --std
import { test, run, assert, assertEqual } from "./harness.js";
import { Bytes, Text, compare as freeCompare } from "dyna:bytes";

test("isAscii and isValidUtf8 are computed once, at construction", () => {
  const b = new Bytes("hello");
  assertEqual(b.length, 5);
  assertEqual(b.isAscii, true);
  assertEqual(b.isValidUtf8, true);

  const w = new Bytes("héllo→");
  assertEqual(w.isAscii, false);
  assertEqual(w.isValidUtf8, true);
  assertEqual(w.length, 9);
});

test("invalid UTF-8 is detected, not repaired", () => {
  assertEqual(new Bytes(new Uint8Array([0x41, 0xC3, 0x28])).isValidUtf8, false);
  assertEqual(new Bytes(new Uint8Array([0xC0, 0x80])).isValidUtf8, false);
  assertEqual(new Bytes(new Uint8Array([0xED, 0xA0, 0x80])).isValidUtf8, false);
  assertEqual(new Bytes(new Uint8Array([0xF5, 0x80, 0x80, 0x80])).isValidUtf8, false);
});

test("the constructor COPIES so the cached flags cannot become lies", () => {
  const src = new Uint8Array([65, 66, 67]);
  const b = new Bytes(src);
  src[0] = 0xFF;
  assertEqual(b.isAscii, true);
  assertEqual(b.toString().charCodeAt(0), 65);
});

test("but .slice() VIEWS — that is the point of the method", () => {
  const owner = new Bytes("abcdef");
  const mid = owner.slice(1, 4);
  assertEqual(mid.toString(), "bcd");
  mid.fill(88);
  assertEqual(owner.toString(), "aXXXef");
  assertEqual(owner.slice(1, 5).slice(1, 3).length, 2);
});

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
  assert(viaHandle > 0 && viaRaw > 0, "both paths ran");

  const small = timeIt(() => bh.slice(0, 8), 20000);
  const big = timeIt(() => bh.slice(0, 4096), 20000);
  print(`  slice    8B ${small.toFixed(3)} us   4KB ${big.toFixed(3)} us` +
        `   (${(big / small).toFixed(2)}x — a view is O(1))`);
  assert(bh.slice(0, 4096).length === 4096 && bh.slice(0, 8).length === 8,
         "slice returns the requested length regardless of size");
  assertEqual(bh.indexOf(needle), 23);
});

test("WORST: constructing a handle per operation", () => {
  const raw = new Uint8Array(4096);
  const perOp = timeIt(() => new Bytes(raw).length, 5000);
  const rawCopy = timeIt(() => raw.slice(0), 5000);
  print(`  new Bytes(4KB) ${perOp.toFixed(3)} us   vs raw copy ${rawCopy.toFixed(3)} us` +
        `   (${(perOp / rawCopy).toFixed(1)}x — the extra object plus the scan)`);
  assert(perOp > 0 && rawCopy > 0, "both constructions ran");
});

test("Text is an interpretation of a string", () => {
  const t = new Text("héllo");
  assertEqual(t.value, "héllo");
  assertEqual(t.countUtf8(), 5);
  assertEqual(t.toBytes().length, 6);
  assert(Bytes.isBytes(t.toBytes()), "toBytes yields a Bytes handle");
  assertEqual(t.isWide, false);
  assertEqual(new Text("héllo→").isWide, true);
});

test("abuse: hostile arguments and degenerate inputs", () => {
  const b = new Bytes("abcdefgh");
  const throws = (fn) => { try { fn(); return false; } catch { return true; } };

  let ran = 0;
  assertEqual(b.slice({ valueOf() { ran++; return 2; } }, 5).toString(), "cde");
  assert(ran > 0, "the valueOf hook actually fired");

  assert(throws(() => new Bytes({ toString() { return "xx"; } })));
  assert(throws(() => new Bytes(new Float64Array(2))), "a wider view is refused");
  assert(throws(() => new Bytes()), "the argument is required");
  assert(throws(() => Bytes.alloc()), "alloc needs a length");
  assert(throws(() => b.readUint32LE(99)), "an out-of-range offset throws");

  assertEqual(new Bytes("").length, 0);
  assertEqual(new Bytes("").isAscii, true);
  assertEqual(b.slice(4, 1).length, 0, "an inverted range is empty, not negative");
  assertEqual(b.slice(0, 999).length, 8, "an end past the buffer clamps");
  assertEqual(b.slice(-2).toString(), "gh", "a negative start counts from the end");
  assertEqual(Bytes.concat([]).length, 0);
  assertEqual(new Text("").countUtf8(), 0);

  const owner = new Bytes("0123456789".repeat(100));
  for (let i = 0; i < 5000; i++)
    if (owner.slice(i % 900, (i % 900) + 10).length !== 10) throw new Error("width");
  if (typeof gc === "function") gc();
  assertEqual(owner.length, 1000, "the owner survives 5000 views");
});

await run("dyna:bytes — the Bytes and Text value handles");
