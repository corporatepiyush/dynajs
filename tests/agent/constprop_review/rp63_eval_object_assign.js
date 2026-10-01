__EXP = {};
__EXP[0] = ["1 1"];
const OP = Object.freeze({ HALT: 1 });
eval("Object.freeze = function(o){ return {}; };");
__L(0, OP.HALT, (function(){ return OP.HALT; })());

summary("constprop_review");
