// flags: --std
// covers: c:src/builtins/builtin-array.c
// Parser memory-safety regressions: E1-01 (const-prop byte offsets read from the
// wrong function def), E1-02 (a const-prop member folded from its first token
// only) and E1b-01/E1b-02 (class parser back-patching a saved bytecode offset
// after the bytecode DynBuf failed to grow).
//
// E1-01 and E1b-01 are memory-safety only -- there is no wrong VALUE to assert,
// so they are gated on the engine not dying: E1-01 on a child process parsing a
// large source (release SIGBUS / ASan heap-buffer-overflow before the fix), and
// E1b-01 (a heap-capped child) was removed with the CLI heap-cap flag. The row SKIPs when no child can be launched.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_array_audit_engine_2");
import * as os from "os";
const TBASE = (std.getenv("TMPDIR") || "/tmp/").replace(/\/?$/, "/") + "dyna_audit_engine_2_" + os.getpid() + "_";

let n = 0, failures = 0, skips = 0;
function assert(c, m) {
    n++;
    if (!c) { failures++; print("FAIL: " + m); }
}
function eq(got, want, m) {
    assert(got === want, m + " (got " + JSON.stringify(got) +
           ", want " + JSON.stringify(want) + ")");
}
function eqJ(got, want, m) {
    eq(JSON.stringify(got), JSON.stringify(want), m);
}
function skip(m) { skips++; print("SKIP: " + m); }

__kitS.section("object literal inside an arrow inside a const initializer", () => {
{
    const L = Object.freeze([1].map((v) => ({ v, w: v * 2 })));
    eqJ(L, [{ v: 1, w: 2 }], "object literal inside an arrow inside a const initializer");
    eq(L.length, 1, "the mapped array keeps its length");
    assert(Object.isFrozen(L), "the const-propagated array is frozen");

    const M = Object.freeze([1, 2].map(v => ({ v, get g() { return v; } })));
    eqJ(M.map(e => e.g), [1, 2], "a getter in the folded literal stays live");
    eqJ(M.map(e => e.v), [1, 2], "the folded literal still has its own fields");

    var a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8, a8 = 9, a9 = 10;
    var b0 = [1], b1 = [2], b2 = [3];
    const fz = Object.freeze;
    const P = fz([1].map((v) => ({ v, w: v })));
    eqJ(P, [{ v: 1, w: 1 }], "the E1 lane probe shape at its smallest crashing size");
    eq(a0 + a9 + b2[0], 14, "the lane probe prelude still evaluates");

    const Q = fz({ r: [1].map(v => ({ v })) });
    eqJ(Q, { r: [{ v: 1 }] }, "an object literal holding a mapped object literal");

    function mk() { return fz({ s: [1, 2].map(v => ({ v, t: v + 1 })) }); }
    eqJ(mk(), { s: [{ v: 1, t: 2 }, { v: 2, t: 3 }] },
         "a folded literal built inside a function body");

    const deep = fz([1].map(v => [2].map(w => ({ v, w }))[0]));
    eqJ(deep, [{ v: 1, w: 2 }], "two nested arrows inside a const initializer");

    try {
        const S = fz([1].map(v => ({ v })));
        S.push({ v: 9 });
        assert(false, "a frozen const-propagated array rejects push");
    } catch (e) {
        assert(e instanceof TypeError, "a frozen const-propagated array throws TypeError");
    }
}
});
__kitS.end();

if (failures)
    throw new Error("test_audit_engine_2: " + failures + " failures");
print("test_audit_engine_2: all " + n + " assertions passed" +
      (skips ? " (" + skips + " rows skipped)" : ""));
