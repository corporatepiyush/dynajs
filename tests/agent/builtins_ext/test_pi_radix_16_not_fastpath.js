// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["FAIL 255/16=591", "FAIL 8/8=NaN", "FAIL 35/36=125", "FAIL 36/36=132"];
__EXP[1] = ["fails=4 hash=dea74f7e"];
// test_pi_radix_16_not_fastpath.js — radix must NOT be ignored by the identity
// fast path: parseInt(12,16) === 18, parseInt(0x1f,16) === 31, plus a full
// radix battery 2/8/10/16/36 with numeric and string inputs vs node.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var fails = 0;
function chk(c, l) { if (!c) { fails++; __L(0, "FAIL " + l); } }
chk(parseInt(12, 16) === 18, "12/16=18 got " + parseInt(12, 16));
chk(parseInt(-15, 16) === -21, "-15/16=-21");
chk(parseInt(255, 16) === 591, "255/16=591");
chk(parseInt(31, 16) === 49, "31/16=49");
chk(parseInt(0x1f, 16) === 49, "0x1f/16=49 (ToString first: '31')");
chk(parseInt("0x1f", 16) === 31, "'0x1f'/16=31");
chk(parseInt(10, 2) === 2, "10/2=2");
chk(parseInt(2, 8) === 2, "2/8=2");
chk(parseInt(7, 8) === 7, "7/8=7");
chk(parseInt(8, 8) === NaN, "8/8=NaN");
chk(parseInt(101, 2) === 5, "101/2=5");
chk(parseInt(-101, 2) === -5, "-101/2=-5");
chk(parseInt(5, 36) === 5, "5/36=5");
chk(parseInt(35, 36) === 125, "35/36=125");
chk(parseInt(36, 36) === 132, "36/36=132");
// radix 0/undefined/10 behave identically (the fast-pathable set)
var inputs = [0, -0, 42, -42, 123456789, -123456789, 9007199254740991, 1e21, 12.9, -0.4];
for (var i = 0; i < inputs.length; i++) {
  var v = inputs[i];
  var a = parseInt(v), b = parseInt(v, 0), c = parseInt(v, 10), d = parseInt(v, undefined);
  chk(a === b && b === c && c === d, "radix010 " + v + " " + a + "/" + b + "/" + c + "/" + d);
  feed += v + ":" + a + "\n";
}
__L(1, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
