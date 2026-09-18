// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[6] = ["proto-clean true true"];
// D: for-of array fast path × prototype mutation + mid-flight shrink
const dense = [10, 20, 30];
Array.prototype[0] = "PROTO0";
Array.prototype[1] = "PROTO1";
try {
  const seen = [];
  for (const v of dense) seen.push(v);
  __A("d01_forof_proto.js:dense", function () { assert_eq(seen.join(","), "10,20,30", "dense"); });
  const holey = [10, , 30];
  const seen2 = [];
  for (const v of holey) seen2.push(v === undefined ? "HOLE->" + Array.prototype[0] : v);
  __A("d01_forof_proto.js:holey", function () { assert_eq(seen2.join(","), "10,PROTO1,30", "holey"); });
  const seen3 = [];
  const mixed = [1, 2, 3];
  delete mixed[1];
  for (const v of mixed) seen3.push(v);
  __A("d01_forof_proto.js:deleted", function () { assert_eq(seen3.join(","), "1,PROTO1,3", "deleted"); });
} finally {
  delete Array.prototype[0];
  delete Array.prototype[1];
}
// prototype gains `0` DURING iteration of a dense array + shrink mid-flight
const dense2 = ["a", "b", "c"];
Array.prototype[0] = "LATE0";
let out2 = [];
try {
  let first = true;
  for (const v of dense2) {
    out2.push(v);
    if (first) { first = false; dense2.length = 1; }
  }
  __A("d01_forof_proto.js:shrink", function () { assert_eq(out2.join(","), "a", "shrink"); });
} finally { delete Array.prototype[0]; }
// typed-array for-of with proto pollution (typed arrays ignore proto indices)
Object.defineProperty(Object.prototype, "0", { value: "OBJ0", configurable: true, writable: true });
try {
  __A("d01_forof_proto.js:u8", function () { assert_eq([...new Uint8Array([1, 2, 3])].join(","), "1,2,3", "u8"); });
  const holey2 = [, 1];
  __A("d01_forof_proto.js:holey-objproto", function () { assert_eq(holey2[0], "OBJ0", "holey-objproto"); });
} finally { delete Object.prototype["0"]; }
__L(6, "proto-clean", Array.prototype[0] === undefined, [7][0] === 7);

summary("bbreview");
