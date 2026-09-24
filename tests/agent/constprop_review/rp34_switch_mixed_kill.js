// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a b d"];
__EXP[1] = ["d a b d d"];
const OP = { A: 1, B: 2 };
function f(v) { switch (v) { case OP.A: return "a"; case OP.B: return "b"; default: return "d"; } }
__L(0, f(1), f(2), f(3));
OP.A = 10;            // kill A; switch table must be disabled (case A used it)
__L(1, f(1), f(10), f(2), f(20), f(3));

summary("constprop_review");
