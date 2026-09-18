import { structuredClone } from "dyna:serialize";
import { eq as EQ, ok as OK, throws as TH, done as DONE } from "./harness.js";

// subarray (offset view): clone holds same ELEMENTS over a tight fresh buffer
{
  const big = new Uint32Array([10, 20, 30, 40]);
  const sub = big.subarray(1, 3);
  const c = structuredClone(sub);
  EQ(c.constructor.name, "Uint32Array", "subarray ctor");
  EQ(Array.from(c), [20, 30], "subarray elements");
  EQ(c.buffer.byteLength, 8, "subarray buffer normalized to bytes");
}
// zero-length TA and empty AB
EQ(structuredClone(new Uint8Array(0)).length, 0, "empty U8 len");
EQ(structuredClone(new Float64Array(0)).length, 0, "empty F64 len");
EQ(structuredClone(new ArrayBuffer(0)).byteLength, 0, "empty AB");
// detached buffer view: must not crash; degrades to plain object (no bytes left)
{
  const ab = new ArrayBuffer(8);
  const u = new Uint8Array(ab); u[0] = 42;
  ab.transfer();
  const c = structuredClone(u);
  OK(c !== undefined, "detached view clone returns something");
  EQ(Object.keys(c).length, 0, "detached view clone empty");
}
// expando properties are dropped on typed arrays (HTML-consistent byte clone)
{
  const t = new Uint8Array([1, 2]); t.foo = 99;
  EQ(structuredClone(t).foo, undefined, "expando dropped");
}
// cycles and shared plain objects still memoized
{
  const doc = { x: 1 }; doc.self = doc;
  const cl = structuredClone(doc);
  OK(cl.self === cl, "cycle identity kept");
  const sh = { v: 1 };
  const h = structuredClone({ a: sh, b: sh });
  OK(h.a === h.b, "shared plain object identity kept");
}
// shared TYPED ARRAY references: HTML structuredClone preserves identity
{
  const ta = new Float64Array([1, 2, 3]);
  const h = structuredClone({ a: ta, b: ta });
  OK(h.a === h.b, "shared typed array identity preserved (HTML semantics)");
}
// mutation independence of wide TA clones
{
  const f = new Float64Array([1.5, 2.5]);
  const cf = structuredClone(f);
  f[0] = 999;
  OK(cf[0] === 1.5, "wide TA not aliased");
}
// nesting: TA inside object inside array at depth
{
  const doc = { list: [new Int32Array([7]), { inner: new Float32Array([0.5]) }] };
  const c = structuredClone(doc);
  EQ(c.list[0].constructor.name, "Int32Array", "nested Int32Array");
  EQ(c.list[1].inner.constructor.name, "Float32Array", "nested Float32Array");
  OK(c.list[1].inner[0] === 0.5, "nested Float32 value");
}
// functions still refused; primitives pass through
TH(function () { structuredClone(function f() {}); }, "TypeError", "function refused");
OK(structuredClone(123456789012345678901234567890n) === 123456789012345678901234567890n, "BigInt primitive passes through");
// exception safety: a throwing constructor getter must not corrupt or crash
{
  const t = new Float64Array([1, 2, 3, 4]);
  Object.defineProperty(t, "constructor", { get: function () { throw new Error("boom"); } });
  TH(function () { structuredClone(t); }, "Error", "throwing ctor getter propagates");
}
// global Uint8Array removed: fallback must still produce a safe byte clone
{
  const saved = globalThis.Uint8Array;
  const t = new saved([5, 6, 7]);
  const g = globalThis;
  try {
    // subclass of Uint8Array: identification fails -> documented Uint8Array fallback
    class Sub extends saved {}
    const s = new Sub([5, 6, 7]);
    const c = structuredClone(s);
    EQ(c.constructor.name, "Uint8Array", "subclass falls back to Uint8Array");
    EQ(Array.from(c), [5, 6, 7], "subclass fallback bytes");
  } finally { globalThis.Uint8Array = saved; }
}
DONE("p11_serialize_clone_edges");
