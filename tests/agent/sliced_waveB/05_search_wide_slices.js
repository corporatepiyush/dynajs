/* T3: wide slices — mixed-width search matrices and wide lastIndexOf
 * budget-fallback, plus atom-parent wide slices. */
var PW = mkWideRnd(20000, 31);
var SW = PW.slice(500, 16500); /* 16000 wide units */

test("wide_wide_search", function () {
  var mid = PW.substring(8000, 8030);
  eq(SW.indexOf(mid), 7500, "wide indexOf relative");
  eq(SW.lastIndexOf(mid), 7500, "wide lastIndexOf relative");
  eq(SW.includes(mid), true, "wide includes");
  eq(SW.startsWith(mid), false, "wide startsWith");
  eq(SW.endsWith(mid), false, "wide endsWith");
});
test("wide_lastindexof_budget_fallback", function () {
  /* > 1024 positions from the default start: must take the exact
     forward fallback and still find the only match */
  var late = PW.substring(15000, 15025);
  eq(SW.lastIndexOf(late), 14500, "wide late match");
  var at0 = PW.substring(500, 525);
  eq(SW.lastIndexOf(at0), 0, "wide at-start match (full fallback walk)");
  var absent = PW.substring(0, 30) + "\u0378"; /* \u0378 unassigned: absent */
  eq(SW.lastIndexOf(absent), -1, "wide absent");
});
test("mixed_width_search", function () {
  var narrow = mkvalRnd(100, 41, false);
  var wideNarrowNeedle = SW.substring(2000, 2010);
  /* narrow haystack, wide needle: always absent (width-agnostic compare
     by VALUE: latin1 chars can equal wide chars) */
  eq(narrow.indexOf("\u0101"), -1, "wide char not in narrow haystack");
  var sN = narrow.slice(10, 90);
  eq(sN.indexOf("\u0101"), -1, "same on slice");
  /* wide slice containing latin1-range units: value-based matching */
  var mixed = mkWideRnd(2000, 43);
  var mixedSlice = mixed.slice(0, 2000);
  var u = mixedSlice.substring(100, 101);
  eq(mixedSlice.indexOf(u), 100, "single unit round-trip");
});
test("wide_atom_slice_search", function () {
  var W = "\u0F00\u0F01\u0F02\u0F03\u0F04\u0F05\u0F06\u0F07\u0F08\u0F09\u0F0A\u0F0B\u0F0C\u0F0D\u0F0E\u0F0F\u0F10\u0F11\u0F12\u0F13\u0F14\u0F15\u0F16\u0F17\u0F18\u0F19\u0F1A\u0F1B\u0F1C\u0F1D\u0F1E\u0F1F\u0F20\u0F21\u0F22\u0F23\u0F24\u0F25\u0F26\u0F27\u0F28\u0F29\u0F2A\u0F2B\u0F2C\u0F2D\u0F2E\u0F2F\u0F30\u0F31\u0F32\u0F33\u0F34\u0F35\u0F36\u0F37\u0F38\u0F39\u0F3A\u0F3B\u0F3C\u0F3D\u0F3E\u0F3F\u0F40";
  var t = W.slice(8, 60);
  eq(t.indexOf("\u0F10"), 8, "wide atom slice indexOf");
  eq(t.lastIndexOf("\u0F3B"), 51, "wide atom slice lastIndexOf");
  ok(t.startsWith("\u0F08\u0F09"), "wide atom slice startsWith");
  ok(t.endsWith("\u0F3F\u0F40") === false, "wide atom slice endsWith clipped");
});
test("wide_replace_positions", function () {
  var s = SW.slice(0, 8000);
  var nd = PW.substring(3000, 3010);
  var r = s.replace(nd, "\u2014");
  eq(r.indexOf("\u2014"), 2500, "wide replace position");
  eq(r.length, s.length - 10 + 1, "wide replace length math");
});
test("search_genuine_wide_after_narrow_concat", function () {
  /* genuinely-wide content concat keeps working (T4 must not narrow it) */
  var gw = mkWideRnd(2000, 47);
  var s2 = gw.slice(0, 1999);
  var r = s2 + "\u1450"; /* last char genuinely wide */
  eq(r.charCodeAt(r.length - 1), 0x1450, "wide char preserved");
  eq(r.indexOf(String.fromCharCode(0x1450)), r.length - 1, "wide char found");
});
summary("sliced_waveB");
