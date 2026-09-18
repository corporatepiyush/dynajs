// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 18_slice_widesearch"];
/* search and replace on WIDE slices. */
var w = "";
for (var i = 0; i < 3000; i++) w += String.fromCharCode(0x400 + (i % 100));
w += "MARK\u4e16MARK";
for (var i2 = 0; i2 < 3000; i2++) w += String.fromCharCode(0x400 + (i2 % 100));
var t = w.slice(100, w.length - 100);
var i3 = t.indexOf("MARK");
ok(i3 > 0, "wide indexOf");
eq(t.indexOf("MARK", i3 + 4), i3 + 5, "second MARK");
eq(t.lastIndexOf("\u4e16"), i3 + 4, "lastIndexOf CJK");
eq(t.replace("MARK", "X").indexOf("X"), i3, "wide replace");
deepEq(t.split("MARK").length, 3, "wide split");
ok(t.includes("MARK\u4e16"), "wide includes");
/* startsWith/endsWith with wide args */
ok(t.startsWith(t.slice(0, 5)), "wide startsWith self");
/* wide slice compare */
var cmp = t.slice(0, 10) < w.slice(101, 111);
ok(typeof cmp === "boolean", "wide relational");
__L(0, "PASS 18_slice_widesearch");

summary("sliced_strings");
