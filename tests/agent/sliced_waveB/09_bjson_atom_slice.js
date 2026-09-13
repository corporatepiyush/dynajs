/* dynajs-only: bjson round-trip over atom-parent slices and
 * narrow-at-flatten results. Listed in tests/agent/_h/exclude.tsv for
 * node (no bjson global there). */
var LIT = "Bjson battery: pack and unpack slices of interned strings. Bjson battery: pack and unpack slices of interned strings. Bjson battery: pack and unpack slices of interned strings.";
var WN = mkWideNarrowable(500);

function rt(v) { var buf = bjson.write(v); return bjson.read(buf, 0, buf.byteLength); }

test("bjson_atom_slice_roundtrip", function () {
  var t = LIT.slice(6, 56);
  var back = rt(t);
  ok(back === t, "atom slice round-trip content/identity");
  var o = rt({ s: LIT.slice(6, 56), arr: [LIT.slice(10, 60), LIT.slice(20, 70)] });
  eq(o.s, LIT.slice(6, 56), "object field");
  eq(o.arr[1], LIT.slice(20, 70), "nested array element");
});
test("bjson_slice_of_slice", function () {
  var a = LIT.slice(6, 106);
  var b = a.slice(10, 90);
  ok(rt(b) === b, "slice-of-slice round-trip");
});
test("bjson_wide_atom_slice", function () {
  var W = "\u3041\u3043\u3045\u3047\u3049\u304B\u304D\u304F\u3051\u3053\u3042\u3044\u3046\u3048\u304A\u3041\u3043\u3045\u3047\u3049\u304B\u304D\u304F\u3051\u3053\u3042\u3044\u3046\u3048\u304A\u3041\u3043\u3045\u3047\u3049\u304B\u304D\u304F\u3051\u3053\u3042\u3044\u3046\u3048\u304A\u3041\u3043\u3045\u3047\u3049\u304B\u304D\u304F\u3051\u3053\u3042\u3044\u3046\u3048\u304A\u3041\u3043\u3045\u3047\u3049";
  var t = W.slice(10, 60);
  ok(rt(t) === t, "wide atom slice round-trip");
});
test("bjson_narrow_flatten_result", function () {
  var r = WN.slice(1, 401) + "xy"; /* narrow-at-flatten path */
  ok(rt(r) === r, "flattened concat round-trip");
  var o = rt({ k: r });
  eq(o.k, WN.substring(1, 401) + "xy", "flattened content via bjson");
});
test("bjson_object_with_slice_keys", function () {
  var o = {};
  o[LIT.slice(6, 46)] = 1;
  o[LIT.slice(46, 86)] = 2;
  var back = rt(o);
  eq(back[LIT.slice(6, 46)], 1, "object key 1");
  eq(back[LIT.slice(46, 86)], 2, "object key 2");
  var arr = [LIT.slice(6, 46), LIT.slice(46, 86)];
  var backA = rt(arr);
  eq(backA[0], LIT.slice(6, 46), "array element 0");
  eq(backA[1], LIT.slice(46, 86), "array element 1");
});
summary("sliced_waveB");
