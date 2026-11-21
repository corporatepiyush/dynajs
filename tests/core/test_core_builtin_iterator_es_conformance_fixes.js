// covers: c:src/builtins/builtin-iterator.c
// Spec behaviours fixed while taking test262 from 139 to 4 failures and enabling joint iteration, await-dictionary,
// legacy RegExp statics, immutable ArrayBuffers, string module export names and private-add-to-nonextensible.
// test262 is the bulk oracle; this file pins the cases it does not cover and the ones whose regression would be
// silent (a crash, a lost write, a value that looks plausible).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_iterator_es_conformance_fixes");
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsT(f, ctor, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ctor, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}

// EvalDeclarationInstantiation creates a var only when the binding does not exist: a second eval("var y") keeps y.

__kitS.section("padding read twice and closed once", () => {
    let pulled = 0, closed = 0;
    const pad = { [Symbol.iterator]() { return { next() { pulled++; return { value: pulled, done: false }; }, return() { closed++; return {}; } }; } };
    const rows = [...Iterator.zip([[1], [2, 3]], { mode: "longest", padding: pad })];
    check(pulled === 2 && closed === 1, "padding read twice and closed once (" + pulled + "/" + closed + ")");
    check(JSON.stringify(rows) === "[[1,2],[1,3]]", "the first input is padded with padding[0]");
    const order = [];
    const mk = (tag) => ({ next() { return { value: 0, done: false }; }, return() { order.push(tag); return {}; } });
    const z = Iterator.zip([mk("a"), mk("b"), mk("c")]);
    z.next();
    z.return();
    check(order.join("") === "cba", "return() closes the inputs last to first");
    check(Object.getPrototypeOf(z) === Object.getPrototypeOf([].values().map((x) => x)), "zip results are Iterator Helpers");
    const k = Iterator.zipKeyed({ a: [1], b: [2] }).next().value;
    check(Object.getPrototypeOf(k) === null && k.a === 1 && k.b === 2, "zipKeyed yields null-prototype objects");
    throwsT(() => Iterator.zip([[1]], { mode: 1 }), TypeError, "a non-string mode is refused");
});
__kitS.end();

print("test_core_builtin_iterator_es_conformance_fixes: " + n + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failures");
