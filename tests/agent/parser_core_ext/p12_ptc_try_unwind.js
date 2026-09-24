// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a done f0,f1,f2,f3"];
__EXP[1] = ["b handled c0:bottom,c1,c2,c3"];
__EXP[2] = ["c 300 m0,m100,m200,m300"];
// P12: call in tail position THROUGH try/finally is not a tail call — the
// finally must run per frame on unwind, ordered innermost-first (bounded).
var log = [];
function rec(n) {
  try {
    if (n === 0) return "done";
    return rec(n - 1);
  } finally { log.push("f" + n); }
}
__L(0, "a", rec(3), log.join(","));
log.length = 0;
// try/catch variant — a throw from deep unwinds through every frame's catch
function rec2(n) {
  try {
    if (n === 0) throw new Error("bottom");
    return rec2(n - 1);
  } catch (e) {
    log.push("c" + n + (n === 0 ? ":" + e.message : ""));
    if (n === 3) return "handled";
    throw e;
  }
}
__L(1, "b", rec2(3), log.join(","));
log.length = 0;
// recursion depth 300 with finally at each level — both engines must agree
function rec3(n) {
  try {
    if (n === 0) return 0;
    return rec3(n - 1) + 1;
  } finally { if (n % 100 === 0) log.push("m" + n); }
}
__L(2, "c", rec3(300), log.join(","));

summary("parser_core_ext");
