// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["nfc=OK:abc"];
__EXP[1] = ["nfd=OK:abc"];
__EXP[2] = ["nfkc=OK:abc"];
__EXP[3] = ["nfkd=OK:abc"];
__EXP[4] = ["lower-nfc=THROW:RangeError"];
__EXP[5] = ["nfx=THROW:RangeError"];
__EXP[6] = ["empty=THROW:RangeError"];
__EXP[7] = ["undef=OK:abc"];
__EXP[8] = ["missing=OK:abc"];
__EXP[9] = ["num=THROW:RangeError"];
__EXP[10] = ["obj=OK:abc"];
__EXP[11] = ["objThrow=THROW:TypeError"];
__EXP[12] = ["null=THROW:RangeError"];
__EXP[13] = ["fails=0"];
// test_norm_badform_throw.js — invalid normalization form arguments: unknown
// string, undefined (defaults to NFC), numbers, objects. Error CLASS is spec'd;
// message text is impl-defined → print constructor name only.
function fails_count() { return fails; }
var fails = 0;
function probe(arg) {
  try {
    var out = "abc".normalize(arg);
    return "OK:" + out;
  } catch (e) {
    return "THROW:" + e.constructor.name;
  }
}
__L(0, "nfc=" + probe("NFC"));
__L(1, "nfd=" + probe("NFD"));
__L(2, "nfkc=" + probe("NFKC"));
__L(3, "nfkd=" + probe("NFKD"));
__L(4, "lower-nfc=" + probe("nfc"));
__L(5, "nfx=" + probe("NFX"));
__L(6, "empty=" + probe(""));
__L(7, "undef=" + probe(undefined));
__L(8, "missing=" + probe());
__L(9, "num=" + probe(42));
__L(10, "obj=" + probe({ toString: function () { return "NFC"; } }));
__L(11, "objThrow=" + probe({ toString: function () { throw new TypeError("x"); } }));
__L(12, "null=" + probe(null));
__L(13, "fails=" + fails_count());

summary("builtins_ext");
