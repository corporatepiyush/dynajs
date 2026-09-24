// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["n2 3 31,2044,32"];
__EXP[3] = ["n4 true \"e\u0301\""];
__EXP[5] = ["n6 2 true"];
__EXP[7] = ["n8 abc abc"];
__EXP[11] = ["n11 true true"];
__EXP[12] = ["n12 true true 200"];
__EXP[13] = ["n13 \u00bd 1\u20442"];
__EXP[14] = ["n14 234 IX"];
// F: normalize paths (compatibility decomposition must NOT be treated as ASCII)
__A("f05_normalize.js:n1", function () { assert_eq("\u00bd".normalize("NFKD"), "1\u20442", "n1"); });
__L(1, "n2", "\u00bd".normalize("NFKD").length, [..."\u00bd".normalize("NFKD")].map(c => c.codePointAt(0).toString(16)).join(","));
__A("f05_normalize.js:n3", function () { assert_eq("\u00bd".normalize("NFC") === "\u00bd", true, "n3"); });
__L(3, "n4", "e\u0301".normalize() === "\u00e9", JSON.stringify("\u00e9".normalize("NFD")));
__A("f05_normalize.js:n5", function () { assert_eq("\uFB01".normalize("NFKD"), "fi", "n5"); });
__L(5, "n6", "\u00c5".normalize("NFD").length, "\u212b".normalize("NFC") === "\u00c5");
__A("f05_normalize.js:n7", function () { assert_eq("\u1E9B\u0323".normalize("NFC").codePointAt(0).toString(16), "1e9b", "n7"); });
__L(7, "n8", "abc".normalize(), "abc".normalize("NFKC"));
__A("f05_normalize.js:n9", function () { assert_eq("\uFDFA".normalize("NFKD").length, 18, "n9"); });
try { "x".normalize("BOGUS"); __L(9, "n10", "no-throw"); } catch (e) { __A("f05_normalize.js:n10", function () { assert_eq(e.constructor.name, "RangeError", "n10"); }); }
const as = "abc123".repeat(33).slice(0, 200);
__L(11, "n11", as.normalize() === as, as.normalize("NFD") === as);
const em = "\u{1F600}\u{1F601}".repeat(50);
__L(12, "n12", em.normalize("NFC") === em, em.normalize("NFD") === em, em.length);
__L(13, "n13", "\u00bd".normalize(), "\u00bd".normalize("NFKC"));
// NFKD of superscript digits and roman numerals
__L(14, "n14", "\u00b2\u00b3\u2074".normalize("NFKD"), "\u2168".normalize("NFKD"));

summary("bbreview");
