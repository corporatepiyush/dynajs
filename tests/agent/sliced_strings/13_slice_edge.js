// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 13_slice_edge"];
/* edge cases: clamps, negatives, NaN, empty, huge indices. */
var s = parent(1000);
eq(s.slice(500, 500), "", "empty range");
eq(s.slice(999), s.slice(999, 1000), "one char");
eq(s.slice(-5), s.slice(995), "negative start");
eq(s.slice(2, -2), s.slice(2, 998), "negative end");
eq(s.slice(-2, -1), s.slice(998, 999), "both negative");
eq(s.slice(NaN), s.slice(0), "NaN start");
eq(s.slice(0, NaN), "", "NaN end");
eq(s.slice(undefined, 3), s.slice(0, 3), "undefined start");
eq(s.slice(1e9), "", "huge start");
eq(s.slice(3, 1e9), s.slice(3), "huge end");
eq(s.slice(5, 3), "", "inverted range");
eq(s.substring(3, 1), s.slice(1, 3), "substring swaps");
eq(s.substr(5, 0), "", "substr zero len");
eq(s.substr(-3, 2), s.slice(997, 999), "substr negative");
eq(s.substr(5, -1), "", "substr negative len");
eq(s.substr(1e9, 5), "", "substr huge");
eq(s.substring(NaN, NaN), "", "substring NaNs");
/* slices of empty/one-char */
eq("".slice(0, 0), "", "empty parent");
eq("x".slice(0), "x", "one-char");
eq("x".slice(0, 100), "x", "one-char clamp");
/* long slice of a short slice */
eq("ab".slice(100, 200), "", "slice past short");
/* slice then strict-eq against identical freshly-built string */
var lit = "";
for (var i = 0; i < 100; i++) lit += "abcdefghij";
var lit2 = "";
for (var i = 0; i < 100; i++) lit2 += "abcdefghij";
eq(lit.slice(10, 900), lit2.slice(10, 900), "independent slices equal");
/* Symbol.iterator / spread / for-of over slices */
var t = s.slice(100, 200);
var out = [];
for (var c of t) out.push(c);
eq(out.join(""), t, "for-of");
eq([...t].join(""), t, "spread");
eq(Array.from(t).join(""), t, "Array.from");
var it = t[Symbol.iterator]();
eq(it.next().value, s[100], "iterator next");
eq(it.next().done, false, "iterator not done");
/* split("") on slice vs Array.from for BMP */
deepEq(t.split(""), Array.from(t), "split vs Array.from");
__L(0, "PASS 13_slice_edge");

summary("sliced_strings");
