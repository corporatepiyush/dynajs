/* T1/T3 regexp battery over atom-parent slices and windows — same class
 * as the wave-A review probes (byte-offset aliasing, capture recovery,
 * substitutions $` $' $<name>), now with atom parents and the T3
 * windowed search underneath replace/split. */
var LIT = "Regex battery: alpha-1, beta-22, gamma-333 repeated. Regex battery: alpha-1, beta-22, gamma-333 repeated. Regex battery: alpha-1, beta-22, gamma-333 repeated.";
var WIN = LIT.slice(0, 120);

test("regexp_exec_captures_on_atom_slice", function () {
  var re = /(alpha|beta|gamma)-(\d+)/g;
  var m, out = [];
  while ((m = re.exec(WIN)) !== null) out.push([m[1], m[2], m.index]);
  deepEq(out, [
    ["alpha", "1", 15], ["beta", "22", 24], ["gamma", "333", 33],
    ["alpha", "1", 68], ["beta", "22", 77], ["gamma", "333", 86]
  ], "exec captures + indices");
});
test("regexp_sticky_lastindex_on_window", function () {
  var re = /\d+/y;
  re.lastIndex = 21;
  var m = re.exec(WIN);
  eq(m[0], "1", "sticky first number");
  eq(m.index, 21, "sticky index");
  re.lastIndex = 29;
  eq(re.exec(WIN)[0], "22", "sticky second");
});
test("regexp_replace_substitution_on_atom_slice", function () {
  var t = LIT.slice(15, 45); /* "alpha-1, beta-22, gamma-333 re" */
  eq(t.replace(/(\w+)-(\d+)/, "<$2$1>"), "<1alpha>, beta-22, gamma-333 re", "replace $2$1");
  eq(t.replace(/beta-22/, "[$`|$&|$']"), "alpha-1, [alpha-1, |beta-22|, gamma-333 re], gamma-333 re", "backtick/amp/quote");
  eq(t.replace(/(\w+)-(\d+)/g, function (m0, a, b, off) {
    return b + ":" + a + "@" + off;
  }), "1:alpha@0, 22:beta@9, 333:gamma@18 re", "functional replace offsets");
});
test("regexp_split_on_atom_slice", function () {
  var t = LIT.slice(15, 45);
  deepEq(t.split(/,\s*/), ["alpha-1", "beta-22", "gamma-333 re"], "split");
  deepEq(t.split(/-(\d+)/), ["alpha", "1", ", beta", "22", ", gamma", "333", " re"], "split with captures");
});
test("regexp_unicode_on_wide_atom_slice", function () {
  var W = "\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA\u00E5\u00E9\u00EE\u00F5\u00FA";
  var t = W.slice(10, 60);
  var re = /[\u00E5\u00E9]/g;
  var count = 0;
  while (re.exec(t) !== null) count++;
  eq(count, 20, "wide atom slice char class matches");
  var ru = /\u00F5/gu;
  var m2 = ru.exec(t);
  eq(m2.index, 3, "unicode flag index");
});
test("regexp_matchall_indices_on_slice", function () {
  var t = LIT.slice(0, 100);
  var it = t.matchAll(/(\w+)-(\d+)/g);
  var idxs = [];
  for (var m of it) idxs.push(m.index);
  deepEq(idxs, [15, 24, 33, 68, 77, 86], "matchAll indices slice-relative");
});
test("regexp_anchors_at_window_edges", function () {
  var t = LIT.slice(15, 45);
  ok(/^alpha/.test(t), "^ at window start");
  ok(/re$/.test(t), "$ at window end");
  var m = /^alpha/.exec(t);
  eq(m.index, 0, "anchor index 0");
});
test("regexp_long_range_search_on_slice", function () {
  var P = mkvalRnd(40000, 61, false);
  var s = P.slice(0, 38000);
  var target = P.substring(30000, 30010);
  var re = new RegExp(target.replace(/[a-z]/g, function (c) { return c; }));
  var m = re.exec(s);
  eq(m.index, s.indexOf(target), "regexp long-window search consistent with indexOf");
});
summary("sliced_waveB");
