// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 02_slice_reads"];
/* charCodeAt/charAt/codePointAt/at/[i] on slices (hot set, zero-copy). */
var s = parent(3000);
var t = s.slice(100, 2000);
eq(t.length, 1900, "len");
for (var i = 0; i < 1900; i++) eq(t.charCodeAt(i), s.charCodeAt(100 + i), "charCodeAt " + i);
ok(isNaN(t.charCodeAt(1900)), "charCodeAt OOB");
eq(t.charAt(0), s.charAt(100), "charAt");
eq(t.charAt(-1), "", "charAt negative");
eq(t.codePointAt(5), s.codePointAt(105), "codePointAt");
eq(t.at(0), s.at(100), "at");
eq(t.at(-1), s.at(1999), "at negative");
eq(t[0], s[100], "index get");
eq(t[1899], s[1999], "index get last");
eq(t[1900], undefined, "index OOB");
eq(t["0"], s[100], "string index");
eq(t.length, 1900, "length prop");
ok(!("5" in Object.getPrototypeOf(t)), "no proto index");
/* bracket access beyond slice but inside parent must be undefined */
eq(t[3000], undefined, "index past parent");
/* unicode reads */
var w = parentWide(1000);
var ws = w.slice(50, 900);
for (var k = 0; k < 850; k++) eq(ws.charCodeAt(k), w.charCodeAt(50 + k), "wide charCodeAt " + k);
__L(0, "PASS 02_slice_reads");

summary("sliced_strings");
