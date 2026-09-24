// converted from an escaping-throw probe: the original source is
// re-executed (output silenced) and its per-engine behavior —
// throw + emitted lines — asserted against baked expectations
__EXP = null;
test("c01_tail_try.js per-engine behavior contract", function () {
    var __SRC = "// C: tail calls \u00d7 try/catch (frame teardown must not lose handlers)\nfunction deepThrow(n) { if (n === 0) throw new Error(\"deep-boom\"); return deepThrow(n - 1); }\nfunction t1() { try { return deepThrow(3); } catch (e) { return \"caught:\" + e.message; } }\nconsole.log(\"t1\", t1());\nfunction t1deep() { try { return deepThrow(200000); } catch (e) { return \"caught:\" + e.message; } }\nconsole.log(\"t1d\", t1deep());\nfunction f(n) { if (n === 0) return \"done\"; return g(n - 1); }\nfunction g(n) { if (n === 0) return \"done-g\"; try { throw new Error(\"e\" + n); } catch (e) { return f(n - 1); } }\nconsole.log(\"c1s\", f(5));\ntry { console.log(\"c1\", f(100000)); } catch (e) { console.log(\"c1-overflow\", e.constructor.name); }\nlet fin = 0;\nfunction tf(n) { try { if (n === 0) return \"tf-done\"; return tf(n - 1); } finally { fin++; } }\nconsole.log(\"tf\", tf(5), fin);\nlet fin2 = 0;\nfunction tfdeep(n) { try { if (n === 0) return \"tfd-done\"; return tfdeep(n - 1); } finally { fin2++; } }\nconsole.log(\"tfd\", tfdeep(100000), fin2);\nfunction a(n) { if (!n) return 0; try { return b(n - 1); } catch (e) { return -1; } }\nfunction b(n) { if (!n) return 1; return a(n - 1); }\nconsole.log(\"ab\", a(10), a(9));\n// tail inside try where the DEEP frame throws a non-Error\nfunction deepStr(n) { if (n === 0) throw \"string-boom\"; return deepStr(n - 1); }\nfunction strCatch() { try { return deepStr(150000); } catch (e) { return \"sc:\" + e; } }\nconsole.log(\"sc\", strCatch());\n";
    var __lines = __NP(); __lines.n = 0; var __e = null;
    var __sl = console.log; var __sp = (typeof print === 'function') ? print : null;
    console.log = function () { var a = __NP(); var i; for (i = 0; i < arguments.length; i++) a[i] = arguments[i]; a.n = arguments.length; __lines[__lines.n] = __fmtLine(a); __lines.n++; };
    if (__sp) { try { print = function (s) { console.log(String(s)); }; } catch (__e9) {} }
    try { eval(__SRC); } catch (err) { __e = err; }
    console.log = __sl;
    if (__sp) { try { print = __sp; } catch (__e8) {} }
    if (__ENGINE === 'dynajs') {
        assert_eq(__e !== null, true, "dynajs threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "RangeError", "dynajs error ctor");
        assert_eq(__e && __e.message, "Maximum call stack size exceeded", "dynajs error message");
        assert_eq(__lines.n, 5, "dynajs line count");
        assert_eq(__lines[0], "t1 caught:deep-boom", "dynajs line 1");
        assert_eq(__lines[1], "t1d caught:deep-boom", "dynajs line 2");
        assert_eq(__lines[2], "c1s done-g", "dynajs line 3");
        assert_eq(__lines[3], "c1-overflow RangeError", "dynajs line 4");
        assert_eq(__lines[4], "tf tf-done 6", "dynajs line 5");
    }
    if (__ENGINE !== 'dynajs') {
        assert_eq(__e !== null, true, "node threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "RangeError", "node error ctor");
        assert_eq(__e && __e.message, "Maximum call stack size exceeded", "node error message");
        assert_eq(__lines.n, 5, "node line count");
        assert_eq(__lines[0], "t1 caught:deep-boom", "node line 1");
        assert_eq(__lines[1], "t1d caught:Maximum call stack size exceeded", "node line 2");
        assert_eq(__lines[2], "c1s done-g", "node line 3");
        assert_eq(__lines[3], "c1-overflow RangeError", "node line 4");
        assert_eq(__lines[4], "tf tf-done 6", "node line 5");
    }
});
summary("bbreview");
