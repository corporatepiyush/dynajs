// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["c01 F.A F.B F.C F.dflt"];
__EXP[1] = ["c02 N.A N.B N.C N.dflt"];
__EXP[2] = ["c03 no-throw 10"];
__EXP[4] = ["c04 true false"];
__EXP[5] = ["c05 5 true"];
__EXP[7] = ["c07 1 2 4 4"];
__EXP[8] = ["c08 true true true true"];
__EXP[9] = ["c09 6 60"];
__EXP[10] = ["c10 10 20 false"];
// tp07: non-frozen and Object.freeze literals both fold; frozen objects
// reject writes. 10 checks.
const N = { A: 1, B: 2, C: 3 };
const F = Object.freeze({ A: 10, B: 20, C: 30 });

function pick(o, k) {
    switch (k) {
        case o.A: return "N.A";
        default: break;
    }
    return "other";
}

// frozen dispatch correctness
function disF(pc) {
    switch (pc) {
        case F.A: return "F.A";
        case F.B: return "F.B";
        case F.C: return "F.C";
        default: return "F.dflt";
    }
}
function disN(pc) {
    switch (pc) {
        case N.A: return "N.A";
        case N.B: return "N.B";
        case N.C: return "N.C";
        default: return "N.dflt";
    }
}
__L(0, "c01", disF(10), disF(20), disF(30), disF(40));
__L(1, "c02", disN(1), disN(2), disN(3), disN(4));

// frozen: strict-mode write throws; sloppy silently ignores
try {
    F.A = 999;
    __L(2, "c03", "no-throw", F.A);
} catch (e) {
    __L(3, "c03", "throws", F.A);
}

// Object.isFrozen both
__L(4, "c04", Object.isFrozen(F), Object.isFrozen(N));

// frozen via Object.freeze on a literal stored later — still plain object
const G = Object.freeze({ X: 5 });
__L(5, "c05", G.X, Object.isFrozen(G));

// members with duplicate keys: last wins
const D = { K: 1, K: 2 };
__A("tp07_frozen_both.js:c06", function () { assert_eq(D.K, 2, "c06"); });

// mixed string and numeric member names
const M = { a: 1, "b": 2, 3: 4 };
__L(7, "c07", M.a, M.b, M["3"], M[3]);

// null/bool members read back correctly
const BN = { t: true, f: false, n: null, u: undefined };
__L(8, "c08", BN.t === true, BN.f === false, BN.n === null, BN.u === undefined);

// numeric member used in arithmetic folds to the same value
__L(9, "c09", N.A + N.B + N.C, F.A + F.B + F.C);

// frozen object spread copy is un frozen and reads the same
const copy = { ...F };
__L(10, "c10", copy.A, copy.B, Object.isFrozen(copy));

summary("constprop");
