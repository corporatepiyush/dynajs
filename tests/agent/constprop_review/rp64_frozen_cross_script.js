__EXP = null;
const OP = Object.freeze({ HALT: 1 });
globalThis.getOP = function () { return OP.HALT; };
globalThis.readOP = function () { __L(0, "read", OP.HALT); };

summary("constprop_review");
