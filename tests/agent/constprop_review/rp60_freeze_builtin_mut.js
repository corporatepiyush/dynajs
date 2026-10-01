__EXP = {};
__EXP[0] = ["direct 1 closure 1"];
Object.freeze = function (o) { return o; };
const OP = Object.freeze({ HALT: 1 });
function get() { return OP.HALT; }
__L(0, "direct", OP.HALT, "closure", get());

summary("constprop_review");
