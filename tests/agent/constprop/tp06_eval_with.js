__EXP = {};
__EXP[3] = ["c04 withObj 42"];
__EXP[4] = ["c05 mutated 42"];
__EXP[5] = ["c06 a b d0"];
__EXP[8] = ["c08 141 141"];
const OP = { A: 1, B: 2 };

eval("OP.A = 42");
__A("tp06_eval_with.js:c01", function () { assert_eq(OP.A, 42, "c01"); });

eval("OP.C = 3");
__A("tp06_eval_with.js:c02", function () { assert_eq(OP.C, 3, "c02"); });

function sneaky() { eval("OP.B = 99"); }
sneaky();
__A("tp06_eval_with.js:c03", function () { assert_eq(OP.B, 99, "c03"); });

const probe = { A: "withObj" };
let withResult;
with (probe) {
    withResult = A;
}
__L(3, "c04", withResult, OP.A);

with (probe) {
    A = "mutated";
}
__L(4, "c05", probe.A, OP.A);

function dispatch(pc) {
    switch (pc) {
        case OP.A: return "a";
        case OP.B: return "b";
        default: return "d" + pc;
    }
}
__L(5, "c06", dispatch(42), dispatch(99), dispatch(0));

const ind = eval;
try { ind("1+1"); __A("tp06_eval_with.js:c07", function () { assert_eq("indirect-ok", "indirect-ok", "c07"); }); }
catch (e) { __L(7, "c07", "indirect-throws"); }

__L(8, "c08", eval("OP.A + OP.B"), OP.A + OP.B);

summary("constprop");
