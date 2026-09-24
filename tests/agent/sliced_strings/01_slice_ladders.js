// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 01_slice_ladders"];
/* slice-of-slice ladders: depth 100 chains must resolve to root values. */
var s = parent(5000);
var t = s.slice(100, 4000);
for (var i = 0; i < 100; i++) t = t.slice(10, t.length - 1);
eq(t.length, 2800, "ladder length");
eq(t[0], s[100 + 100 * 10], "ladder first char");
eq(t, s.slice(1100, 3900), "ladder value");
/* substring chains */
var a = s.substring(0, 4000).substring(10, 3000).substring(20, 2000);
eq(a, s.slice(30, 2010), "substring chain");
/* mixed producers */
var b = s.substr(500, 2000).slice(10, 1900).substring(5, 1000);
eq(b, s.slice(515, 1510), "mixed chain");
/* slice of full-length dup (start==0 && end==len returns receiver itself) */
var w = s.slice(100, 4000);
eq(w.slice(0, w.length), w, "full-range slice of slice");
/* ladder keeping root alive after intermediates would be collectable */
var ladder = s;
for (var j = 0; j < 30; j++) ladder = ladder.substring(j, ladder.length - j);
eq(ladder.length, s.length - 2 * 435, "root ladder length");
eq(ladder, s.slice(435, s.length - 435), "root ladder value");
__L(0, "PASS 01_slice_ladders");

summary("sliced_strings");
