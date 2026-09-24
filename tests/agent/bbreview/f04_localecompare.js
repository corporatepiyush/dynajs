// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["lc1 false 0 0"];
__EXP[1] = ["lc2 2 1"];
__EXP[2] = ["lc3 200 200 0"];
__EXP[3] = ["lc4 -1 1 0"];
__EXP[4] = [["lc5 31 111", "lc5 -1 -1"]];
__EXP[5] = [["lc6 19916 0", "lc6 1 0"]];
__EXP[7] = ["lc8 0 false"];
__EXP[8] = ["lc9 0 0"];
__EXP[9] = ["lc10 -1 1 0"];
__EXP[10] = [["lc11 -1 25 0", "lc11 -1 1 0"]];
__REQ = {"dynajs": {"0": 1, "1": 1, "2": 1, "3": 1, "4": 1, "5": 1, "7": 1, "8": 1, "9": 1, "10": 1}, "node": {"0": 1, "1": 1, "2": 1, "3": 1, "4": 1, "5": 1, "7": 1, "8": 1, "9": 1, "10": 1}};
// F: localeCompare correctness — ASCII raw compare must NOT bypass NFC normalization
const a = "e\u0301";
const b = "\u00e9";
__L(0, "lc1", a === b, a.localeCompare(b), b.localeCompare(a));
__L(1, "lc2", a.length, b.length);
const w1 = "\u00e9\u4e2d\u6587".repeat(70).slice(0, 200);
const w2 = "\u00e9\u4e2d\u6587".repeat(70).slice(0, 200);
__L(2, "lc3", w1.length, w2.length, w1.localeCompare(w2));
__L(3, "lc4", "a".localeCompare("b"), "b".localeCompare("a"), "a".localeCompare("a"));
// KNOWN divergence: 'a' vs 'B' sign (ICU vs codepoint) — documented, skip strict check
__L(4, "lc5", "a".localeCompare("B"), "\u00e9".localeCompare("z"));
__L(5, "lc6", "\u4e2d".localeCompare("a"), "\u4e2d".localeCompare("\u4e2d"));
const c = "abc" + "e\u0301";
const d2 = "abc" + "\u00e9";
__A("f04_localecompare.js:lc7", function () { assert_eq(c.localeCompare(d2), 0, "lc7"); });
// NFD forms of CJK-compat and long asymmetric pairs
const e = "\u4e2d\u6587" + "a\u0301";
const f = "\u4e2d\u6587" + "\u00e1";
__L(7, "lc8", e.localeCompare(f), e === f);
// mixed NFC/NFD both directions with longer NFD prefix
__L(8, "lc9", "a\u0301bc".localeCompare("\u00e1bc"), "\u00e1bc".localeCompare("a\u0301bc"));
// wide strings differing AFTER a long equal prefix (raw-compare fast path must still be correct)
const base = "\u00e9x".repeat(60);
__L(9, "lc10", (base + "a").localeCompare(base + "b"), (base + "b").localeCompare(base + "a"), (base + "a").localeCompare(base + "a"));
// ASCII pairs (fast path) sanity
__L(10, "lc11", "abc".localeCompare("abd"), "zzz".localeCompare("aaa"), "".localeCompare(""));

summary("bbreview");
