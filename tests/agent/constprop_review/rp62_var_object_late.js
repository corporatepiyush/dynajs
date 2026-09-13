// converted from an escaping-throw probe: the original source is
// re-executed (output silenced) and its per-engine behavior —
// throw + emitted lines — asserted against baked expectations
__EXP = null;
test("rp62_var_object_late.js per-engine behavior contract", function () {
    var __SRC = "// (c) var Object hoisted AFTER the const decl in the same script\nconst OP = Object.freeze({ HALT: 1 });\nvar Object = 42;\nconsole.log(OP.HALT, Object, (function(){ return OP.HALT; })());\n";
    var __lines = __NP(); __lines.n = 0; var __e = null;
    var __sl = console.log; var __sp = (typeof print === 'function') ? print : null;
    console.log = function () { var a = __NP(); var i; for (i = 0; i < arguments.length; i++) a[i] = arguments[i]; a.n = arguments.length; __lines[__lines.n] = __fmtLine(a); __lines.n++; };
    if (__sp) { try { print = function (s) { console.log(String(s)); }; } catch (__e9) {} }
    try { eval(__SRC); } catch (err) { __e = err; }
    console.log = __sl;
    if (__sp) { try { print = __sp; } catch (__e8) {} }
    if (__ENGINE === 'dynajs') {
        assert_eq(__e !== null, true, "dynajs threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "TypeError", "dynajs error ctor");
        assert_eq(__e && __e.message, "cannot read property 'freeze' of undefined", "dynajs error message");
        assert_eq(__lines.n, 0, "dynajs line count");
    }
    if (__ENGINE !== 'dynajs') {
        assert_eq(__e !== null, true, "node threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "TypeError", "node error ctor");
        assert_eq(__e && __e.message, "Cannot read properties of undefined (reading 'freeze')", "node error message");
        assert_eq(__lines.n, 0, "node line count");
    }
});
summary("constprop_review");
