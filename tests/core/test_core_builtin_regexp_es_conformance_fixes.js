// covers: c:src/builtins/builtin-regexp.c
// Spec behaviours fixed while taking test262 from 139 to 4 failures and enabling joint iteration, await-dictionary,
// legacy RegExp statics, immutable ArrayBuffers, string module export names and private-add-to-nonextensible.
// test262 is the bulk oracle; this file pins the cases it does not cover and the ones whose regression would be
// silent (a crash, a lost write, a value that looks plausible).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_regexp_es_conformance_fixes");
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsT(f, ctor, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ctor, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}

// EvalDeclarationInstantiation creates a var only when the binding does not exist: a second eval("var y") keeps y.

__kitS.section("groups and lastParen", () => {
    /(a)(b)?(c)/.exec("xxacyy");
    check(RegExp.$1 === "a" && RegExp.$2 === "" && RegExp.$3 === "c" && RegExp.lastParen === "c", "groups and lastParen");
    check(RegExp.leftContext === "xx" && RegExp.rightContext === "yy" && RegExp.input === "xxacyy", "contexts and input");
    "2026-10-26".replace(/(\d+)-(\d+)/, "_");
    check(RegExp.$1 === "2026" && RegExp.$2 === "10", "replace updates the statics");
    "a1b2".replace(/\d/g, "#");
    check(RegExp.lastMatch === "2", "a global replace leaves the last match");
    "q,w".split(/,/);
    check(RegExp.lastMatch === ",", "split updates the statics");
    class R extends RegExp {}
    new R("q").exec("q");
    throwsT(() => RegExp.$1, TypeError, "statics are unavailable after a subclass match");
    /z/.test("z");
    check(RegExp.lastMatch === "z", "the next legacy match restores them");
    throwsT(() => R.$1, TypeError, "a subclass receiver is refused");
    throwsT(() => new R("x").compile(), TypeError, "compile() is refused on subclass instances");
});
__kitS.end();

print("test_core_builtin_regexp_es_conformance_fixes: " + n + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failures");
