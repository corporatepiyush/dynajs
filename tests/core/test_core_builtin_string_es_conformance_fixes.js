// covers: c:src/builtins/builtin-string.c
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
const S = sections("test_core_builtin_string_es_conformance_fixes");
S.section("a bound function inherits the target's prototype and natives keep their toString", () => {
    function T() {}
    Object.setPrototypeOf(T, null);
    check(Object.getPrototypeOf(Function.prototype.bind.call(T, null)) === null, "a bound function inherits the target's [[Prototype]]");
    check(String(function named() {}.bind()) === "function () {\n    [native code]\n}", "bound toString omits the 'bound named' name");
    check(String(Math.max).startsWith("function max("), "plain natives keep their name");
});
S.end();

print("test_core_builtin_string_es_conformance_fixes: " + n + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failures");
});
