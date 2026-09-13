// converted from a throw-contract probe: the original source is
// re-executed via eval and must throw (ctor/message pinned per engine)
__EXP = null;
test("rp51_parse_error_atom.js must throw", function () {
    var __e = __mustThrow(function () { return eval("const OP = { HALT: 1 };\nconsole.log(OP.HALT);\n@@@ syntax error @@@\n"); });
    assert(__e !== null, 'expected a throw, got none');
    assert_diverge(__e && __e.constructor.name, "SyntaxError", "SyntaxError", 'constructor');
    assert_diverge(__e && __e.message, "unexpected token in expression: '@'", "Invalid or unexpected token", 'message');
});
summary("constprop_review");
