// converted from a throw-contract probe: the original source is
// re-executed via eval and must throw (ctor/message pinned per engine)
__EXP = null;
test("rp55_eval_loop_noconst.js must throw", function () {
    var __e = __mustThrow(function () { return eval("// rp54: many separately-compiled scripts each declaring a const object.\n// Patched leaks cp_binds/cp_sites (success path never frees them).\nfor (let i = 0; i < 50000; i++) {\n    (0, eval)(\"true;\n}\nconsole.log(\"done\", typeof Q1);\n"); });
    assert(__e !== null, 'expected a throw, got none');
    assert_diverge(__e && __e.constructor.name, "SyntaxError", "SyntaxError", 'constructor');
    assert_diverge(__e && __e.message, "unexpected end of string", "Invalid or unexpected token", 'message');
});
summary("constprop_review");
