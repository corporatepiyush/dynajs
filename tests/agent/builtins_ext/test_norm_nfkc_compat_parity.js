// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["0 NFKC=[1\u20442] NFKDhash=a253c1ce NFCeq=true", "1 NFKC=[1\u20444] NFKDhash=a453c4f4 NFCeq=true", "2 NFKC=[2\u20443] NFKDhash=58ca9f50 NFCeq=true", "3 NFKC=[IV] NFKDhash=68e907b6 NFCeq=true", "4 NFKC=[fi] NFKDhash=5c22ded0 NFCeq=true", "5 NFKC=[fl] NFKDhash=6122e6af NFCeq=true", "6 NFKC=[ffi] NFKDhash=ac09484c NFCeq=true", "7 NFKC=[1] NFKDhash=340ca71c NFCeq=true", "8 NFKC=[2] NFKDhash=370cabd5 NFCeq=true", "9 NFKC=[17] NFKDhash=1aeb28b1 NFCeq=true", "10 NFKC=[MHz] NFKDhash=3842653e NFCeq=true", "11 NFKC=[cm2] NFKDhash=cc9215cd NFCeq=true", "12 NFKC=[ABc] NFKDhash=7c8461cb NFCeq=true", "13 NFKC=[\u03a9] NFKDhash=ac0d6404 NFCeq=false", "14 NFKC=[\u00c5] NFKDhash=64d3e6b2 NFCeq=false", "15 NFKC=[3] NFKDhash=360caa42 NFCeq=true", "16 NFKC=[x2] NFKDhash=f630cef NFCeq=true", "17 NFKC=[123 1\u20442 + fi] NFKDhash=df6281b8 NFCeq=true", "18 NFKC=[gal] NFKDhash=3c170eeb NFCeq=true", "19 NFKC=[\u20a9] NFKDhash=ac3e3104 NFCeq=true", "20 NFKC=[\u00a2] NFKDhash=a70bc925 NFCeq=true"];
__EXP[1] = ["half=[1\u20442] len=3 ok=true"];
__EXP[2] = ["hash=9cbbccd5"];
// test_norm_nfkc_compat_parity.js — NFKC/NFKD compatibility decomposition
// parity vs node: fractions, ligatures, circled, squared, fullwidth, Ohm,
// superscripts. The "½".normalize("NFKC") parity row is mandatory.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var samples = [
  "\u00bd", "\u00bc", "\u2154", "\u2163",
  "\ufb01", "\ufb02", "\ufb03",
  "\u2460", "\u2461", "\u2470",
  "\u3392", "\u33a0",
  "\uff21\uff22\uff43",
  "\u2126", "\u212b",
  "\u00b3", "x\u00b2",
  "\u2460\u2461\u2462 \u00bd + \ufb01",
  "\u33ff", "\uffe6", "\uffe0"
];
for (var i = 0; i < samples.length; i++) {
  var s = samples[i];
  __L(0, i + " NFKC=[" + s.normalize("NFKC") + "] NFKDhash=" + fnv(s.normalize("NFKD")) +
    " NFCeq=" + (s.normalize("NFC") === s));
  feed += i + ":" + fnv(s.normalize("NFKC")) + ":" + fnv(s.normalize("NFKD")) + "\n";
}
// mandatory parity row: "½".normalize("NFKC") must be the 3-char "1⁄2"
var halfNFKC = "\u00bd".normalize("NFKC");
__L(1, "half=[" + halfNFKC + "] len=" + halfNFKC.length + " ok=" + (halfNFKC === "1\u20442"));
feed += "half=" + halfNFKC + "\n";
__L(2, "hash=" + fnv(feed));

summary("builtins_ext");
