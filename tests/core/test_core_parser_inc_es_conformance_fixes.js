// covers: c:src/parser/parser.inc.c
// Spec behaviours fixed while taking test262 from 139 to 4 failures and enabling joint iteration, await-dictionary,
// legacy RegExp statics, immutable ArrayBuffers, string module export names and private-add-to-nonextensible.
// test262 is the bulk oracle; this file pins the cases it does not cover and the ones whose regression would be
// silent (a crash, a lost write, a value that looks plausible).
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsT(f, ctor, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ctor, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}

// EvalDeclarationInstantiation creates a var only when the binding does not exist: a second eval("var y") keeps y.

import("../lib/kit.js").then(async ({ sections }) => {
const S = sections("test_core_parser_inc_es_conformance_fixes");
S.section("EvalDeclarationInstantiation: a repeated eval var keeps the existing value", () => {
    function f() { eval("var y = 1"); eval("var y"); return y; }
    check(f() === 1, "a repeated eval var keeps the existing value");
    function g() { try { throw 8; } catch (x) { eval("var x = 42"); } return typeof x; }
    check(g() === "undefined", "eval var inside catch(x) creates the function-level binding (B.3.4)");
});

S.section("compound and logical assignment around eval and const", () => {
    function t() { var x = 3; var inner = (function () { x *= (eval("var x = 2;"), 4); return x; })(); return [inner, x]; }
    const [inner, outer] = t();
    check(inner === 2 && outer === 12, "x *= (eval('var x'), 4) assigns the outer x");
    function u() { const c = 1; let r; c ||= (r = "rhs"); return r; }
    check(u() === undefined, "a short-circuiting ||= on a const neither throws nor runs the right-hand side");
    throwsT(() => { const c = 0; c ||= 1; }, TypeError, "a ||= that does assign a const throws TypeError");
});

S.section("a call expression as an assignment target throws ReferenceError at run time", () => {
    let called = 0, rhs = 0;
    function f() { called++; return {}; }
    throwsT(() => { f() = (rhs++, 1); }, ReferenceError, "f() = 1 throws ReferenceError");
    check(called === 1 && rhs === 0, "the call runs, the right-hand side does not");
    throwsT(() => (0, eval)("'use strict'; f() = 1"), SyntaxError, "strict code keeps the early error");
    throwsT(() => (0, eval)("f`x` = 1"), SyntaxError, "a tagged template is not a call-expression target");
});

S.section("the Function constructor cannot be broken out of", () => {
    throwsT(() => new Function("/*", "*/) {"), SyntaxError, "a comment cannot straddle parameters and body");
    throwsT(() => new Function("", "}); globalThis.__pwned = 1; (function(){"), SyntaxError, "the body cannot close the function");
    check(globalThis.__pwned === undefined, "the injected statement never ran");
    check(new Function("return typeof anonymous")() === "undefined", "the dynamic function has no name binding");
    check(String(new Function("a", "return a")).startsWith("function anonymous(a"), "toString still names it anonymous");
});

S.section("set followed by a newline before *a() is a field and a generator", () => {
    const C = eval("(class { set\n *a() {} })");
    check(new C().hasOwnProperty("set") && typeof C.prototype.a === "function", "set<LF>*a(){} is a field and a generator");
});
S.end();

print("test_core_parser_inc_es_conformance_fixes: " + n + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failures");
});
