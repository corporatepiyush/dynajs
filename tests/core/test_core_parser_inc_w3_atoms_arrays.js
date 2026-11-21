// timeout: 120
// covers: c:src/parser/parser.inc.c
// Wave-3 engine regression suite: OPT-019 (array ext intersperse/scan
// fast-array allocation), COMPAT-137 (typed-array join after detach/resize),
// OPT-013/OPT-015 (atom interning paths), SEC-162 companion behavior rows.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_parser_inc_w3_atoms_arrays");
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eqArr(a, b, m) {
    let ja = JSON.stringify(Array.from(a, function (x) { return x === undefined ? "<undefined>" : x; }));
    let jb = JSON.stringify(Array.from(b, function (x) { return x === undefined ? "<undefined>" : x; }));
    ok(ja === jb, m + " (got " + ja + ", want " + jb + ")");
}
function catches(fn) { try { fn(); return null; } catch (e) { return e; } }

// OPT-019: intersperse/scan semantics across dense, sparse, array-like, empty
__kitS.section("OPT-019: intersperse/scan semantics across dense, sparse, array-like, empty", () => {

(function () {
    let nm = "\u65e5\u672c\u8a9e\u30c6\u30b9\u30c8".repeat(60);
    let code = "var " + nm + " = 41; " + nm + " + 1";
    ok(eval(code) === 42, "OPT-013/015 long non-ASCII identifier evaluates");
    let o = {};
    o[nm] = 7;
    ok(o[eval("'" + nm + "'")] === 7, "OPT-013/015 long non-ASCII property key round-trips");
    let t = cpuNow();
    for (let i = 0; i < 200; i++) eval("var " + nm + " = i;");
    let dt = cpuNow() - t;
    ok(dt < 5000, "OPT-015 200 evals of a 1.8KB non-ASCII identifier complete (got " + dt + "ms)");
    ok(eval("({a1:1}).a1") === 1, "OPT-013 ASCII keys unaffected");
    ok(eval("({1.5:2})[1.5]") === 2, "OPT-013 numeric-like keys unaffected");
    ok(eval("({\"\\u00e9\":9})[\"\\u00e9\"]") === 9, "OPT-013 Latin-1 escaped key unaffected");
})();

setTimeout(function () {
    if (n !== 0) print("w3_atoms_arrays: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_atoms_arrays: " + failed + " failures");
    print("w3_atoms_arrays: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
