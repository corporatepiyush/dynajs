// Random.fillNormal / fillUniform / fillExponential: distribution checks against the DEFINITIONS (normal CDF from
// mathx.erf, exponential CDF 1 - e^{-lambda x}, uniform bounds), reproducibility, and the refusals.
import { Random } from "dyna:random";
import { erf } from "dyna:mathx";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

// Kolmogorov-Smirnov statistic of a sample against a CDF; 1.63 / sqrt(n) is the 1% critical value.
function ks(xs, cdf) {
    const s = Float64Array.from(xs).sort();
    let d = 0;
    for (let i = 0; i < s.length; i++) { const f = cdf(s[i]); d = Math.max(d, f - i / s.length, (i + 1) / s.length - f); }
    return d;
}
const N = 200_000;
for (const T of [Float64Array, Float32Array]) {
    const name = T.name;
    const a = new T(N);
    const r = new Random(7);
    ok(r.fillNormal(a, 3, 2) === r, `${name} fillNormal returns this`);
    const mean = a.reduce((s, x) => s + x, 0) / N, v = a.reduce((s, x) => s + (x - mean) ** 2, 0) / N;
    ok(Math.abs(mean - 3) < 4 * 2 / Math.sqrt(N), `${name} normal mean ${mean}`);
    ok(Math.abs(v - 4) < 0.05, `${name} normal variance ${v}`);
    ok(ks(a, (x) => 0.5 * (1 + erf((x - 3) / (2 * Math.SQRT2)))) < 1.63 / Math.sqrt(N), `${name} normal passes KS against the erf CDF`);

    const u = new T(N);
    new Random(8).fillUniform(u, -5, 7);
    ok(u.every((x) => x >= -5 && x < 7), `${name} uniform stays in [-5, 7)`);
    ok(ks(u, (x) => (x + 5) / 12) < 1.63 / Math.sqrt(N), `${name} uniform passes KS`);

    const e = new T(N);
    new Random(9).fillExponential(e, 2.5);
    ok(e.every((x) => x >= 0), `${name} exponential is non-negative`);
    ok(ks(e, (x) => 1 - Math.exp(-2.5 * x)) < 1.63 / Math.sqrt(N), `${name} exponential passes KS against 1 - e^-lx`);
}
{
    const x = new Float64Array(1001), y = new Float64Array(1001);
    new Random(42).fillNormal(x); new Random(42).fillNormal(y);
    ok(x.every((v, i) => v === y[i]), "same seed, same normals");
    ok(new Random(1).fillNormal(new Float64Array(5), 9, 0) && new Float64Array(5).fill(0).length === 5, "sigma 0 accepted");
    const z = new Float32Array(6);
    new Random(1).fillNormal(z, 9, 0);
    ok(z.every((v) => v === 9), "sigma 0 fills mu");
    const odd = new Float64Array(3);
    new Random(3).fillNormal(odd);
    ok(odd.every(Number.isFinite), "an odd length uses half of the last pair");
    const f = new Float32Array(400_000);
    new Random(5).fillUniform(f, 0, 1e-30);
    ok(f.every((v) => v >= 0 && v < 1e-30 && Math.fround(1e-30) !== v), "float32 rounding never reaches hi");
    const sub = new Float64Array(10);
    new Random(2).fillUniform(sub.subarray(3, 6), 1, 2);
    ok(sub.every((v, i) => (i >= 3 && i < 6 ? v >= 1 && v < 2 : v === 0)), "a subarray view fills only its window");
}
{
    const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name); } };
    const r = new Random(1);
    throwsT(() => r.fillNormal(new Int32Array(4)), TypeError, "Int32Array refused");
    throwsT(() => r.fillNormal([1, 2]), TypeError, "plain array refused");
    throwsT(() => r.fillNormal(new Float64Array(4), 0, -1), RangeError, "negative sigma refused");
    throwsT(() => r.fillUniform(new Float64Array(4), 2, 2), RangeError, "hi == lo refused");
    throwsT(() => r.fillUniform(new Float64Array(4), -1e308, 1e308), RangeError, "infinite width refused");
    throwsT(() => r.fillExponential(new Float64Array(4), 0), RangeError, "lambda 0 refused");
    throwsT(() => r.fillNormal(new Float64Array(4), NaN), RangeError, "NaN mu refused");
    const ta = new Float64Array(8);
    const hostile = { valueOf() { ta.buffer.transfer(); return 0; } };
    throwsT(() => r.fillNormal(ta, hostile), TypeError, "a buffer detached by argument coercion is refused, not written");
}
print("test_random_fill_dist: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
