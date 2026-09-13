/* T1: slices over ATOM parents (string literals as runtime values).
 * Literals reach the interpreter as the interned JSString (atom_type != 0),
 * so every assertion here exercises the atom-parent slice path. */
var L64 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";
var L1K = L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64 + L64;
/* NOTE: L1K is built by concat => value parent. The ATOM cases are the
 * string literals below (source-level 256+ char literals). */
var ALIT = "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.";
var BLIT = "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678";

test("atom_slice_value", function () {
  var t = ALIT.slice(4, 24);
  eq(t, "quick brown fox jump", "slice content");
  eq(t.length, 20, "slice length");
});
test("atom_slice_eq_content", function () {
  var t = ALIT.slice(4, 24);
  ok(t === "quick brown fox jump", "slice === equal literal (content eq)");
  ok(t === ALIT.slice(4, 24), "two slices of same atom are === (content eq)");
  ok(!(t === ALIT.slice(4, 25)), "different length not ===");
});
test("atom_slice_full_range_is_value", function () {
  var t = ALIT.slice(0);
  ok(t === ALIT, "slice(0) content-equals the literal");
  eq(t.length, ALIT.length, "full length");
});
test("atom_slice_reads", function () {
  var t = ALIT.slice(10, 60);
  eq(t.charAt(0), "b", "charAt");
  eq(t.charCodeAt(0), 98, "charCodeAt");
  eq(t[1], "r", "bracket read");
  eq(t.indexOf("own"), 2, "indexOf in atom slice");
  eq(t.lastIndexOf("o"), 47, "lastIndexOf in atom slice");
  eq(t.includes("own"), true, "includes");
  eq(t.startsWith("brown"), true, "startsWith");
  eq(t.endsWith("he do"), false, "endsWith");
  eq(t.codePointAt(0), 98, "codePointAt");
  eq(t.at(-1), "n", "at(-1)");
});
test("atom_slice_of_slice", function () {
  var t = ALIT.slice(4, 60).slice(6, 20);
  eq(t, "brown fox jump", "slice of atom slice");
  eq(ALIT.slice(4).substring(2, 8), "ick br", "substring of atom slice");
  eq(ALIT.slice(4).substr(2, 5), "ick b", "substr of atom slice");
});
test("atom_slice_search_replace", function () {
  var t = ALIT.slice(0, 50);
  eq(t.replace("fox", "cat"), "The quick brown cat jumps over the lazy dog. The q", "replace");
  eq(t.replaceAll("quick", "slow"), "The slow brown fox jumps over the lazy dog. The q", "replaceAll");
  eq(t.split(" ").length, 11, "split");
  eq(t.toLowerCase().substring(0, 9), "the quick", "toLowerCase");
  eq(t.toUpperCase().substring(0, 9), "THE QUICK", "toUpperCase");
});
test("atom_slice_json_template", function () {
  var t = ALIT.slice(4, 24);
  eq(JSON.stringify(t), '"quick brown fox jump"', "JSON.stringify");
  eq(JSON.parse(JSON.stringify({ s: t })).s, t, "JSON round-trip");
  eq(`[${t}]`, "[quick brown fox jump]", "template literal");
});
test("atom_slice_keys", function () {
  var key = ALIT.slice(4, 44); /* 40 chars, not a canonical number */
  var o = {};
  o[key] = 1;
  eq(o[ALIT.slice(4, 44)], 1, "slice key round-trip");
  eq(Object.keys(o)[0], key, "key enumeration");
  var m = new Map();
  m.set(BLIT.slice(5, 85), "v");
  eq(m.get(BLIT.slice(5, 85)), "v", "Map key from atom slice");
  var st = new Set();
  st.add(ALIT.slice(4, 44));
  ok(st.has(ALIT.slice(4, 44)), "Set has slice");
});
test("atom_slice_numeric_keys", function () {
  var o = {};
  o[BLIT.slice(1, 3)] = "a"; /* "12" -> canonical key 12 */
  eq(o[12], "a", "numeric slice key canonicalizes");
  eq(o["12"], "a", "string numeric key");
});
test("atom_slice_wide_literal", function () {
  var W = "\u0101\u0103\u0105\u0107\u0109\u010b\u010d\u010f\u0111\u0113\u0115\u0117\u0119\u011b\u011d\u011f\u0121\u0123\u0125\u0127\u0129\u012b\u012d\u012f\u0131\u0133\u0135\u0137\u0139\u013b\u013d\u013f\u0141\u0143\u0145\u0147\u0149\u014b\u014d\u014f\u0151\u0153\u0155\u0157\u0159\u015b\u015d\u015f\u0161\u0163\u0165\u0167\u0169\u016b\u016d\u016f\u0171\u0173\u0175\u0177\u017a\u017c\u017e\u0100\u0102\u0104\u0106\u0108\u010a\u010c\u010e\u0110\u0112\u0114";
  var t = W.slice(10, 60);
  eq(t.length, 50, "wide atom slice length");
  eq(t.charCodeAt(0), 0x0115, "wide atom slice charCodeAt");
  ok(t === W.slice(10, 60), "wide atom slice content eq");
  eq(t.indexOf(String.fromCharCode(0x0127)), 9, "wide indexOf");
});
test("atom_slice_astral", function () {
  var A = "\u{1F600}\u{1F601}\u{1F602}\u{1F603}\u{1F604}\u{1F605}\u{1F606}\u{1F607}\u{1F608}\u{1F609}\u{1F60A}\u{1F60B}\u{1F60C}\u{1F60D}\u{1F60E}\u{1F60F}\u{1F610}\u{1F611}\u{1F612}\u{1F613}\u{1F614}\u{1F615}\u{1F616}\u{1F617}\u{1F618}\u{1F619}\u{1F61A}\u{1F61B}\u{1F61C}\u{1F61D}\u{1F61E}\u{1F61F}\u{1F620}\u{1F621}\u{1F622}\u{1F623}";
  var t = A.slice(4, 40);
  eq(t.length, 36, "astral atom slice unit length");
  eq([...t].length, 18, "astral atom slice codepoint count");
  eq(t.codePointAt(0), 0x1F602, "codePointAt");
  eq(t.indexOf("\u{1F60A}"), 16, "astral indexOf");
});
summary("sliced_waveB");
