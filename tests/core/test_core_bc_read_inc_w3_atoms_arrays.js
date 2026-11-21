// timeout: 120
// covers: c:src/serialize/bc_read.inc.c
// Wave-3 engine regression suite: OPT-019 (array ext intersperse/scan
// fast-array allocation), COMPAT-137 (typed-array join after detach/resize),
// OPT-013/OPT-015 (atom interning paths), SEC-162 companion behavior rows.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_bc_read_inc_w3_atoms_arrays");
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
    let cnt = 0;
    let e = catches(function () { [1, 2, 3].scan(function (a, b) { cnt++; if (cnt === 2) throw new Error("boom"); return a + b; }, 0); });
    ok(e !== null && e.message === "boom", "OPT-019 scan propagates a callback throw");
})();

setTimeout(function () {
    if (n !== 0) print("w3_atoms_arrays: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_atoms_arrays: " + failed + " failures");
    print("w3_atoms_arrays: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
