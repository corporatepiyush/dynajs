/* T4: narrow-at-flatten — concats involving wide all-latin1 slices.
 * Width itself is not observable from JS; every assertion is content-
 * and behavior-based (both engines must agree). The engine-side width
 * proof lives in scratch/t4_p5.js via the -d memory dump. */
var WN = mkWideNarrowable(3000); /* wide parent, narrowable tail */
var S = WN.slice(1, 2901);       /* wide slice, all-latin1 content */

test("narrow_flatten_content", function () {
  var r = S + "xy";
  eq(r.length, 2902, "concat length");
  eq(r.substring(0, 3), WN.substring(1, 4), "head content");
  eq(r.substring(r.length - 2), "xy", "tail content");
  ok(r === WN.substring(1, 2901) + "xy", "content equality vs spec");
});
test("narrow_flatten_prefix_concat", function () {
  var r = "ab" + S;
  eq(r.length, 2902, "prefix concat length");
  ok(r === "ab" + WN.substring(1, 2901), "prefix content");
});
test("narrow_flatten_slice_slice", function () {
  var a = WN.slice(1, 1451), b = WN.slice(1451, 2901);
  var r = a + b;
  eq(r.length, 2900, "slice+slice length");
  ok(r === WN.substring(1, 2901), "slice+slice content");
});
test("narrow_flatten_result_usable_as_key", function () {
  var r = S + "xy";
  var m = new Map();
  m.set(r, 1);
  eq(m.get(WN.substring(1, 2901) + "xy"), 1, "narrow-flattened key interning");
});
test("genuine_wide_concat_not_narrowed", function () {
  var gw = mkWideRnd(500, 51); /* genuinely wide */
  var sl = gw.slice(0, 500);
  var r = sl + "x";
  eq(r.length, 501, "genuine wide concat length");
  ok(r === gw + "x", "genuine wide concat content");
  eq(r.charCodeAt(500), 120, "narrow tail after wide content");
});
test("narrow_flatten_then_search", function () {
  var r = S + "0123456789";
  var needle = WN.substring(100, 113);
  eq(r.indexOf(needle), 9, "search over flattened result (period-90 content)");
  eq(r.lastIndexOf(needle), refLastIndexOf(r, needle, undefined), "lastIndexOf over flattened result");
  var absent = WN.substring(100, 113) + "zz";
  eq(r.indexOf(absent), -1, "absent in flattened result");
});
test("number_concat_with_wide_slice", function () {
  var r = S + 42;
  ok(r === WN.substring(1, 2901) + "42", "number concat content");
  var r2 = 42 + S;
  ok(r2 === "42" + WN.substring(1, 2901), "prefix number concat content");
});
test("caseconv_compare_after_flatten", function () {
  var r = (S + "ABC").toLowerCase();
  ok((S + "ABC").toLowerCase() === (WN.substring(1, 2901) + "ABC").toLowerCase(), "caseconv after flatten");
  var a = WN.slice(1, 2001), b = WN.slice(1, 2001);
  ok(!(a < b) && !(a > b), "equal slices compare middle");
  var c = WN.slice(1, 2001), d = WN.slice(2, 2002);
  eq(c < d, WN.substring(1, 2001) < WN.substring(2, 2002), "ordering matches spec");
});
test("narrow_flatten_inplace_rope_paths", function () {
  /* rope path: long second operand forces rope flatten; result must be
     content-identical whichever internal width it lands in */
  var long = mkvalRnd(2000, 53, false);
  var r = S + long;
  ok(r === WN.substring(1, 2901) + long, "rope-flatten content");
  var r2 = long + S;
  ok(r2 === long + WN.substring(1, 2901), "rope-flatten prefix content");
});
summary("sliced_waveB");
