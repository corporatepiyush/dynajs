__EXP = {};
__EXP[0] = ["PASS 04_slice_astral"];
var em = "\ud83d\ude00" + parent(200) + "\ud83d\ude02" + parent(300) + "\ud83e\uddfd";
var t = em.slice(2, 480);
eq(t.length, 478, "astral slice len");
eq(t.codePointAt(0), 97, "ascii start");
var idx = -1;
for (var i = 0; i < t.length - 1; i++) {
  if (t.charCodeAt(i) === 0xd83d && t.charCodeAt(i + 1) === 0xde02) { idx = i; break; }
}
ok(idx > 0, "found emoji in slice");
eq(t.codePointAt(idx), 0x1f602, "codePointAt emoji");
eq(t.codePointAt(idx + 1), 0xde02, "low surrogate codePointAt");
eq(t.charCodeAt(idx), 0xd83d, "high surrogate unit");
var chars = Array.from(t);
ok(chars.some(function (c) { return c.codePointAt(0) === 0x1f602; }), "Array.from emoji");
var t2 = t.slice(idx - 1, idx + 3);
eq(t2.length, 4, "pair sub-slice len");
eq(t2.charCodeAt(1), 0xd83d, "pair sub-slice high");
eq(t2.charCodeAt(2), 0xde02, "pair sub-slice low");
eq(Array.from(t2).length, 3, "pair sub-slice codepoints");
eq([].map.call(t2, function (c) { return c.codePointAt(0).toString(16); }).join(","), "72,d83d,de02,61", "unit walk");
eq(Array.from(t2).map(function (c) { return c.codePointAt(0).toString(16); }).join(","), "72,1f602,61", "codepoint walk");
eq(String.fromCodePoint(t.codePointAt(idx)), "\ud83d\ude02", "fromCodePoint roundtrip");
__L(0, "PASS 04_slice_astral");

summary("sliced_strings");
