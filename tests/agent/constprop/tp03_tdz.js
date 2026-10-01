__EXP = null;
function early() { return OP.A; }
try {
    const r = early();
    __L(0, "c01 no-throw", r);
} catch (e) {
    __A("tp03_tdz.js:c01", function () { assert_eq(e instanceof ReferenceError, true, "c01"); });
}
const OP = { A: 1 };
__A("tp03_tdz.js:c02", function () { assert_eq(early(), 1, "c02"); });

try {
    const v = OP2.B;
    __L(3, "c03 no-throw", v);
} catch (e) {
    __A("tp03_tdz.js:c03", function () { assert_eq(e instanceof ReferenceError, true, "c03"); });
}
const OP2 = { B: 7 };
__A("tp03_tdz.js:c04", function () { assert_eq(OP2.B, 7, "c04"); });

callIt();
const OP3 = { C: 3 };
function callIt() { try { return OP3.C; } catch (e) { return e instanceof ReferenceError; } }
__A("tp03_tdz.js:c05", function () { assert_eq(callIt(), 3, "c05"); });

const OP4 = { D: 4 };
function late() { return OP4.D; }
__A("tp03_tdz.js:c06", function () { assert_eq(late(), 4, "c06"); });

summary("constprop");
