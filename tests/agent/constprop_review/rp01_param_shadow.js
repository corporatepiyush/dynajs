__EXP = null;
const OP = { HALT: 1 };
function f(OP) { return OP.HALT; }
__A("rp01_param_shadow.js:f99", function () { assert_eq(f({ HALT: 99 }), 99, "f99"); });
__A("rp01_param_shadow.js:f7", function () { assert_eq(f({ HALT: 7 }), 7, "f7"); });
__A("rp01_param_shadow.js:top", function () { assert_eq(OP.HALT, 1, "top"); });

summary("constprop_review");
