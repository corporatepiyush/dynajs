// converted from a throw-contract probe: the original source is
// re-executed via eval and must throw (ctor/message pinned per engine)
__EXP = null;
test("a08_negpow.js must throw", function () {
    var __e = __mustThrow(function () { return eval("// A: `-2**2` must be a SyntaxError in BOTH engines (whole file must fail to parse)\nconsole.log(\"never-reached\", -2 ** 2);\n"); });
    assert(__e !== null, 'expected a throw, got none');
    assert_diverge(__e && __e.constructor.name, "SyntaxError", "SyntaxError", 'constructor');
    assert_diverge(__e && __e.message, "unparenthesized unary expression can't appear on the left-hand side of '**'", "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence", 'message');
});
summary("bbreview");
