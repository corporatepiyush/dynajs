/* T3: window edges — a slice is a VIEW into its parent, so a needle
 * extending past the window end must be absent (the search domain is
 * the parent region [offset, offset+len)), match indices are
 * slice-relative, and surrogate pairs split by the window edge must not
 * match across it. */
var P = mkvalRnd(40000, 21, false);

test("needle_crossing_window_end_absent", function () {
  var s = P.slice(100, 1100);
  var across = P.substring(1090, 1110); /* 10 inside, 10 outside */
  eq(s.indexOf(across), -1, "indexOf across end");
  eq(s.includes(across), false, "includes across end");
  eq(s.lastIndexOf(across), -1, "lastIndexOf across end");
});
test("needle_crossing_window_start_absent", function () {
  var s = P.slice(100, 1100);
  var across = P.substring(90, 110); /* 10 before, 10 inside */
  eq(s.indexOf(across), -1, "indexOf across start");
  eq(s.includes(across), false, "includes across start");
});
test("match_indices_are_slice_relative", function () {
  var off = 500;
  var s = P.slice(off, off + 5000);
  var inWindow = P.substring(off + 1000, off + 1030);
  eq(s.indexOf(inWindow), 1000, "indexOf relative");
  eq(s.lastIndexOf(inWindow), 1000, "lastIndexOf relative");
  var second = P.substring(off + 1000, off + 1030);
  eq(s.replace(inWindow, "X").indexOf("X"), 1000, "replace position relative");
});
test("search_on_slice_of_slice", function () {
  var a = P.slice(200, 12000);
  var b = a.slice(300, 9000); /* parent window [500, 9500) */
  var inB = P.substring(2000, 2030);
  eq(b.indexOf(inB), 2000 - 500, "relative to slice-of-slice root window");
  var atB0 = P.substring(500, 530);
  eq(b.indexOf(atB0), 0, "window start match");
  eq(b.lastIndexOf(atB0), 0, "window start lastIndexOf");
});
test("astral_pairs_split_at_window_edges", function () {
  var base = mkAstral(4000);
  var MARK = "\u{1F9FF}"; /* outside the random alphabet: unique per string */
  var A = base.substring(0, 2000) + MARK + base.substring(2002);
  var s = A.slice(100, 3900);
  eq(s.indexOf(MARK), 1900, "unique marker found window-relative");
  eq(s.lastIndexOf(MARK), 1900, "marker lastIndexOf");
  /* marker split by the window edge cannot match inside the window */
  var B = base.substring(0, 100) + MARK + base.substring(102);
  var sB = B.slice(101, 3900); /* window starts mid-marker */
  eq(sB.indexOf(MARK), -1, "split marker absent (indexOf)");
  eq(sB.includes(MARK), false, "split marker absent (includes)");
  eq(sB.lastIndexOf(MARK), -1, "split marker absent (lastIndexOf)");
  var sC = B.slice(100, 3900); /* window starts exactly at the marker */
  eq(sC.indexOf(MARK), 0, "unaligned marker at window start matches");
});

test("astral_lastindexOf_and_caseconv", function () {
  var A = mkAstral(3000);
  var s = A.slice(100, 2900);
  var nd = A.substring(2000, 2002);
  eq(s.lastIndexOf(nd), refLastIndexOf(s, nd, undefined), "astral lastIndexOf vs reference");
  eq(s.lastIndexOf(A.substring(0, 2)), refLastIndexOf(s, A.substring(0, 2), undefined), "needle match agrees with reference");
});
test("zero_length_needles_at_boundaries", function () {
  var s = P.slice(10, 1010);
  eq(s.indexOf(""), 0, "empty needle at 0");
  eq(s.indexOf("", 5000), 1000, "empty needle clamps to window length");
  eq(s.lastIndexOf(""), 1000, "empty lastIndexOf = window length");
  eq(s.lastIndexOf("", 3), 3, "empty lastIndexOf from 3");
});
test("replace_substitution_is_window_relative", function () {
  var s = P.slice(100, 3100);
  var nd = P.substring(1000, 1010);
  var r = s.replace(nd, "[$`|$&|$']");
  ok(r.indexOf("[") >= 0, "substitution applied");
  ok(r.indexOf(nd) > 0, "matched text kept via $&");
  eq(r.split(nd).length, 2, "exactly one replacement");
  var rall = s.replaceAll(nd, "X");
  eq(rall.indexOf("X"), 900, "replaceAll first position");
});
test("trim_and_caseconv_over_atom_slice_windows", function () {
  var lit = "   The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.   ";
  var t = lit.slice(3, lit.length - 3);
  eq(t.trim().length, t.length, "trim boundaries outside window");
  ok(t.toLowerCase() === t.substring(0).toLowerCase(), "caseconv content stable");
});

function mkAstral(n) {
  var s = "";
  var x = 999;
  for (var i = 0; i < n; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    s += String.fromCharCode(0xD83D, 0xDE00 + (x % 26));
  }
  return s;
}
summary("sliced_waveB");
