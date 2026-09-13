// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["valueOfThrow=THROW:RangeError"];
__EXP[1] = ["toStringThrow=THROW:TypeError"];
__EXP[2] = ["symbolPrim=OK:18"];
__EXP[3] = ["symbolThrow=THROW:RangeError"];
__EXP[4] = ["sideEff v=18 calls=1"];
__EXP[5] = ["strSide v2=42 calls2=1"];
__EXP[6] = ["hash=12f6a853"];
// test_pi_throwing_radix.js — radix argument that throws during ToInt32
// (valueOf/toString/Symbol.toPrimitive), and a radix object with side effects.
// Behavior class must match node (which hook fires, error class).
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
function probe(radixFactory) {
  try {
    var v = parseInt(12, radixFactory());
    return "OK:" + v;
  } catch (e) {
    return "THROW:" + e.constructor.name;
  }
}
__L(0, "valueOfThrow=" + probe(function () {
  return { valueOf: function () { throw new RangeError("vo"); }, toString: function () { return 16; } };
}));
feed += "valueOfThrow\n";
__L(1, "toStringThrow=" + probe(function () {
  return { toString: function () { throw new TypeError("ts"); } };
}));
feed += "toStringThrow\n";
__L(2, "symbolPrim=" + probe(function () {
  return { [Symbol.toPrimitive]: function (hint) { return 16; }, valueOf: function () { throw new Error("never"); } };
}));
feed += "symbolPrim\n";
__L(3, "symbolThrow=" + probe(function () {
  return { [Symbol.toPrimitive]: function (hint) { throw new RangeError("sym"); } };
}));
feed += "symbolThrow\n";
// side-effecting radix: counts calls, returns 16 on second call
var calls = 0;
var v = parseInt(12, { valueOf: function () { calls++; return 16; } });
__L(4, "sideEff v=" + v + " calls=" + calls);
feed += "sideEff " + calls + "\n";
// string input with side-effecting toString
var calls2 = 0;
var v2 = parseInt({ toString: function () { calls2++; return "42"; } }, 10);
__L(5, "strSide v2=" + v2 + " calls2=" + calls2);
feed += "strSide " + calls2 + "\n";
__L(6, "hash=" + fnv(feed));

summary("builtins_ext");
