// converted from an escaping-throw probe: the original source is
// re-executed (output silenced) and its per-engine behavior —
// throw + emitted lines — asserted against baked expectations
__EXP = null;
test("rp03_cross_script_mutation.js per-engine behavior contract", function () {
    var __SRC = "// rp03: a SECOND globally-compiled script (Function ctor) mutates the member\n// after the first script's reads were folded. node: 2. folded engine: 1?\nconst OP = { HALT: 1 };\nfunction get() { return OP.HALT; }\nconsole.log(\"before\", get());\n(0, eval)(\"OP.HALT = 2;\");   // indirect eval = fresh global script\nconsole.log(\"after\", get());\n";
    var __lines = __NP(); __lines.n = 0; var __e = null;
    var __sl = console.log; var __sp = (typeof print === 'function') ? print : null;
    console.log = function () { var a = __NP(); var i; for (i = 0; i < arguments.length; i++) a[i] = arguments[i]; a.n = arguments.length; __lines[__lines.n] = __fmtLine(a); __lines.n++; };
    if (__sp) { try { print = function (s) { console.log(String(s)); }; } catch (__e9) {} }
    try { eval(__SRC); } catch (err) { __e = err; }
    console.log = __sl;
    if (__sp) { try { print = __sp; } catch (__e8) {} }
    if (__ENGINE === 'dynajs') {
        assert_eq(__e !== null, true, "dynajs threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "ReferenceError", "dynajs error ctor");
        assert_eq(__e && __e.message, "'OP' is not defined", "dynajs error message");
        assert_eq(__lines.n, 1, "dynajs line count");
        assert_eq(__lines[0], "before 1", "dynajs line 1");
    }
    if (__ENGINE !== 'dynajs') {
        assert_eq(__e !== null, true, "node threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "ReferenceError", "node error ctor");
        assert_eq(__e && __e.message, "OP is not defined", "node error message");
        assert_eq(__lines.n, 1, "node line count");
        assert_eq(__lines[0], "before 1", "node line 1");
    }
});
summary("constprop_review");
