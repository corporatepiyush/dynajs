// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a A B C d reads=2"];
__EXP[2] = ["b E:getter-boom reads=1"];
__EXP[3] = ["c hitA/reads=0 hitC/reads=1 dd/reads=2"];
__EXP[4] = ["d va vlit vd"];
// S4: varprop fused probe (`case OP.X:`) against an enum object carrying an
// ACCESSOR — getter call order and throw propagation must match the unfused
// semantics: cases checked in source order, getter runs once per case check.
var reads = 0, throwAt = -1;
var OP = {};
Object.defineProperties(OP, {
  A: { value: 1, enumerable: true },
  B: { value: 2, enumerable: true },
  C: { get() { reads++; if (reads === throwAt) throw new Error("getter-boom"); return 3; }, enumerable: true },
});
function dispatch(code) {
  switch (code) {
    case OP.A: return "A";
    case OP.B: return "B";
    case OP.C: return "C";
    default: return "d";
  }
}
__L(0, "a", dispatch(1), dispatch(2), dispatch(3), dispatch(9), "reads=" + reads);
throwAt = 1; reads = 0;
try { __L(1, "b", dispatch(7)); } catch (e) { __L(2, "b", "E:" + e.message, "reads=" + reads); }
throwAt = -1; reads = 0;
// getter on the matched case: value read exactly once per dispatch
function dispatch2(code) {
  var log = [];
  switch (code) {
    case OP.A: log.push("hitA"); break;
    case OP.C: log.push("hitC"); break;
    default: log.push("dd");
  }
  return log.join("|") + "/reads=" + reads;
}
__L(3, "c", dispatch2(1), dispatch2(3), dispatch2(0));
// mixed value + varprop + computed labels in one switch
var key = "K";
function dispatch3(code) {
  switch (code) {
    case OP.A: return "va";
    case OP[key] === undefined ? "never" : OP[key]: return "vk";
    case "lit": return "vlit";
    default: return "vd";
  }
}
__L(4, "d", dispatch3(OP.A), dispatch3("lit"), dispatch3("zz"));

summary("parser_core_ext");
