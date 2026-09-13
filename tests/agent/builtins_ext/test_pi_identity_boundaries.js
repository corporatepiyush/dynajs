// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["0 in=0 out=0 isNegZero=false strPathEq=true sameZero=true", "1 in=0 out=0 isNegZero=false strPathEq=true sameZero=true", "2 in=0 out=0 isNegZero=false strPathEq=true sameZero=true", "3 in=1 out=1 isNegZero=false strPathEq=true sameZero=true", "4 in=-1 out=-1 isNegZero=false strPathEq=true sameZero=true", "5 in=42 out=42 isNegZero=false strPathEq=true sameZero=true", "6 in=-42 out=-42 isNegZero=false strPathEq=true sameZero=true", "7 in=2147483647 out=2147483647 isNegZero=false strPathEq=true sameZero=true", "8 in=2147483648 out=2147483648 isNegZero=false strPathEq=true sameZero=true", "9 in=-2147483648 out=-2147483648 isNegZero=false strPathEq=true sameZero=true", "10 in=-2147483649 out=-2147483649 isNegZero=false strPathEq=true sameZero=true", "11 in=4503599627370495 out=4503599627370495 isNegZero=false strPathEq=true sameZero=true", "12 in=4503599627370496 out=4503599627370496 isNegZero=false strPathEq=true sameZero=true", "13 in=-4503599627370496 out=-4503599627370496 isNegZero=false strPathEq=true sameZero=true", "14 in=9007199254740991 out=9007199254740991 isNegZero=false strPathEq=true sameZero=true", "15 in=9007199254740992 out=9007199254740992 isNegZero=false strPathEq=true sameZero=true", "16 in=-9007199254740992 out=-9007199254740992 isNegZero=false strPathEq=true sameZero=true", "17 in=-9007199254740992 out=-9007199254740992 isNegZero=false strPathEq=true sameZero=true", "18 in=9007199254740994 out=9007199254740994 isNegZero=false strPathEq=true sameZero=true", "19 in=1000000000000000 out=1000000000000000 isNegZero=false strPathEq=true sameZero=true", "20 in=-1000000000000000 out=-1000000000000000 isNegZero=false strPathEq=true sameZero=true", "21 in=1e+21 out=1 isNegZero=false strPathEq=true sameZero=true", "22 in=-1e+21 out=-1 isNegZero=false strPathEq=true sameZero=true", "23 in=1e-7 out=1 isNegZero=false strPathEq=true sameZero=true", "24 in=-1e-7 out=-1 isNegZero=false strPathEq=true sameZero=true", "25 in=5e-324 out=5 isNegZero=false strPathEq=true sameZero=true", "26 in=1.7976931348623157e+308 out=1 isNegZero=false strPathEq=true sameZero=true", "27 in=0.5 out=0 isNegZero=false strPathEq=true sameZero=true", "28 in=-0.5 out=0 isNegZero=true strPathEq=true sameZero=true", "29 in=1.5 out=1 isNegZero=false strPathEq=true sameZero=true", "30 in=-1.5 out=-1 isNegZero=false strPathEq=true sameZero=true", "31 in=-0.4 out=0 isNegZero=true strPathEq=true sameZero=true", "32 in=9.999999e+21 out=9 isNegZero=false strPathEq=true sameZero=true"];
__EXP[1] = ["isNegZero(parseInt(-0.4))=true"];
__EXP[2] = ["isNegZero(parseInt(-0))=false"];
__EXP[3] = ["isPosZero(parseInt(0))=true isNeg=false"];
__EXP[4] = ["hash=a30709ea"];
// test_pi_identity_boundaries.js — parseInt(number) identity fast path at the
// integral boundaries: |v| <= 2^53 must behave EXACTLY like ToString round-trip;
// -0 vs +0 semantics via Object.is; e-notation inputs past the fast path.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var inputs = [0, -0, 0.0, 1, -1, 42, -42,
              2147483647, 2147483648, -2147483648, -2147483649,
              4503599627370495, 4503599627370496, -4503599627370496,
              9007199254740991, 9007199254740992, -9007199254740992, -9007199254740993,
              9007199254740994, 1e15, -1e15,
              1e21, -1e21, 1e-7, -1e-7, 5e-324, 1.7976931348623157e308,
              0.5, -0.5, 1.5, -1.5, -0.4, 99.99999e20];
for (var i = 0; i < inputs.length; i++) {
  var v = inputs[i];
  var p = parseInt(v);
  var line = i + " in=" + String(v) + " out=" + String(p) +
    " isNegZero=" + Object.is(p, -0) +
    " strPathEq=" + (p === parseInt(String(v))) +
    " sameZero=" + (Object.is(p, parseInt(String(v))));
  feed += line + "\n";
  __L(0, line);
}
// explicit -0 checks
__L(1, "isNegZero(parseInt(-0.4))=" + Object.is(parseInt(-0.4), -0));
__L(2, "isNegZero(parseInt(-0))=" + Object.is(parseInt(-0), -0));
__L(3, "isPosZero(parseInt(0))=" + Object.is(parseInt(0), +0) + " isNeg=" + Object.is(parseInt(0), -0));
feed += "zeros\n";
__L(4, "hash=" + fnv(feed));

summary("builtins_ext");
