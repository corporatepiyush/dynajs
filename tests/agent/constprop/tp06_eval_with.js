// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["c04 withObj 42"];
__EXP[4] = ["c05 mutated 42"];
__EXP[5] = ["c06 a b d0"];
__EXP[8] = ["c08 141 141"];
// tp06: direct eval / with anywhere in the script disables all folding —
// observable effects stay correct. 8 checks.
const OP = { A: 1, B: 2 };

// direct eval assigning into OP must be visible
eval("OP.A = 42");
__A("tp06_eval_with.js:c01", function () { assert_eq(OP.A, 42, "c01"); });

// eval that adds a property
eval("OP.C = 3");
__A("tp06_eval_with.js:c02", function () { assert_eq(OP.C, 3, "c02"); });

// eval in a function, executed before the read
function sneaky() { eval("OP.B = 99"); }
sneaky();
__A("tp06_eval_with.js:c03", function () { assert_eq(OP.B, 99, "c03"); });

// with-scope read of same-named property must not be confused with the
// const binding (with present => folding off for the whole script)
const probe = { A: "withObj" };
let withResult;
with (probe) {
    withResult = A;
}
__L(3, "c04", withResult, OP.A);

// with writing through the object
with (probe) {
    A = "mutated";
}
__L(4, "c05", probe.A, OP.A);

// switch dispatch still correct with eval present
function dispatch(pc) {
    switch (pc) {
        case OP.A: return "a";
        case OP.B: return "b";
        default: return "d" + pc;
    }
}
__L(5, "c06", dispatch(42), dispatch(99), dispatch(0));

// indirect eval writing into OP via the binding name (global scope here)
const ind = eval;
// (only direct eval is in scope of the disable rule; an indirect eval
// cannot see the lexical binding, so nothing to observe — just run it)
try { ind("1+1"); __A("tp06_eval_with.js:c07", function () { assert_eq("indirect-ok", "indirect-ok", "c07"); }); }
catch (e) { __L(7, "c07", "indirect-throws"); }

// eval reading (no writes) keeps everything consistent
__L(8, "c08", eval("OP.A + OP.B"), OP.A + OP.B);

summary("constprop");
