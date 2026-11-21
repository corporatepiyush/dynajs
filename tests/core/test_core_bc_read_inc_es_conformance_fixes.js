// covers: c:src/serialize/bc_read.inc.c
// Spec behaviours fixed while taking test262 from 139 to 4 failures and enabling joint iteration, await-dictionary,
// legacy RegExp statics, immutable ArrayBuffers, string module export names and private-add-to-nonextensible.
// test262 is the bulk oracle; this file pins the cases it does not cover and the ones whose regression would be
// silent (a crash, a lost write, a value that looks plausible).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_bc_read_inc_es_conformance_fixes");
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsT(f, ctor, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ctor, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}

// EvalDeclarationInstantiation creates a var only when the binding does not exist: a second eval("var y") keeps y.

__kitS.section("a bad descriptor throws", () => {
    const o = {};
    throwsT(() => Object.defineProperties(o, { a: { value: 1 }, b: { get: 1 } }), TypeError, "a bad descriptor throws");
    check(!("a" in o), "nothing is defined when a later descriptor is invalid");
});
__kitS.end();

print("test_core_bc_read_inc_es_conformance_fixes: " + n + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failures");
