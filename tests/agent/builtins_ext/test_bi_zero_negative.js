// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["fails=0"];
// test_bi_zero_negative.js — no BigInt negative zero exists: String(-0n) === "0";
// D&C toString on zero across radices; asIntN/asUintN of zero; signed zero
// arithmetic. Byte-identical vs node.
var fails = 0;
function chk(c, l) { if (!c) { fails++; __L(0, "FAIL " + l); } }
chk(String(0n) === "0", "String(0n)");
chk(String(-0n) === "0", "String(-0n)");
chk(String(0n * -1n) === "0", "0n*-1n");
chk(-0n === 0n, "-0n===0n");
chk(Object.is(-0n, 0n), "Object.is(-0n,0n) true (same value)");
chk(BigInt.asIntN(64, 0n) === 0n, "asIntN(64,0n)");
chk(BigInt.asUintN(64, 0n) === 0n, "asUintN(64,0n)");
chk(BigInt.asUintN(0, 12345n) === 0n, "asUintN(0,x)=0n");
chk(BigInt.asIntN(1, -1n) === -1n, "asIntN(1,-1n)");
chk(BigInt.asUintN(1, -1n) === 1n, "asUintN(1,-1n)");
chk(BigInt.asUintN(64, 2n ** 64n) === 0n, "asUintN(64,2^64)=0");
chk(BigInt.asUintN(64, 2n ** 64n - 1n) === 2n ** 64n - 1n, "asUintN(64,max)");
chk(BigInt.asIntN(64, 2n ** 63n) === -(2n ** 63n), "asIntN(64,2^63) wraps");
chk(BigInt.asIntN(64, 2n ** 63n - 1n) === 2n ** 63n - 1n, "asIntN(64,max)");
chk(BigInt.asUintN(64, -1n) === 2n ** 64n - 1n, "asUintN(64,-1)");
// zero across radices
for (var rd = 2; rd <= 36; rd++) {
  if (0n.toString(rd) !== "0") { fails++; __L(1, "FAIL zero radix " + rd); }
}
// negative toString sign handling
chk(String(-(2n ** 100n)) === "-" + String(2n ** 100n), "neg-sign");
chk((-7n).toString(7) === "-10", "-7/7");
chk((0n).toString(7) === "0", "0/7");
__L(2, "fails=" + fails);

summary("builtins_ext");
