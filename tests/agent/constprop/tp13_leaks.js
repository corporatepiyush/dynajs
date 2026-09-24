// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[4] = ["c05 true true"];
__EXP[5] = ["c06 33 44"];
__EXP[6] = ["c07 33 44"];
__EXP[7] = ["c08 x y c:7"];
// tp13: leaks — aliasing the object into another container disables folding
// and mutations through the alias stay visible. 8 checks.
const OP = { A: 1, B: 2 };

// alias through a holder object
const holder = { op: OP };
holder.op.A = 9;
__A("tp13_leaks.js:c01", function () { assert_eq(OP.A, 9, "c01"); });

// alias through an array
const arr = [];
arr.push(OP);
arr[0].B = 22;
__A("tp13_leaks.js:c02", function () { assert_eq(OP.B, 22, "c02"); });

// alias through a function argument
function mutate(o) { o.A = 33; }
mutate(OP);
__A("tp13_leaks.js:c03", function () { assert_eq(OP.A, 33, "c03"); });

// alias returned from a function used to write
function getRef() { return OP; }
getRef().B = 44;
__A("tp13_leaks.js:c04", function () { assert_eq(OP.B, 44, "c04"); });

// bare reference in a comparison keeps semantics (no mutation here, but the
// object is observed: the fold may or may not apply; behavior identical)
__L(4, "c05", OP === holder.op, typeof OP === "object");

// Object.assign target
const sink = {};
Object.assign(sink, OP);
__L(5, "c06", sink.A, sink.B);

// reads of members after all the above reflect the final state
__L(6, "c07", OP.A, OP.B);

// a fresh binding without leaks keeps folding semantics (values identical)
const CLEAN = { X: 5, Y: 6 };
function dis(v) {
    switch (v) {
        case CLEAN.X: return "x";
        case CLEAN.Y: return "y";
        default: return "c:" + v;
    }
}
__L(7, "c08", dis(5), dis(6), dis(7));

summary("constprop");
