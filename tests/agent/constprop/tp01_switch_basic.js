// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// tp01: basic switch dispatch over a non-frozen const object folds and
// behaves identically to a literal-label switch. 10 checks.
const OP = { ADD: 0, SUB: 1, MUL: 2, DIV: 3, NEG: 4, ABS: 5 };

function apply(op, x, y) {
    switch (op) {
        case OP.ADD: return x + y;
        case OP.SUB: return x - y;
        case OP.MUL: return x * y;
        case OP.DIV: return y === 0 ? 0 : (x / y) | 0;
        case OP.NEG: return -x;
        case OP.ABS: return x < 0 ? -x : x;
        default: return -999;
    }
}

__A("tp01_switch_basic.js:c01", function () { assert_eq(apply(OP.ADD, 3, 4), 7, "c01"); });
__A("tp01_switch_basic.js:c02", function () { assert_eq(apply(OP.SUB, 10, 4), 6, "c02"); });
__A("tp01_switch_basic.js:c03", function () { assert_eq(apply(OP.MUL, 6, 7), 42, "c03"); });
__A("tp01_switch_basic.js:c04", function () { assert_eq(apply(OP.DIV, 9, 2), 4, "c04"); });
__A("tp01_switch_basic.js:c05", function () { assert_eq(apply(OP.DIV, 1, 0), 0, "c05"); });
__A("tp01_switch_basic.js:c06", function () { assert_eq(apply(OP.NEG, 5, 0), -5, "c06"); });
__A("tp01_switch_basic.js:c07", function () { assert_eq(apply(OP.ABS, -8, 0), 8, "c07"); });
__A("tp01_switch_basic.js:c08", function () { assert_eq(apply(OP.ABS, 8, 0), 8, "c08"); });
__A("tp01_switch_basic.js:c09", function () { assert_eq(apply(99, 1, 1), -999, "c09"); });
__A("tp01_switch_basic.js:c10", function () { assert_eq(apply(-1, 1, 1), -999, "c10"); });

summary("constprop");
