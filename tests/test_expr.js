/* test_expr.js -- the compiled Expression in dyna:mathx (design 26).
 *
 * THE ORACLE IS JAVASCRIPT ITSELF: every case states the same formula twice,
 * once as text for the evaluator and once as a JS arrow function, and the two
 * must agree to the last bit. That catches what a table of expected numbers
 * cannot -- an operator that binds the wrong way still produces *a* number.
 *
 * The reason this exists is that it is NOT eval: an identifier is a variable
 * or one of the listed functions, and nothing reaches an object or a scope.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_expr.js
 */
import { Expression } from "dyna:mathx";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const E = (s) => new Expression(s);

/* ------------------------------ the differential against JS itself */

{
    const M = Math;
    const cases = [
        ["1+2", () => 1 + 2],
        ["2*3+4", () => 2 * 3 + 4],
        ["2+3*4", () => 2 + 3 * 4],
        ["(2+3)*4", () => (2 + 3) * 4],
        ["10-4-3", () => 10 - 4 - 3],
        ["100/5/2", () => 100 / 5 / 2],
        ["2^3^2", () => 2 ** (3 ** 2)],
        ["-2^2", () => -(2 ** 2)],
        ["2^-2", () => 2 ** -2],
        ["-x^2", () => -(3 ** 2), { x: 3 }],
        ["-x*y", () => -3 * 4, { x: 3, y: 4 }],
        ["10%3", () => 10 % 3],
        ["-7%3", () => -7 % 3],
        ["1.5e2 + 0.5", () => 1.5e2 + 0.5],
        [".5*4", () => 0.5 * 4],
        ["x*2 + y/4 - 1", () => 3 * 2 + 8 / 4 - 1, { x: 3, y: 8 }],
        ["sin(x)", () => M.sin(0.7), { x: 0.7 }],
        ["cos(x)^2 + sin(x)^2", () => M.cos(0.7) ** 2 + M.sin(0.7) ** 2, { x: 0.7 }],
        ["sqrt(x*x + y*y)", () => M.sqrt(3 * 3 + 4 * 4), { x: 3, y: 4 }],
        ["hypot(3,4)", () => M.hypot(3, 4)],
        ["atan2(1,1)*4", () => M.atan2(1, 1) * 4],
        ["max(min(5, 3), 2)", () => M.max(M.min(5, 3), 2)],
        ["pow(2, 10)", () => M.pow(2, 10)],
        ["abs(-x)", () => M.abs(-5), { x: 5 }],
        ["floor(x) + ceil(x) + round(x)", () => M.floor(2.5) + M.ceil(2.5) + M.round(2.5), { x: 2.5 }],
        ["trunc(-2.7)", () => M.trunc(-2.7)],
        ["log(exp(x))", () => M.log(M.exp(2)), { x: 2 }],
        ["log2(1024) + log10(1000)", () => M.log2(1024) + M.log10(1000)],
        ["cbrt(27)", () => M.cbrt(27)],
        ["tanh(x) + sinh(x) + cosh(x)", () => M.tanh(1) + M.sinh(1) + M.cosh(1), { x: 1 }],
        ["expm1(x) + log1p(x)", () => M.expm1(0.5) + M.log1p(0.5), { x: 0.5 }],
        ["sign(-3) + sign(0) + sign(9)", () => M.sign(-3) + M.sign(0) + M.sign(9)],
        ["fmod(10.5, 3)", () => 10.5 % 3],
        ["pi", () => M.PI],
        ["e", () => M.E],
        ["2*pi*r", () => 2 * M.PI * 1.5, { r: 1.5 }],
        ["((x))", () => 7, { x: 7 }],
        ["+5", () => 5],
        ["1/0", () => 1 / 0],
        ["0/0", () => 0 / 0],
    ];
    let bad = 0;
    for (const [src, fn, vars] of cases) {
        const got = E(src).eval(vars || {});
        const want = fn();
        const same = (got !== got && want !== want) || got === want;
        if (!same) {
            bad++;
            print("  " + src + " = " + got + ", JS says " + want);
        }
    }
    assert(bad === 0, "every expression agrees with the same formula in JS ("
                      + (cases.length - bad) + "/" + cases.length + ")");
    assert(cases.length === 40, "the differential ran all 40 formulas");
}

/* --------------------------------------------------------- variables */

eq(JSON.stringify(E("a*b + c").variables()), '["a","b","c"]',
   "variables() lists them in first-use order");
eq(JSON.stringify(E("x + x*x").variables()), '["x"]', "each one once");
eq(JSON.stringify(E("1+2").variables()), "[]", "a constant expression has none");
eq(JSON.stringify(E("pi*e").variables()), "[]", "and pi and e are constants, not variables");
eq(E("1+1").eval(), 2, "a constant expression needs no argument");

