__EXP = {};
__EXP[4] = ["c05 true true"];
__EXP[5] = ["c06 33 44"];
__EXP[6] = ["c07 33 44"];
__EXP[7] = ["c08 x y c:7"];
const OP = { A: 1, B: 2 };

const holder = { op: OP };
holder.op.A = 9;
__A("tp13_leaks.js:c01", function () { assert_eq(OP.A, 9, "c01"); });

const arr = [];
arr.push(OP);
arr[0].B = 22;
__A("tp13_leaks.js:c02", function () { assert_eq(OP.B, 22, "c02"); });

function mutate(o) { o.A = 33; }
mutate(OP);
__A("tp13_leaks.js:c03", function () { assert_eq(OP.A, 33, "c03"); });

function getRef() { return OP; }
getRef().B = 44;
__A("tp13_leaks.js:c04", function () { assert_eq(OP.B, 44, "c04"); });

__L(4, "c05", OP === holder.op, typeof OP === "object");

const sink = {};
Object.assign(sink, OP);
__L(5, "c06", sink.A, sink.B);

__L(6, "c07", OP.A, OP.B);

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
