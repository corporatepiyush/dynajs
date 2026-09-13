// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 05_slice_search"];
/* indexOf/lastIndexOf/includes/startsWith/endsWith/replace/split on slices. */
var s = parent(3000) + "NEEDLE" + parent(3000);
var t = s.slice(100, s.length - 100);
var i = t.indexOf("NEEDLE");
ok(i > 0, "indexOf on slice");
eq(t.indexOf("NEEDLE", i + 1), -1, "indexOf from after");
eq(t.lastIndexOf("NEEDLE"), i, "lastIndexOf on slice");
eq(t.lastIndexOf("NEEDLE", i + 2), i, "lastIndexOf bounded");
ok(t.includes("NEEDLE"), "includes");
ok(t.includes(String.fromCharCode(97 + (100 % 26)) + String.fromCharCode(97 + (101 % 26))), "includes at seam");
ok(t.startsWith(s.slice(100, 110)), "startsWith slice arg");
ok(t.endsWith(s.slice(s.length - 110, s.length - 100)), "endsWith slice arg");
/* replace with string and with regexp on slices */
eq(t.replace("NEEDLE", "X"), s.slice(100, s.length - 100).replace("NEEDLE", "X"), "replace literal");
eq(t.replace(/N+(E+)D/g, "|$1|"), t.replace(/N+(E+)D/g, function (m, e) { return "|" + e + "|"; }), "replace regexp vs fn");
/* GetSubstitution capture arithmetic on slices */
eq("aaaNEEDLEbbb".slice(2).replace(/(NE)(ED)LE/, "$2$1"), "aEDNEbbb", "capture swap on slice");
/* split */
deepEq(t.split("NEEDLE"), [t.slice(0, i), t.slice(i + 6)], "split on slice");
deepEq(t.split(""), Array.from(t), "split empty on slice");
/* slice with regexp-special chars */
var spec = "a.b*".repeat(500);
eq(spec.slice(3, 1000).indexOf("."), 2, "indexOf special char in slice");
__L(0, "PASS 05_slice_search");

summary("sliced_strings");
