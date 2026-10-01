__EXP = {};
__EXP[3] = ["c04 1 true"];
__EXP[5] = ["c06 d d"];
__EXP[7] = ["c08 1"];
const arguments = Object.freeze({ A: 1 });

function f(x) { return arguments.A; }
__A("tp18_arguments_lazy.js:c01", function () { assert_eq(f(2), undefined, "c01"); });

function g() { return typeof arguments; }
__A("tp18_arguments_lazy.js:c02", function () { assert_eq(g(), "object", "c02"); });

const argsLen = (function () { return arguments.length; })(1, 2, 3);
__A("tp18_arguments_lazy.js:c03", function () { assert_eq(argsLen, 3, "c03"); });

__L(3, "c04", arguments.A, Object.isFrozen(arguments));

const h = () => arguments.A;
__A("tp18_arguments_lazy.js:c05", function () { assert_eq(h(), 1, "c05"); });

function dis(x) {
    switch (x) {
        case arguments.A: return "A";
        default: return "d";
    }
}
__L(5, "c06", dis(1), dis(9));

function p(arguments) { return arguments.A; }
__A("tp18_arguments_lazy.js:c07", function () { assert_eq(p({ A: 7 }), 7, "c07"); });

arguments.A = 5;
__L(7, "c08", arguments.A);

const okNames = true;
__A("tp18_arguments_lazy.js:c09", function () { assert_eq(okNames, true, "c09"); });

summary("constprop");
