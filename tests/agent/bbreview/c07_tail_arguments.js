// converted from an escaping-throw probe: the original source is
// re-executed (output silenced) and its per-engine behavior —
// throw + emitted lines — asserted against baked expectations
__EXP = null;
test("c07_tail_arguments.js per-engine behavior contract", function () {
    var __SRC = "// C: tail calls passing `arguments` (mapped-arguments + frame teardown)\nfunction outer(n) { return mid(arguments); }\nfunction mid(args) { return inner(args.length); }\nfunction inner(x) { return \"len\" + x; }\nconsole.log(\"ar1\", outer(1, 2, 3));\nfunction f(n, args) { if (n === 0) return \"f\" + args.length + \":\" + args[0]; return g(n - 1, arguments); }\nfunction g(n, args) { if (n === 0) return \"g\" + args.length; return f(n - 1, args); }\nconsole.log(\"ar2s\", f(5, [9]));\ntry { console.log(\"ar2d\", f(100000, [\"deep\"])); } catch (e) { console.log(\"ar2d-overflow\", e.constructor.name); }\nfunction t(n) { if (n === 0) return \"t-done\"; return t(n - 1, arguments[0]); }\nconsole.log(\"ar3s\", t(5));\ntry { console.log(\"ar3d\", t(200000)); } catch (e) { console.log(\"ar3d-overflow\", e.constructor.name); }\n// arguments.callee + tail (sloppy only)\nfunction calleeChain(n) {\n  if (n === 0) return \"callee-done\";\n  return arguments.callee(n - 1);\n}\nconsole.log(\"ar4\", calleeChain(5));\ntry { console.log(\"ar4d\", calleeChain(100000)); } catch (e) { console.log(\"ar4d-overflow\", e.constructor.name); }\n// rest params variant (array, not mapped arguments)\nfunction restChain(n, ...rest) { if (n === 0) return \"rest:\" + rest.length; return restChain(n - 1, n); }\nconsole.log(\"ar5\", restChain(5), restChain(100000));\n";
    var __lines = __NP(); __lines.n = 0; var __e = null;
    var __sl = console.log; var __sp = (typeof print === 'function') ? print : null;
    console.log = function () { var a = __NP(); var i; for (i = 0; i < arguments.length; i++) a[i] = arguments[i]; a.n = arguments.length; __lines[__lines.n] = __fmtLine(a); __lines.n++; };
    if (__sp) { try { print = function (s) { console.log(String(s)); }; } catch (__e9) {} }
    try { eval(__SRC); } catch (err) { __e = err; }
    console.log = __sl;
    if (__sp) { try { print = __sp; } catch (__e8) {} }
    if (__ENGINE === 'dynajs') {
        assert_eq(__e !== null, false, "dynajs threw");
        assert_eq(__lines.n, 8, "dynajs line count");
        assert_eq(__lines[0], "ar1 len3", "dynajs line 1");
        assert_eq(__lines[1], "ar2s g2", "dynajs line 2");
        assert_eq(__lines[2], "ar2d f2:2", "dynajs line 3");
        assert_eq(__lines[3], "ar3s t-done", "dynajs line 4");
        assert_eq(__lines[4], "ar3d t-done", "dynajs line 5");
        assert_eq(__lines[5], "ar4 callee-done", "dynajs line 6");
        assert_eq(__lines[6], "ar4d callee-done", "dynajs line 7");
        assert_eq(__lines[7], "ar5 rest:1 rest:1", "dynajs line 8");
    }
    if (__ENGINE !== 'dynajs') {
        assert_eq(__e !== null, true, "node threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "RangeError", "node error ctor");
        assert_eq(__e && __e.message, "Maximum call stack size exceeded", "node error message");
        assert_eq(__lines.n, 7, "node line count");
        assert_eq(__lines[0], "ar1 len3", "node line 1");
        assert_eq(__lines[1], "ar2s g2", "node line 2");
        assert_eq(__lines[2], "ar2d-overflow RangeError", "node line 3");
        assert_eq(__lines[3], "ar3s t-done", "node line 4");
        assert_eq(__lines[4], "ar3d-overflow RangeError", "node line 5");
        assert_eq(__lines[5], "ar4 callee-done", "node line 6");
        assert_eq(__lines[6], "ar4d-overflow RangeError", "node line 7");
    }
});
summary("bbreview");
