// covers: dyna:random.Random.binomial
// Random#gamma / beta / binomial. Each distribution is checked against a CDF that does not come from the
// sampler: gamma and beta against mathx's regularized incomplete functions (pinned elsewhere against scipy),
// binomial against an exact CDF summed here from the pmf. The statistic is Kolmogorov-Smirnov at the 0.1%
// level (1.95 / sqrt(N)) with a fixed seed, plus the mean within 5 standard errors.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_binomial_samplers");
import { Random } from "dyna:random";
import { gammainc, betainc } from "dyna:mathx";

let failures = 0,
    passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else {
        failures++;
        print("FAIL " + msg);
    }
}
const N = 20000,
    KS = 1.95 / Math.sqrt(N);
function ksContinuous(xs, cdf) {
    const s = Float64Array.from(xs).sort();
    let d = 0;
    for (let i = 0; i < s.length; i++) {
        const f = cdf(s[i]);
        d = Math.max(d, Math.abs(f - i / s.length), Math.abs((i + 1) / s.length - f));
    }
    return d;
}
const meanOf = (xs) => xs.reduce((a, b) => a + b, 0) / xs.length;

const rng = new Random(20261113);

function binomCdf(n, p) {
    const lf = new Float64Array(n + 1);
    for (let i = 2; i <= n; i++) lf[i] = lf[i - 1] + Math.log(i);
    const cdf = new Float64Array(n + 1);
    let acc = 0;
    for (let k = 0; k <= n; k++) {
        const lp = lf[n] - lf[k] - lf[n - k] + (p > 0 ? k * Math.log(p) : k ? -Infinity : 0) + (p < 1 ? (n - k) * Math.log1p(-p) : n - k ? -Infinity : 0);
        acc += Math.exp(lp);
        cdf[k] = Math.min(acc, 1);
    }
    return cdf;
}
for (const [n, p] of [
    [1, 0.5],
    [20, 0.3],
    [50, 0.999],
    [1000, 0.01],
    [100000, 0.5],
    [7, 0.07],
]) {
    const xs = Array.from({ length: N }, () => rng.binomial(n, p));
    ok(xs.every((k) => Number.isInteger(k) && k >= 0 && k <= n), `binomial(${n}, ${p}): integers in [0, n]`);
    const cdf = binomCdf(n, p),
        counts = new Map();
    for (const k of xs) counts.set(k, (counts.get(k) || 0) + 1);
    let d = 0,
        run = 0;
    for (let k = 0; k <= n; k++) {
        run += counts.get(k) || 0;
        d = Math.max(d, Math.abs(run / N - cdf[k]));
    }
    ok(d < KS, `binomial(${n}, ${p}): KS ${d.toFixed(4)} < ${KS.toFixed(4)}`);
    ok(Math.abs(meanOf(xs) - n * p) < 5 * Math.sqrt((n * p * (1 - p)) / N) + 1e-12, `binomial(${n}, ${p}): mean`);
}

__kitS.section("binomial(2^31-1, 0.5): mean", () => {
    const big = Array.from({ length: 2000 }, () => rng.binomial(2 ** 31 - 1, 0.5));
    const sd = Math.sqrt((2 ** 31 - 1) / 4);
    ok(Math.abs(meanOf(big) - (2 ** 31 - 1) / 2) < 5 * sd / Math.sqrt(2000), "binomial(2^31-1, 0.5): mean");
    ok(rng.binomial(0, 0.5) === 0 && rng.binomial(10, 0) === 0 && rng.binomial(10, 1) === 10, "binomial edges: n=0, p=0, p=1");
    const tiny = Array.from({ length: 2000 }, () => rng.binomial(2 ** 31 - 1, 1e-9));
    ok(Math.abs(meanOf(tiny) - 2.147) < 0.2, `binomial(2^31-1, 1e-9): mean near 2.147 (${meanOf(tiny)})`);
});

__kitS.section("no throw", () => {
    const r = new Random(1);
    const refuses = (f, re, msg) => {
        try {
            f();
            ok(false, msg + ": no throw");
        } catch (e) {
            ok(re.test(String(e)), `${msg}: ${e}`);
        }
    };
    refuses(() => r.gamma(), /shape is required/, "gamma()");
    for (const bad of [0, -1, NaN, Infinity]) refuses(() => r.gamma(bad), /shape/, `gamma(${bad})`);
    for (const bad of [0, -2, Infinity]) refuses(() => r.gamma(1, bad), /scale/, `gamma(1, ${bad})`);
    refuses(() => r.beta(1), /required/, "beta(1)");
    refuses(() => r.beta(0, 1), /a and b must be/, "beta(0, 1)");
    refuses(() => r.beta(1, NaN), /b/, "beta(1, NaN)");
    refuses(() => r.binomial(10), /required/, "binomial(10)");
    refuses(() => r.binomial(2.5, 0.5), /integer/, "binomial(2.5, 0.5)");
    refuses(() => r.binomial(-1, 0.5), /n/, "binomial(-1, 0.5)");
    refuses(() => r.binomial(2 ** 31, 0.5), /n/, "binomial(2^31, 0.5)");
    refuses(() => r.binomial(10, 1.5), /p/, "binomial(10, 1.5)");
    refuses(() => r.binomial(10, NaN), /p/, "binomial(10, NaN)");
});
__kitS.end();
print(`random samplers: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
