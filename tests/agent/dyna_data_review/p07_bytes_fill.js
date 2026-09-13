import { Bytes, fill } from "dyna:bytes";
import { eq as EQ, ok as OK, throws as TH, done as DONE } from "./harness.js";
function rd(b) { const o = []; for (let i = 0; i < b.length; i++) o.push(b.readUint8(i)); return o; }

// documented: handle fill returns the UNDERLYING Uint8Array, not the handle
const b = new Bytes(new Uint8Array(4));
const r = b.fill(0xAB);
OK(r instanceof Uint8Array, "fill returns a Uint8Array");
OK(!Bytes.isBytes(r), "fill does not return the Bytes handle");
EQ(r.length, 4, "returned view has full length");
EQ(Array.from(r), [171, 171, 171, 171], "bytes actually filled");
r[0] = 1;
EQ(b.readUint8(0), 1, "returned Uint8Array aliases handle storage");
// partial fill via handle
const c = new Bytes(new Uint8Array(4));
c.fill(7, 1, 3);
EQ(rd(c), [0, 7, 7, 0], "handle fill start/end");
// wrong-typed fill values: val coerced to number, low 8 bits
const d = new Bytes(new Uint8Array(2));
d.fill("5");
EQ(rd(d), [5, 5], "string val coerced");
const e2 = new Bytes(new Uint8Array(2));
e2.fill(258);
EQ(rd(e2), [2, 2], "val truncated to low 8 bits");
TH(function () { c.fill(1, -1); }, "RangeError", "negative start throws");
TH(function () { c.fill(1, 0, 5); }, "RangeError", "end OOB throws");
TH(function () { c.fill(1, 3, 1); }, "RangeError", "start>end throws");
// free function still returns buf
const u = new Uint8Array(3);
OK(fill(u, 9) === u, "free fill returns buf itself");
EQ(Array.from(u), [9, 9, 9], "free fill wrote");
// chaining through the returned view keeps handle storage consistent
const f = new Bytes(new Uint8Array(2));
f.fill(1).fill(2, 1);
EQ(rd(f), [1, 2], "chain via returned view");
DONE("p07_bytes_fill");
