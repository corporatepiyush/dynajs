__EXP = {};
__EXP[0] = ["c01 100 1"];
__EXP[2] = ["c03 300 1"];
__EXP[8] = ["c08 param 1"];
const OP = { A: 1, B: 2 };

function f1() {
    let OP = { A: 100 };
    return OP.A;
}
__L(0, "c01", f1(), OP.A);

function f2() {
    function inner() { return typeof OP; }
    var OP = { A: 200 };
    return inner() + ":" + OP.A;
}
__A("tp04_shadowing.js:c02", function () { assert_eq(f2(), "object:200", "c02"); });

function f3() {
    const OP = { A: 300 };
    return OP.A;
}
__L(2, "c03", f3(), OP.A);

function f4() {
    let r;
    try {
        r = OP.A;
        let OP = { A: 400 };
    } catch (e) {
        r = e instanceof ReferenceError;
    }
    return r;
}
__A("tp04_shadowing.js:c04", function () { assert_eq(f4(), true, "c04"); });

function f5() {
    const before = OP.A;
    { let OP = { A: 500 }; }
    return before;
}
__A("tp04_shadowing.js:c05", function () { assert_eq(f5(), 1, "c05"); });

function f6() {
    return OP.A;
    function OP() { return null; }
}
try {
    __A("tp04_shadowing.js:c06", function () { assert_eq(typeof f6(), "undefined", "c06"); });
} catch (e) {
    __L(6, "c06", "throws", e instanceof TypeError);
}

function f7() {
    try { null.x; } catch (OP) { return OP instanceof TypeError; }
}
__A("tp04_shadowing.js:c07", function () { assert_eq(f7(), true, "c07"); });

function f8(OP) { return OP; }
__L(8, "c08", f8("param"), OP.A);

function f9() {
    class OP { }
    return typeof OP;
}
__A("tp04_shadowing.js:c09", function () { assert_eq(f9(), "function", "c09"); });

function f10() {
    const inner = () => { let OP = { A: 900 }; return OP.A; };
    return inner() + OP.A;
}
__A("tp04_shadowing.js:c10", function () { assert_eq(f10(), 901, "c10"); });

summary("constprop");
