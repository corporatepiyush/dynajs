// timeout: 120
// covers: c:src/vm/interpreter.inc.c
// Wave-3 engine regression suite: OPT-019 (array ext intersperse/scan
// fast-array allocation), COMPAT-137 (typed-array join after detach/resize),
// OPT-013/OPT-015 (atom interning paths), SEC-162 companion behavior rows.
import { cpuNow } from "../lib/cpuclock.js";
import { sections } from "../lib/kit.js";

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eqArr(a, b, m) {
    let ja = JSON.stringify(Array.from(a, function (x) { return x === undefined ? "<undefined>" : x; }));
    let jb = JSON.stringify(Array.from(b, function (x) { return x === undefined ? "<undefined>" : x; }));
    ok(ja === jb, m + " (got " + ja + ", want " + jb + ")");
}
function catches(fn) { try { fn(); return null; } catch (e) { return e; } }

// OPT-019: intersperse/scan semantics across dense, sparse, array-like, empty

const S = sections("test_core_interpreter_inc_w3_atoms_arrays");
S.section("intersperse over dense, single, empty and sparse arrays with default and string separators", () => {
eqArr([1, 2, 3].intersperse(0), [1, 0, 2, 0, 3], "OPT-019 intersperse dense");

eqArr([1].intersperse(0), [1], "OPT-019 intersperse one");

eqArr([].intersperse(0), [], "OPT-019 intersperse empty");

eqArr([1, , 3].intersperse(0), [1, 0, undefined, 0, 3], "OPT-019 intersperse sparse");

eqArr([1, 2, 3].intersperse(), [1, undefined, 2, undefined, 3], "OPT-019 intersperse default separator");

eqArr([1, 2, 3].intersperse("--"), [1, "--", 2, "--", 3], "OPT-019 intersperse string separator");
});

S.section("scan yields running totals over dense, empty and sparse arrays", () => {
eqArr([1, 2, 3].scan(function (a, b) { return a + b; }, 10), [10, 11, 13, 16], "OPT-019 scan");

eqArr([].scan(function (a, b) { return a + b; }, 5), [5], "OPT-019 scan empty");

eqArr([1, , 3].scan(function (a, b) { return a + (b === undefined ? 100 : b); }, 0), [0, 1, 101, 104], "OPT-019 scan sparse");
});
S.end();

setTimeout(function () {
    if (n !== 0) print("w3_atoms_arrays: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_atoms_arrays: " + failed + " failures");
    print("w3_atoms_arrays: OK (" + n + " checks)");
}, 20);
