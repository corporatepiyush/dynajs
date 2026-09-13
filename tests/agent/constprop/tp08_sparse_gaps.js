// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["c01 A B C"];
__EXP[1] = ["c02 D E F"];
__EXP[2] = ["c03 gap:1 gap:-2 gap:999"];
__EXP[3] = ["c04 gap:100001 gap:-6 gap:41"];
__EXP[5] = ["c06 seven no no"];
__EXP[6] = ["c07 x y z"];
__EXP[7] = ["c08 1 2 7 none"];
__EXP[8] = ["c09 a b c w"];
__EXP[9] = ["c10 pqr pqr pqr other"];
// tp08: sparse/gap switches and negative case values over const members.
// 10 checks.
const OP = { A: -5, B: -1, C: 0, D: 1000, E: 100000, F: 42 };

function dis(pc) {
    switch (pc) {
        case OP.A: return "A";
        case OP.B: return "B";
        case OP.C: return "C";
        case OP.D: return "D";
        case OP.E: return "E";
        case OP.F: return "F";
        default: return "gap:" + pc;
    }
}
__L(0, "c01", dis(-5), dis(-1), dis(0));
__L(1, "c02", dis(1000), dis(100000), dis(42));
__L(2, "c03", dis(1), dis(-2), dis(999));
__L(3, "c04", dis(100001), dis(-6), dis(41));

// gaps inside the dense range fall through to the chain/default
const SP = { LO: 2, HI: 10 };
function gap(v) {
    switch (v) {
        case SP.LO: return "lo";
        case SP.HI: return "hi";
        default: return "mid:" + v;
    }
}
let out = [];
for (let v = 0; v <= 12; v++) out.push(gap(v));
__A("tp08_sparse_gaps.js:c05", function () { assert_eq(out.join(","), "mid:0,mid:1,lo,mid:3,mid:4,mid:5,mid:6,mid:7,mid:8,mid:9,hi,mid:11,mid:12", "c05"); });

// single-member switch (below table threshold) still correct
const ONE = { ONLY: 7 };
function one(v) {
    switch (v) {
        case ONE.ONLY: return "seven";
        default: return "no";
    }
}
__L(5, "c06", one(7), one(8), one(6));

// two cases + default
const TWO = { X: 1, Y: 2 };
function two(v) {
    switch (v) {
        case TWO.X: return "x";
        case TWO.Y: return "y";
        default: return "z";
    }
}
__L(6, "c07", two(1), two(2), two(3));

// three cases without default (implicit fall-through-out)
function three(v) {
    let r = "none";
    switch (v) {
        case TWO.X: r = "1"; break;
        case TWO.Y: r = "2"; break;
        case ONE.ONLY: r = "7"; break;
    }
    return r;
}
__L(7, "c08", three(1), three(2), three(7), three(9));

// large range (sparse, exceeds dense window)
const WIDE = { A: 0, B: 5000, C: -5000 };
function wide(v) {
    switch (v) {
        case WIDE.A: return "a";
        case WIDE.B: return "b";
        case WIDE.C: return "c";
        default: return "w";
    }
}
__L(8, "c09", wide(0), wide(5000), wide(-5000), wide(1));

// group cases sharing a body
const GRP = { P: 3, Q: 4, R: 5 };
function grp(v) {
    switch (v) {
        case GRP.P:
        case GRP.Q:
        case GRP.R:
            return "pqr";
        default:
            return "other";
    }
}
__L(9, "c10", grp(3), grp(4), grp(5), grp(6));

summary("constprop");
