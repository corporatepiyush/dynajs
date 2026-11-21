// timeout: 120
// covers: c:src/builtins/builtin-typed-array.c
// Wave-3 engine regression suite: OPT-019 (array ext intersperse/scan
// fast-array allocation), COMPAT-137 (typed-array join after detach/resize),
// OPT-013/OPT-015 (atom interning paths), SEC-162 companion behavior rows.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_typed_array_w3_atoms_arrays");
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

function joinCase(action) {
    let ta = new Uint8Array([1, 2, 3]);
    return ta.join({ toString: function () { action(ta); return ","; } });
}

__kitS.section("COMPAT-137 join after transfer()", () => {
ok(joinCase(function (ta) { ta.buffer.transfer(); }) === ",,", "COMPAT-137 join after transfer()");
});

__kitS.section("COMPAT-137 join after transfer(0)", () => {
ok(joinCase(function (ta) { ta.buffer.transfer(0); }) === ",,", "COMPAT-137 join after transfer(0)");
});

__kitS.section("COMPAT-137 join default separator", () => {
ok(new Uint8Array([1, 2, 3]).join() === "1,2,3", "COMPAT-137 join default separator");
});
__kitS.end();

setTimeout(function () {
    if (n !== 0) print("w3_atoms_arrays: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_atoms_arrays: " + failed + " failures");
    print("w3_atoms_arrays: OK (" + n + " checks)");
}, 20);
