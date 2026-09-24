// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["c04 1 true"];
__EXP[5] = ["c06 d d"];
__EXP[7] = ["c08 1"];
// tp18: `arguments` and other phase-2-lazy names — a top-level const named
// `arguments` never folds (the implicit arguments object shadows it inside
// every non-arrow function). 8 checks.
const arguments = Object.freeze({ A: 1 });

function f(x) { return arguments.A; }
__A("tp18_arguments_lazy.js:c01", function () { assert_eq(f(2), undefined, "c01"); }); // arguments object of f: no A -> undefined

function g() { return typeof arguments; }
__A("tp18_arguments_lazy.js:c02", function () { assert_eq(g(), "object", "c02"); }); // "object"

const argsLen = (function () { return arguments.length; })(1, 2, 3);
__A("tp18_arguments_lazy.js:c03", function () { assert_eq(argsLen, 3, "c03"); });

// top-level read still sees the const binding itself
__L(3, "c04", arguments.A, Object.isFrozen(arguments));

// arrow functions have no own arguments object: the outer (top-level)
// lexical `arguments` is the const -> 1
const h = () => arguments.A;
__A("tp18_arguments_lazy.js:c05", function () { assert_eq(h(), 1, "c05"); });

// switch dispatch inside a function over `arguments.A` stays dynamic
function dis(x) {
    switch (x) {
        case arguments.A: return "A";
        default: return "d";
    }
}
__L(5, "c06", dis(1), dis(9));

// a param named arguments shadows too
function p(arguments) { return arguments.A; }
__A("tp18_arguments_lazy.js:c07", function () { assert_eq(p({ A: 7 }), 7, "c07"); });

// mutation of the frozen top-level binding is a no-op in sloppy mode
arguments.A = 5;
__L(7, "c08", arguments.A);

// other phase-2 pseudo names cannot collide (syntax-checked): this / new.target
const okNames = true;
__A("tp18_arguments_lazy.js:c09", function () { assert_eq(okNames, true, "c09"); });

summary("constprop");
