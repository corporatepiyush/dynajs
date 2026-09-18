import { structuredClone } from "dyna:serialize";
import { eq as EQ, ok as OK, done as DONE } from "./harness.js";

// every typed-array kind: same constructor + same bytes over a FRESH buffer
const F = {
  Int8Array: v => Array.from(v),
  Uint8Array: v => Array.from(v),
  Uint8ClampedArray: v => Array.from(v),
  Int16Array: v => Array.from(v),
  Uint16Array: v => Array.from(v),
  Int32Array: v => Array.from(v),
  Uint32Array: v => Array.from(v),
  Float16Array: v => [v[0], v[1]],
  Float32Array: v => [v[0], v[1]],
  Float64Array: v => [v[0], v[1]],
};
for (const k of Object.keys(F)) {
  const src = new globalThis[k]([1, 2, 3, 4]);
  const c = structuredClone(src);
  EQ(c.constructor.name, k, k + " constructor preserved");
  EQ(F[k](c), F[k](src), k + " bytes preserved");
  c[0] = 99;
  EQ(src[0], 1, k + " not aliased");
}
{
  const src = new BigInt64Array(2); src[0] = 3n; src[1] = 0x7FFFFFFFFFFFFFFFn;
  const c = structuredClone(src);
  EQ(c.constructor.name, "BigInt64Array", "BigInt64Array constructor preserved");
  OK(c[1] === 0x7FFFFFFFFFFFFFFFn, "BigInt64 bytes preserved");
  const u = new BigUint64Array(1); u[0] = 0xFFFFFFFFFFFFFFFFn;
  const cu = structuredClone(u);
  EQ(cu.constructor.name, "BigUint64Array", "BigUint64Array constructor preserved");
  OK(cu[0] === 0xFFFFFFFFFFFFFFFFn, "BigUint64 bytes preserved");
}
// bpe==1 kinds are routed through the pre-existing byte path (vs_new_bytes):
// constructor identification in vs_clone_bytes is BYPASSED for them.
{
  const c = structuredClone(new Int8Array([1, 2]));
  EQ(c.constructor.name, "Int8Array", "Int8Array (bpe=1) constructor preserved");
  const c2 = structuredClone(new Uint8ClampedArray([1, 2]));
  EQ(c2.constructor.name, "Uint8ClampedArray", "Uint8ClampedArray (bpe=1) constructor preserved");
}
// ArrayBuffer
{
  const ab = new ArrayBuffer(8); new Uint16Array(ab)[0] = 0x4142;
  const cab = structuredClone(ab);
  EQ(cab.constructor.name, "ArrayBuffer", "ArrayBuffer stays ArrayBuffer");
  EQ(cab.byteLength, 8, "ArrayBuffer byteLength");
  EQ(new Uint16Array(cab)[0], 0x4142, "ArrayBuffer content");
  OK(cab !== ab, "ArrayBuffer fresh");
}
// DataView clones as Uint8Array over its bytes (documented)
{
  const dv = new DataView(new ArrayBuffer(4)); dv.setUint32(0, 0xDEADBEEF);
  const cdv = structuredClone(dv);
  EQ(cdv.constructor.name, "Uint8Array", "DataView -> Uint8Array");
  EQ(Array.from(cdv), [222, 173, 190, 239], "DataView bytes");
}
DONE("p10_serialize_clone_kinds");
