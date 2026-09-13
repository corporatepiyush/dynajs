// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["add-fast add-slow sub d"];
const OP = { ADD: 0, SUB: 1 };
const MODE = { FAST: 5, SLOW: 6 };
function f(op, m) {
  switch (op) { case OP.ADD: return m === MODE.FAST ? "add-fast" : "add-slow";
                case OP.SUB: return "sub"; default: return "d"; }
}
__L(0, f(0,5), f(0,6), f(1,5), f(9,9));

summary("constprop_review");
