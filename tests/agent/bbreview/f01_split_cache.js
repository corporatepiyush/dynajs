// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["s1 100000 a j"];
__EXP[1] = ["s2 100000 j false"];
__EXP[2] = ["c0 1 1 \"\\u0000\" 0 true", "c41 1 1 \"A\" 65 true", "c7f 1 1 \"\u007f\" 127 true", "c80 1 1 \"\u0080\" 128 true", "cc3 1 1 \"\u00c3\" 195 true", "cff 1 1 \"\u00ff\" 255 true", "c100 1 1 \"\u0100\" 256 true", "c2028 1 1 \"\u2028\" 8232 true", "cd800 1 1 \"\\ud800\" 55296 true", "cdc00 1 1 \"\\udc00\" 56320 true", "c1f600 1 1 \"\uf600\" 62976 true", "c10ffff 1 1 \"\uffff\" 65535 true"];
__EXP[3] = ["m7f_80 127,128 127 128", "mff_100 255,256 255 256", "m0_41 0,65 0 65"];
__EXP[4] = ["w d83d,de00,d83d,de01,78 5"];
__EXP[5] = [["~rj", "~rj"]];
__EXP[6] = ["s80 2 128,97 128"];
// F: split("") cache reuse + char boundary sweep
const big = "abcdefghij".repeat(10000);
let t1 = big.split("");
__L(0, "s1", t1.length, t1[0], t1[99999]);
let t2 = big.split("");
__L(1, "s2", t2.length, t2[49999], t1 === t2);
const codes = [0x00, 0x41, 0x7F, 0x80, 0xC3, 0xFF, 0x100, 0x2028, 0xD800, 0xDC00, 0x1F600, 0x10FFFF];
for (const c of codes) {
  const s = String.fromCharCode(c);
  const parts = s.split("");
  __L(2, "c" + c.toString(16), s.length, parts.length, JSON.stringify(s), s.charCodeAt(0), s.charAt(0) === s);
}
for (const [a, b] of [[0x7F, 0x80], [0xFF, 0x100], [0x00, 0x41]]) {
  const s = String.fromCharCode(a, b);
  __L(3, "m" + a.toString(16) + "_" + b.toString(16), s.split("").map(x => x.charCodeAt(0)).join(","), s.charAt(0).charCodeAt(0), s.charAt(1).charCodeAt(0));
}
const w = "\u{1F600}\u{1F601}x";
__L(4, "w", w.split("").map(x => x.codePointAt(0).toString(16)).join(","), w.split("").length);
// split("") then join round trip
__L(5, "rj", t1.join("") === big, t2.join("").length);
// split of 1-char strings at 0x80 boundary used in charAt chains
const s80 = String.fromCharCode(0x80) + "a";
__L(6, "s80", s80.length, s80.split("").map(x => x.charCodeAt(0)).join(","), s80.charAt(0).charCodeAt(0));

summary("bbreview");
