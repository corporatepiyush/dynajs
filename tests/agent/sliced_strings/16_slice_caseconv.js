// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 16_slice_caseconv"];
/* case conversion on slices: ASCII fast path, wide, final sigma. */
var s = parent(2000);
var t = s.slice(50, 1500);
eq(t.toUpperCase(), s.slice(50, 1500).toUpperCase(), "ascii toUpperCase");
eq(t.toLowerCase(), s.slice(50, 1500).toLowerCase(), "ascii toLowerCase");
/* wide with ascii prefix */
var w = "asciix".repeat(300) + "\u00c9" + parentWide(300);
var ws = w.slice(100, 1500);
eq(ws.toUpperCase(), w.slice(100, 1500).toUpperCase(), "wide toUpperCase");
/* final sigma context inside a slice */
var g = "\u03a3" + "a".repeat(100) + "\u03a3" + "b".repeat(100);
var gs = g.slice(1, 200);
var gsl = gs.toLowerCase();
ok(gsl.indexOf("\u03c2") >= 0 || gsl.indexOf("\u03c3") >= 0, "sigma present");
eq(gs.toLowerCase(), g.slice(1, 200).toLowerCase(), "final sigma matches base");
/* turkish-ish and mixed cases stay consistent */
var mix = "AbC".repeat(300).slice(5, 500);
eq(mix.toLowerCase(), mix.toUpperCase().toLowerCase(), "case roundtrip");
eq(mix.toLowerCase(), "abc".repeat(300).slice(5, 500), "mixed toLowerCase value");
__L(0, "PASS 16_slice_caseconv");

summary("sliced_strings");
