__EXP = {};
__EXP[0] = ["1 1"];
__EXP[1] = ["9 1"];
const OP = { HALT: 1 };
const obj = { HALT: 9 };
function f() { "use strict"; return OP.HALT; }
__L(0, f(), OP.HALT);
let r;
try { with (obj) { r = HALT; } } catch (e) { r = "threw:" + e.constructor.name; }
__L(1, r, OP.HALT);

summary("constprop_review");
