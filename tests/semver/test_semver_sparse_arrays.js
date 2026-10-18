// dyna:semver version lists are read entry by entry into buffers that grow as
// entries validate: a sparse array of length 2^32-1 used to request 5 TB up
// front (three arrays sized by the declared length) before entry 0 was read,
// found by the dyna:semver input sweep under ASan. On macOS the old code
// passes these assertions too (the allocations are lazy and entry 0 still
// fails), so the san stage, which runs this suite, is where it goes red.
import * as semver from "dyna:semver";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
for (const name of ["sort", "rsort", "maxSatisfying", "minSatisfying"]) {
    if (typeof semver[name] !== "function") continue;
    const args = name.endsWith("Satisfying") ? [huge(), "^1.0.0"] : [huge()];
    const t0 = performance.now();
    eq(err(() => semver[name](...args)), "TypeError: dyna:semver: every array element must be a string", name + ": a sparse list is refused at index 0, not as out-of-memory");
    ok(performance.now() - t0 < 200, name + ": before any allocation by the declared length");
}
eq(err(() => semver.sort(["1.0.0", 2])), "TypeError: dyna:semver: every array element must be a string", "a number entry is refused");
eq(JSON.stringify(semver.sort(Array.from({ length: 40 }, (_, i) => "1." + (39 - i) + ".0")).slice(0, 3)), '["1.0.0","1.1.0","1.2.0"]', "a list past the first growth step sorts (control)");

print("test_semver_sparse_arrays: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
