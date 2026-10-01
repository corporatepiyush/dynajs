__EXP = {};
__EXP[0] = ["c01 base base+1 base+step base+step"];
__EXP[1] = ["c02 literal d:1"];
__EXP[2] = ["c03 k k+1 d:2"];
__EXP[3] = ["c04 k k+1 d:3"];
__EXP[4] = ["c05 ax by ab:3"];
__EXP[5] = ["c06 tern tern tern-d"];
__EXP[6] = ["c07 z nz"];
__EXP[7] = ["c08 nine 1"];
__EXP[8] = ["c09 name str-d str-d"];
__EXP[9] = ["c10 true-case bool-d bool-d"];
const OP = { BASE: 10, STEP: 5 };

function dis(v) {
    switch (v) {
        case OP.BASE: return "base";
        case OP.BASE + 1: return "base+1";
        case OP.BASE + OP.STEP: return "base+step";
        case OP.STEP * 2: return "step*2";
        case 77: return "literal";
        default: return "d:" + v;
    }
}
__L(0, "c01", dis(10), dis(11), dis(15), dis(10 + 5 === 15 ? 15 : 0));
__L(1, "c02", dis(77), dis(1));

const M = { K: 3 };
function mdis(v) {
    switch (v) {
        case M.K: return "k";
        case M.K + 1: return "k+1";
        default: return "d:" + v;
    }
}
__L(2, "c03", mdis(3), mdis(4), mdis(2));

M.K = 30;
__L(3, "c04", mdis(30), mdis(31), mdis(3));

const A = { X: 1 };
const B = { Y: 2 };
function ab(v) {
    switch (v) {
        case A.X: return "ax";
        case B.Y: return "by";
        default: return "ab:" + v;
    }
}
__L(4, "c05", ab(1), ab(2), ab(3));

function tern(v, flag) {
    switch (v) {
        case flag ? A.X : B.Y: return "tern";
        default: return "tern-d";
    }
}
__L(5, "c06", tern(1, true), tern(2, false), tern(1, false));

const NEG = { Z: -7 };
function negdis(v) {
    switch (v) {
        case NEG.Z: return "z";
        default: return "nz";
    }
}
__L(6, "c07", negdis(-7), negdis(7));

let counter = 0;
function sideLabel(v) {
    switch (v) {
        case ++counter && 9: return "nine";
        default: return "not9";
    }
}
__L(7, "c08", sideLabel(9), counter);

const S = { NAME: "hallo" };
function sdis(v) {
    switch (v) {
        case S.NAME: return "name";
        default: return "str-d";
    }
}
__L(8, "c09", sdis("hallo"), sdis("hallo "), sdis("HALLO"));

const BF = { FLAG: true };
function bdis(v) {
    switch (v) {
        case BF.FLAG: return "true-case";
        default: return "bool-d";
    }
}
__L(9, "c10", bdis(true), bdis(1), bdis(false));

summary("constprop");
