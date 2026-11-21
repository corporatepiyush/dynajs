// covers: c:src/builtins/builtin-promise.c
// Spec behaviours fixed while taking test262 from 139 to 4 failures and enabling joint iteration, await-dictionary,
// legacy RegExp statics, immutable ArrayBuffers, string module export names and private-add-to-nonextensible.
// test262 is the bulk oracle; this file pins the cases it does not cover and the ones whose regression would be
// silent (a crash, a lost write, a value that looks plausible).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_promise_es_conformance_fixes");
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsT(f, ctor, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ctor, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}

// EvalDeclarationInstantiation creates a var only when the binding does not exist: a second eval("var y") keeps y.

__kitS.section("allKeyed resolves a keyed null-prototype object", () => {
    const s = Symbol("s");
    let got = null, settled = null;
    Promise.allKeyed({ b: Promise.resolve(2), a: 1, [s]: Promise.resolve("sym") }).then((o) => (got = o));
    Promise.allSettledKeyed({ ok: 1, bad: Promise.reject("why") }).then((o) => (settled = o));
    queueMicrotask(() => queueMicrotask(() => queueMicrotask(() => queueMicrotask(() => {
        check(got && Object.getPrototypeOf(got) === null && got.b === 2 && got.a === 1 && got[s] === "sym", "allKeyed resolves a keyed null-prototype object");
        check(got && Reflect.ownKeys(got).map(String).join(",") === "b,a,Symbol(s)", "key order follows the input");
        check(settled && settled.bad.status === "rejected" && settled.ok.value === 1, "allSettledKeyed records each outcome");
        print("test_es_conformance_fixes: " + n + " checks, " + fails + " failed");
        if (fails) throw new Error(fails + " failures");
    }))));
});
__kitS.end();
