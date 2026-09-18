// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 10_slice_atoms"];
/* atoms-vs-slices: identity/interning of identical substrings. */
var s = "literal-marker-" + parent(2000) + "-tail";
var t = s.slice(0, 14);
eq(t, "literal-marker", "slice value");
ok(t === "literal-marker", "slice === identical literal (value equality)");
/* property keys built from slices intern correctly */
var obj = {};
var key = ("key_" + parent(100) + "_end").slice(0, 50);
obj[key] = 1;
var key2 = ("key_" + parent(100) + "_end").slice(0, 50);
eq(obj[key2], 1, "interned slice key");
eq(Object.keys(obj)[0], key2, "key value equal");
/* symbol-style identity via String() of slice vs literal */
eq(String(t), "literal-marker", "String(slice)");
/* object lookup with slice keys vs literal keys */
var m = {};
m["abcdefgh".repeat(40).slice(0, 20)] = "v";
eq(m["abcdefgh".repeat(40).slice(0, 20)], "v", "map-by-slice-key");
eq(m["abcdefghabcdefghabcd"], "v", "map-by-literal-key");
/* number-like slices canonicalize to canonical numeric keys */
var o2 = {};
o2[(parent(50) + "42").slice(50)] = "forty-two";
eq(o2[42], "forty-two", "numeric slice key");
eq(o2["42"], "forty-two", "string numeric slice key");
/* slices as map/set values keep value equality */
var set = new Set();
set.add(parent(100).slice(10, 60));
ok(set.has(parent(100).slice(10, 60)), "set has slice");
var map = new Map();
map.set(("m" + parent(300)).slice(1, 200), 7);
eq(map.get(("m" + parent(300)).slice(1, 200)), 7, "map get slice");
__L(0, "PASS 10_slice_atoms");

summary("sliced_strings");
