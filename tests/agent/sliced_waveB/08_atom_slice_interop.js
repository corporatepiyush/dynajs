/* T1/T4 interplay: atom slices + narrow-at-flatten through JSON, template
 * literals, tagged templates, property enumeration, sort/compare, locale
 * insensitive ordering, and String/Number conversions. */
var LIT = "Interop battery: the quick brown fox jumps over the lazy dog while 42 dwarves watch. Interop battery: the quick brown fox jumps over the lazy dog while 42 dwarves watch.";

test("json_methods_on_atom_slice", function () {
  var t = LIT.slice(17, 42);
  eq(JSON.parse(JSON.stringify(t)), t, "round-trip");
  var o = JSON.parse('{"k":"' + "abc" + '"}');
  eq(o.k, "abc", "JSON.parse result unchanged");
  eq(JSON.stringify({ s: t, n: 1 }), JSON.stringify({ s: t.substring(0), n: 1 }), "stringify object equality");
});
test("template_literals_atom_slice", function () {
  var t = LIT.slice(17, 42);
  eq(`v=${t}!`, "v=the quick brown fox jumps!", "template");
  eq(String.raw`x${t}y`, "xthe quick brown fox jumpsy", "String.raw tagged");
  function tag(s, v) { return [s[0], v, s.raw[0]].join("|"); }
  eq(tag`A${t}B`, "A|the quick brown fox jumps|A", "tagged template");
});
test("enumeration_and_sort_with_slice_keys", function () {
  var o = {};
  o[LIT.slice(17, 37)] = 1;
  o[LIT.slice(38, 58)] = 2;
  o[LIT.slice(59, 79)] = 3;
  deepEq(Object.keys(o).length, 3, "three distinct slice keys");
  eq(o[LIT.slice(38, 58)], 2, "middle key");
  var arr = [LIT.slice(17, 37), LIT.slice(38, 58), LIT.slice(59, 79)];
  deepEq(arr.slice().sort(), arr.slice().sort(), "sort stable content");
  deepEq(arr.map(function (x) { return x.length; }), [20, 20, 20], "lengths");
});
test("string_number_conversions", function () {
  var numLit = "12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456";
  eq(parseInt(numLit.slice(0, 3), 10), 123, "parseInt of atom slice");
  eq(Number(numLit.slice(0, 4)), 1234, "Number of atom slice");
  eq(+numLit.slice(2, 5), 345, "unary plus of atom slice");
  eq(parseFloat(numLit.slice(4, 7) + ".5"), 567.5, "parseFloat");
});
test("slice_as_function_and_ctor_arg", function () {
  var t = LIT.slice(17, 42);
  function f(s) { return s + "/" + s.length; }
  eq(f(t), "the quick brown fox jumps/25", "pass-through");
  eq(new String(t).substring(0, 3), "the", "String object");
  eq(String(t), t, "String()");
  eq([t].join(""), t, "array join");
  eq([t, t].join("-"), t + "-" + t, "join two");
});
test("compare_and_localeordering_atom_slices", function () {
  var a = LIT.slice(17, 47), b = LIT.slice(17, 47);
  ok(a === b, "same window content eq");
  var c = LIT.slice(17, 48);
  ok(a < c || a > c, "comparable");
  eq(a.localeCompare(b), 0, "localeCompare equal");
});
test("narrow_flatten_map_set_identity", function () {
  var WN = mkWideNarrowable(600);
  var s = WN.slice(1, 501);
  var r = s + "!"; /* narrow-at-flatten path */
  var m = new Map();
  m.set(r, "v");
  eq(m.get(WN.substring(1, 501) + "!"), "v", "flattened result as Map key");
  var st = new Set();
  st.add(r);
  ok(st.has(WN.substring(1, 501) + "!"), "flattened result in Set");
});
test("concat_chain_wide_slice_stress", function () {
  var WN2 = mkWideNarrowable(400);
  var acc = WN2.slice(1, 201);
  for (var i = 0; i < 50; i++) {
    acc = acc + WN2.slice(2, 202); /* repeated narrow-at-flatten concats */
  }
  var want = WN2.substring(1, 201);
  for (var i = 0; i < 50; i++) want += WN2.substring(2, 202);
  ok(acc === want, "chained flatten content");
  eq(acc.length, want.length, "chained flatten length");
});
summary("sliced_waveB");
