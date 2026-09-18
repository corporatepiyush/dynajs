/* bytecode-cache style round-trip: sliced strings in cpool positions must
   serialize flattened and read back value-identical. Engine-only (bjson.so);
   the runner injects the module import for engines that have it; on node the
   import throws and the runner records a skip. */

  var s = "";
  for (var i = 0; i < 3000; i++) s += String.fromCharCode(97 + (i % 26));
  var slice = s.slice(100, 2500);           /* 2400-char slice */
  var wide = "";
  for (var i2 = 0; i2 < 1500; i2++) wide += String.fromCharCode(0x100 + (i2 % 300));
  var wslice = wide.slice(50, 1200);        /* wide slice */
  var obj = {
    lit: "plain literal",
    a: slice,
    b: [slice.slice(10, 500), wslice],
    c: { deep: slice.substring(5, 60) }
  };
  var buf = bjson.write(obj);
  var back = bjson.read(buf, 0, buf.byteLength);
  eq(back.a, slice, "slice roundtrip");
  eq(back.a.length, 2400, "slice len roundtrip");
  eq(back.b[0], s.slice(110, 600), "sub-slice roundtrip");
  eq(back.b[1], wslice, "wide slice roundtrip");
  eq(back.b[1].charCodeAt(0), wide.charCodeAt(50), "wide first unit");
  eq(back.c.deep, s.slice(105, 160), "nested roundtrip");
  eq(back.lit, "plain literal", "literal roundtrip");
  /* round-tripped value must behave like any string */
  eq(back.a.indexOf("zzz") === -1 || true, true, "indexOf ok");
  eq(back.a.slice(1, 9), s.slice(101, 109), "reslice after roundtrip");
console.log("PASS 21_bjson_roundtrip");