/* --------------------------------------------- it is NOT an evaluator */

throws(() => E("x").eval({ get x() { return 1; } }),
       "a GETTER is refused: this exists precisely so that no code runs");
throws(() => E("constructor").eval({}), "an unbound name is not resolved from anywhere");
throws(() => E("x").eval({ x: "5" }), "a string is not coerced into a number");
throws(() => E("x").eval({}), "a missing variable names itself");
eq(E("toString").variables()[0], "toString",
   "an inherited property name is an ordinary variable, not a lookup");
throws(() => E("toString").eval({}), "and it has no value unless you pass one");

/* ------------------------------------------------------------ refusals */

for (const bad of ["", "   ", "1+", "+", "(1", "1)", "()", "1 2", "*2", "x y",
                   "foo(1)", "min(1)", "max(1,2,3)", "sin()", "1,2", "$x",
                   "1..2", "x@y", "2^", "((1)"])
    throws(() => E(bad), "refuses " + JSON.stringify(bad));
throws(() => E(42), "the source must be a string");
throws(() => E(), "and is required");
throws(() => E("x".repeat(5000)), "an over-long source");
throws(() => Expression.prototype.eval.call({}, {}), "a foreign receiver");
{
    /* Nesting is bounded, so a pathological source is a SyntaxError not a
     * blown C stack. */
    throws(() => E("(".repeat(500) + "1" + ")".repeat(500)), "500-deep parens");
    eq(E("(".repeat(100) + "1" + ")".repeat(100)).eval({}), 1, "and 100 is fine");
}
{
    /* A compiled expression is reusable -- that is why it is a class. */
    const f = E("a*a + b");
    eq(f.eval({ a: 2, b: 1 }), 5, "first use");
    eq(f.eval({ a: 3, b: 2 }), 11, "second use, same compiled program");
    eq(f.eval({ a: 0, b: 0 }), 0, "third");
}

/* ------------------------------------------------- min/max propagate NaN */

{
    /* Math.min/Math.max semantics: a NaN argument poisons the result. The
     * old C-style a<b?a:b silently DROPPED it: min(NaN,1) returned 1. */
    assert(Number.isNaN(E("min(x, 1)").eval({ x: NaN })), "min(NaN,1) is NaN");
    assert(Number.isNaN(E("min(1, x)").eval({ x: NaN })), "min(1,NaN) is NaN");
    assert(Number.isNaN(E("max(x, 1)").eval({ x: NaN })), "max(NaN,1) is NaN");
    assert(Number.isNaN(E("max(1, x)").eval({ x: NaN })), "max(1,NaN) is NaN");
    assert(Number.isNaN(E("max(x, y)").eval({ x: NaN, y: NaN })), "max(NaN,NaN) is NaN");
    /* and the min/max cases of the JS differential above (no NaN involved)
     * still behave exactly as before the NaN guard went in */
    eq(E("min(5, 3)").eval({}), 3, "min of plain numbers unchanged");
    eq(E("max(5, 3)").eval({}), 5, "max of plain numbers unchanged");
}

/* ------------------------------------- value-stack depth counts OP_NEG too */

{
    /* The compile-time depth must treat unary minus symmetrically in BOTH
     * pop paths (precedence and paren-close): NEG folds in place and takes
     * no value-stack slot, so it must never shrink the count. Asymmetric
     * counting drove the unsigned depth below zero at "(-(-(-...1...)))"
     * (one wrap per NEG pop past the first), the wrapped count then survived
     * into the next value push and poisoned max_depth -- these legal
     * programs were refused with an InternalError. */
    eq(E("(-(-(-1)))+(1+1)").eval({}), 1, "triple paren-nested unary minus + ops parses");
    eq(E("(-(-(-1)))+(1+1+1+1)").eval({}), 3, "same shape, deeper right side");
    eq(E("(-(-1))+(1+1)").eval({}), 3, "double paren-nested unary minus parses");
    eq(E("min((-(-1)), 2)").eval({}), 1, "NEG-in-parens inside a call argument");
    /* near the shunt-stack limit (256 slots; each "(-" costs two) */
    eq(E("(-".repeat(120) + "1" + ")".repeat(120)).eval({}), 1,
       "120 paren-nested unary minuses parse near the limit");
    /* ...and clearly beyond refuses cleanly (SyntaxError, not InternalError) */
    throws(() => E("(-".repeat(130) + "1" + ")".repeat(130)),
           "130 paren-nested unary minuses exceed the limit");
}

if (fails) {
    print("test_expr: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_expr failed");
}
print("test_expr: " + n + " assertions, 0 failures");
