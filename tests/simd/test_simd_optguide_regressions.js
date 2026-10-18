// Split from tests/test_optguide_regressions.js: the simd sections.
import * as simd from "dyna:simd";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
{
    const run = (fn, vals) => { const a = new Float32Array(vals); simd[fn](a); return Array.from(a); };

    const sq = run("vsqrt", [0, 1, 4, 9, 100, 1e-8]);
    eq(sq[0], 0, "vsqrt(0) is 0, not NaN");
    for (const [i, v] of [[1, 1], [2, 4], [3, 9], [4, 100]]) {
        ok(Math.abs(sq[i] - Math.sqrt(v)) < 1e-3 * Math.max(1, Math.sqrt(v)),
           "vsqrt(" + v + ") ~ " + Math.sqrt(v) + " (got " + sq[i] + ")");
    }
    ok(sq.every((v) => !Number.isNaN(v)), "no NaN anywhere in the vsqrt result");

    const ex = run("vexp", [0, 1, 10, 80, 88, 90, 100, 200]);
    eq(ex[0], 1, "vexp(0) is 1");
    ok(ex[2] > 1e4 && ex[2] < 1e5, "vexp(10) is near 22026 (got " + ex[2] + ")");
    for (let i = 1; i < ex.length; i++) {
        ok(ex[i] >= ex[i - 1], "vexp is monotone at index " + i +
           " (" + ex[i - 1] + " -> " + ex[i] + ")");
    }
    ok(ex[5] > 1e38, "vexp(90) overflows UPWARD, not to zero (got " + ex[5] + ")");
    ok(ex[6] > 1e38, "vexp(100) likewise (got " + ex[6] + ")");
    ok(ex.every((v) => v >= 0), "vexp never returns a negative");
}
print("test_simd_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
