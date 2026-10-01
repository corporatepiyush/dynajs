__EXP = {};
__EXP[0] = ["m01 1 20 a b c d"];
__EXP[1] = ["m02 function 3"];
__EXP[2] = ["m03 7 true"];
const OP = { A: 1, B: 2, C: 3 };

function dis(v) {
    switch (v) {
        case OP.A: return "a";
        case OP.B: return "b";
        case OP.C: return "c";
        default: return "d";
    }
}

OP.B = 20;
__L(0, "m01", OP.A, OP.B, dis(1), dis(20), dis(3), dis(9));
__L(1, "m02", typeof dis, Object.keys(OP).length);

const FR = Object.freeze({ X: 7 });
__L(2, "m03", FR.X, Object.isFrozen(FR));

try { __L(3, "m04", EARLYM); } catch (e) { __A("tp17_module.mjs:m04", function () { assert_eq(e instanceof ReferenceError, true, "m04"); }); }
const EARLYM = 5;
__A("tp17_module.mjs:m05", function () { assert_eq(EARLYM, 5, "m05"); });

export const EXPORTED = OP.A;
__A("tp17_module.mjs:m06", function () { assert_eq(EXPORTED, 1, "m06"); });

summary("constprop");
