// converted from a throw-contract probe: the original source is
// re-executed via eval and must throw (ctor/message pinned per engine)
__EXP = null;
test("rp28_classname_shadow.js must throw", function () {
    var __e = __mustThrow(function () { return eval("const OP = { HALT: 1 };\nclass OP { static get x() { return 5; } }\nconsole.log(OP.x, OP.HALT, typeof (new OP()));\n"); });
    assert(__e !== null, 'expected a throw, got none');
    assert_diverge(__e && __e.constructor.name, "SyntaxError", "SyntaxError", 'constructor');
    assert_diverge(__e && __e.message, "invalid redefinition of lexical identifier", "Identifier 'OP' has already been declared", 'message');
});
summary("constprop_review");
