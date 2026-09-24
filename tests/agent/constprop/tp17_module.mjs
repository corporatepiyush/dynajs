// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["m01 1 20 a b c d"];
__EXP[1] = ["m02 function 3"];
__EXP[2] = ["m03 7 true"];
// tp17: module-file variant (-m / .mjs). Module top-level const objects are
// NOT folded (documented lane decision: script-level only) — behavior must
// be identical either way: reads, writes, dispatch. 6 checks.
const OP = { A: 1, B: 2, C: 3 };

function dis(v) {
    switch (v) {
        case OP.A: return "a";
        case OP.B: return "b";
        case OP.C: return "c";
        default: return "d";
    }
}

OP.B = 20; // module bindings are mutable
__L(0, "m01", OP.A, OP.B, dis(1), dis(20), dis(3), dis(9));
__L(1, "m02", typeof dis, Object.keys(OP).length);

const FR = Object.freeze({ X: 7 });
__L(2, "m03", FR.X, Object.isFrozen(FR));

// TDZ inside module
try { __L(3, "m04", EARLYM); } catch (e) { __A("tp17_module.mjs:m04", function () { assert_eq(e instanceof ReferenceError, true, "m04"); }); }
const EARLYM = 5;
__A("tp17_module.mjs:m05", function () { assert_eq(EARLYM, 5, "m05"); });

export const EXPORTED = OP.A; // prove it really is a module
__A("tp17_module.mjs:m06", function () { assert_eq(EXPORTED, 1, "m06"); });

summary("constprop");
