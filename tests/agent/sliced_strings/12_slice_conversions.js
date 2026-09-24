// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 12_slice_conversions"];
/* String()/Number()/Boolean() conversions and comparisons of slices. */
var s = parent(500);
var t = s.slice(10, 60);
eq(String(t), t, "String()");
eq("" + t, t, "concat coercion");
eq(`${t}`, t, "template coercion");
eq(Number("  123  ".slice(0, 50).trim() === "123" ? "123" : "x"), 123, "number via slice");
eq(Number("42abc".slice(0, 2)), 42, "Number of slice");
eq(Number(t.slice(0, 0)), 0, "Number of empty slice");
ok(!!t, "slice truthy");
ok(!t.slice(0, 0), "empty slice falsy");
/* relational comparisons compare by value */
var a = "aaa".repeat(100).slice(0, 99);
var b = "aab".repeat(100).slice(0, 99);
ok(a < b, "lt");
ok(b > a, "gt");
ok(a <= a.slice(0), "le self");
ok(("abc" + parent(100)).slice(0, 3) === "abc", "eq");
/* trim/pad on slices */
var sp = "   x   ".repeat(100);
eq(sp.slice(0, 7).trim(), "x", "trim of slice");
eq(sp.slice(700, 707).trim(), "", "trim of empty window");
eq(t.slice(0, 5).padStart(10, "."), "....." + s.slice(10, 15), "padStart");
eq(t.slice(0, 5).padEnd(10, "."), s.slice(10, 15) + ".....", "padEnd");
/* repeat from slices */
eq(t.slice(0, 5).repeat(3), s.slice(10, 15) + s.slice(10, 15) + s.slice(10, 15), "repeat of slice");
/* localeCompare */
eq(t.slice(0, 3).localeCompare(t.slice(0, 3)), 0, "localeCompare self");
/* encode/decode */
eq(encodeURI(t.slice(0, 20)), encodeURI(s.slice(10, 30)), "encodeURI");
eq(decodeURIComponent(encodeURIComponent(t)), t, "uri roundtrip");
/* error messages embed slices */
try { null.x } catch (e) { ok(String(e).length > 0, "error string"); }
/* boolean/String coercion of slices inside arrays join */
eq([t.slice(0, 2), t.slice(2, 4)].join("-"), s.slice(10, 12) + "-" + s.slice(12, 14), "join slices");
__L(0, "PASS 12_slice_conversions");

summary("sliced_strings");
