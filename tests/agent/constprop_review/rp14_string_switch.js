__EXP = {};
__EXP[0] = ["walked ran ?"];
const OP = { MODE: "walk" };
function m(v) { switch (v) { case OP.MODE: return "walked"; case "run": return "ran"; default: return "?"; } }
__L(0, m("walk"), m("run"), m("x"));

summary("constprop_review");
