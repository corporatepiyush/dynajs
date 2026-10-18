// mathx.solveSPD: the 4x4 Hilbert matrix with b = H * ones must return ones (Hilbert inverse entries are known
// integers, condition ~1.5e4, so ~1e-12 accuracy is expected); random SPD systems M M^T + n I are checked by their
// residual |A x - b|, computed here in plain JS; refusals for indefinite, asymmetric and mis-shaped input.
import { solveSPD } from "dyna:mathx";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name + ": " + e.message); } };

{
    const n = 4, H = new Float64Array(n * n), b = new Float64Array(n);
    for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) { H[i * n + j] = 1 / (i + j + 1); b[i] += H[i * n + j]; }
    const x = solveSPD(H, b);
    ok(x instanceof Float64Array && x.length === 4 && x.every((v) => Math.abs(v - 1) < 1e-10), "Hilbert(4) x = ones: " + Array.from(x).join(", "));
}
{
    const rng = new Random(9);
    let worst = 0;
    for (const n of [1, 2, 3, 8, 20, 64, 150]) {
        const M = Float64Array.from({ length: n * n }, () => rng.normal()), A = new Float64Array(n * n), b = Float64Array.from({ length: n }, () => rng.normal());
        for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) { let s = i === j ? n : 0; for (let k = 0; k < n; k++) s += M[i * n + k] * M[j * n + k]; A[i * n + j] = s; }
        const x = solveSPD(A, b);
        let r = 0, bn = 0;
        for (let i = 0; i < n; i++) { let s = -b[i]; for (let j = 0; j < n; j++) s += A[i * n + j] * x[j]; r += s * s; bn += b[i] * b[i]; }
        worst = Math.max(worst, Math.sqrt(r / bn));
    }
    ok(worst < 1e-12, "random SPD systems up to 150x150 solve to relative residual " + worst.toExponential(2));
    const A = Float64Array.of(4, 2, 2, 3), b = Float64Array.of(2, 1);
    const x = solveSPD(A, b);
    ok(Math.abs(x[0] - 0.5) < 1e-15 && Math.abs(x[1]) < 1e-15, "2x2 by hand: [[4,2],[2,3]] x = [2,1] -> [0.5, 0]");
    ok(A[0] === 4 && b[0] === 2, "inputs are not modified");
}
throwsT(() => solveSPD(Float64Array.of(1, 2, 2, 1), Float64Array.of(1, 1)), RangeError, "indefinite matrix refused");
throwsT(() => solveSPD(Float64Array.of(2, 1, 0, 2), Float64Array.of(1, 1)), RangeError, "asymmetric matrix refused");
throwsT(() => solveSPD(Float64Array.of(1, 0, 0), Float64Array.of(1, 1)), RangeError, "A not n x n refused");
throwsT(() => solveSPD(new Float64Array(0), new Float64Array(0)), RangeError, "empty system refused");
throwsT(() => solveSPD(Float64Array.of(NaN), Float64Array.of(1)), RangeError, "NaN refused");
throwsT(() => solveSPD(Float32Array.of(1), Float64Array.of(1)), TypeError, "Float32Array A refused");
throwsT(() => solveSPD([1], [1]), TypeError, "plain arrays refused");
print("test_mathx_solve_spd: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
