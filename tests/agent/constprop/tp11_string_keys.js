// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["c01 1 1"];
__EXP[3] = ["c04 4 4"];
__EXP[4] = ["c05 5 5"];
__EXP[6] = ["c07 zero double one zero"];
__EXP[7] = ["c08 alpha bg zero4 d:0"];
__EXP[8] = ["c09 1 2"];
__EXP[9] = ["c10 1 2 3 4"];
// tp11: string-keyed members and bracket reads. 10 checks.
const OP = {
    "alpha": 1,
    "beta gamma": 2,
    "": 3,
    "0": 4,
    "1.5": 5,
    "if": 6
};

__L(0, "c01", OP.alpha, OP["alpha"]);
__A("tp11_string_keys.js:c02", function () { assert_eq(OP["beta gamma"], 2, "c02"); });
__A("tp11_string_keys.js:c03", function () { assert_eq(OP[""], 3, "c03"); });
__L(3, "c04", OP["0"], OP[0]);
__L(4, "c05", OP["1.5"], OP[1.5]);
__A("tp11_string_keys.js:c06", function () { assert_eq(OP.if, 6, "c06"); });

// distinctness: "0" vs 0 vs "00"
const K = { "0": "zero", "00": "double", 1: "one" };
__L(6, "c07", K[0], K["00"], K[1], K["0"]);

// string-keyed switch dispatch
function dis(v) {
    switch (v) {
        case OP.alpha: return "alpha";
        case OP["beta gamma"]: return "bg";
        case OP["0"]: return "zero4";
        default: return "d:" + v;
    }
}
__L(7, "c08", dis(1), dis(2), dis(4), dis(0));

// prototype-chain names must NOT be own members
const P = { hasOwnProperty: 1, constructor: 2 };
__L(8, "c09", P.hasOwnProperty, typeof P.constructor === "function" ? "ctorfn" : P.constructor);

// symbol-ish / reserved words as keys
const R = { "class": 1, "function": 2, "null": 3, "true": 4 };
__L(9, "c10", R.class, R.function, R.null, R.true);

summary("constprop");
