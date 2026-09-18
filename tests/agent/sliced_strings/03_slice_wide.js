// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 03_slice_wide"];
/* wide (UTF-16) and latin1 parents; slices across the boundary. */
var w = parentWide(2000);
var latin = parent(2000);
var ws = w.slice(100, 1500);
eq(ws.length, 1400, "wide slice len");
eq(ws.charCodeAt(0), w.charCodeAt(100), "wide first");
ok(ws > "\u0100", "wide compare sanity");
/* latin1 slice of a wide parent region still reads correctly */
var w2 = "\u00e9\u00e8x".repeat(400);           /* all <= 0xFF but wide repr */
var w2s = w2.slice(3, 900);
eq(w2s, "\u00e9\u00e8x".repeat(299), "narrowable wide slice value");
eq(w2s.length, 897, "narrowable wide slice len");
/* concat of wide slice with narrow string */
var mixed = w2s + "abc";
eq(mixed.length, 900, "mixed concat len");
eq(mixed.charCodeAt(0), w2.charCodeAt(3), "mixed concat read");
/* slice containing astral chars mixed with latin1 */
var em = "a\u00e9\u4e16\ud83d\ude00b".repeat(300);
var ems = em.slice(2, 1500);
eq(ems.length, 1498, "mixed repr slice len");
eq(ems.codePointAt(0), 0x4e16, "mixed slice CJK");
eq(ems.codePointAt(1), 0x1f600, "mixed slice emoji");
/* wide slice stays itself under toUpperCase (non-ascii path) */
eq(w2s.toUpperCase(), w2.slice(3, 900).toUpperCase(), "wide slice toUpperCase");
__L(0, "PASS 03_slice_wide");

summary("sliced_strings");
