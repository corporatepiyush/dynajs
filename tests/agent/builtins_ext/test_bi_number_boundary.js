// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["maxSafe=OK:9007199254740991"];
__EXP[1] = ["minSafe=OK:-9007199254740991"];
__EXP[2] = ["numMaxInt=OK:9007199254740992"];
__EXP[3] = ["num2p53=OK:9007199254740992"];
__EXP[4] = ["num2p53m1=OK:9007199254740991"];
__EXP[5] = ["biHalf=THROW:RangeError"];
__EXP[6] = ["bi1e21=OK:1000000000000000000000"];
__EXP[7] = ["bi1e100=OK:10000000000000000159028911097599180468360808563945281389781327557747838772170381060813469985856815104"];
__EXP[8] = ["biNegHalf=THROW:RangeError"];
__EXP[9] = ["biNaN=THROW:RangeError"];
__EXP[10] = ["biInf=THROW:RangeError"];
__EXP[11] = ["biStr=OK:123456789012345678901234567890"];
__EXP[12] = ["biStrNeg=OK:-123456789012345678901234567890"];
__EXP[13] = ["biStrHex=OK:255"];
__EXP[14] = ["biStrOct=OK:15"];
__EXP[15] = ["biStrBin=OK:5"];
__EXP[16] = ["biStrBad=THROW:SyntaxError"];
__EXP[17] = ["biTrue=OK:1"];
__EXP[18] = ["bi0=OK:0"];
__EXP[19] = ["Number-negBig=OK:-1152921504606847000"];
__EXP[20] = ["parse=OK:1.2345678901234568e+29"];
__EXP[21] = ["fails=0"];
// test_bi_number_boundary.js — BigInt<->Number conversion boundaries and the
// error classes for non-integer conversions (message text impl-defined ->
// constructor name only). Byte-identical vs node.
function fails0(){ return fails; }
var fails = 0;
function probe(f) {
  try { return "OK:" + String(f()); }
  catch (e) { return "THROW:" + e.constructor.name; }
}
__L(0, "maxSafe=" + probe(function () { return BigInt(Number.MAX_SAFE_INTEGER); }));
__L(1, "minSafe=" + probe(function () { return BigInt(Number.MIN_SAFE_INTEGER); }));
__L(2, "numMaxInt=" + probe(function () { return Number(BigInt(2) ** 53n); }));
__L(3, "num2p53=" + probe(function () { return Number(BigInt(2) ** 53n + 1n); })); // rounds
__L(4, "num2p53m1=" + probe(function () { return Number(BigInt(2) ** 53n - 1n); }));
__L(5, "biHalf=" + probe(function () { return BigInt(1.5); }));
__L(6, "bi1e21=" + probe(function () { return BigInt(1e21); }));
__L(7, "bi1e100=" + probe(function () { return BigInt(1e100); }));
__L(8, "biNegHalf=" + probe(function () { return BigInt(-0.5); }));
__L(9, "biNaN=" + probe(function () { return BigInt(NaN); }));
__L(10, "biInf=" + probe(function () { return BigInt(Infinity); }));
__L(11, "biStr=" + probe(function () { return BigInt("123456789012345678901234567890"); }));
__L(12, "biStrNeg=" + probe(function () { return BigInt("-123456789012345678901234567890"); }));
__L(13, "biStrHex=" + probe(function () { return BigInt("0xff"); }));
__L(14, "biStrOct=" + probe(function () { return BigInt("0o17"); }));
__L(15, "biStrBin=" + probe(function () { return BigInt("0b101"); }));
__L(16, "biStrBad=" + probe(function () { return BigInt("12x"); }));
__L(17, "biTrue=" + probe(function () { return BigInt(true); }));
__L(18, "bi0=" + probe(function () { return BigInt(-0); }));
__L(19, "Number-negBig=" + probe(function () { return Number(-(2n ** 60n)); }));
__L(20, "parse=" + probe(function () { return parseInt("123456789012345678901234567890"); }));
__L(21, "fails=" + fails0());

summary("builtins_ext");
