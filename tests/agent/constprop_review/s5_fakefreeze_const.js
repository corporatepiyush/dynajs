__EXP = null;
Object.freeze = function (o) { return o; };
const OP = Object.freeze({ HALT: 1 });
globalThis.getOP = function () { return OP.HALT; };
globalThis.readOP = function () { __L(0, "read", OP.HALT); };

summary("constprop_review");
