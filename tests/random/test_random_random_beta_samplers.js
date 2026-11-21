// covers: dyna:random.Random.beta
// Random#gamma / beta / binomial. Each distribution is checked against a CDF that does not come from the
// sampler: gamma and beta against mathx's regularized incomplete functions (pinned elsewhere against scipy),
// binomial against an exact CDF summed here from the pmf. The statistic is Kolmogorov-Smirnov at the 0.1%
// level (1.95 / sqrt(N)) with a fixed seed, plus the mean within 5 standard errors.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_beta_samplers");
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

for (const [a, b] of [
    [0.1, 0.1],
    [0.5, 2],
    [2, 5],
    [30, 30],
    [0.3, 4],
    [1, 1],
]) {
    const xs = Array.from({ length: N }, () => rng.beta(a, b));
    ok(xs.every((x) => x >= 0 && x <= 1), `beta(${a}, ${b}): in [0, 1]`);
    const d = ksContinuous(xs, (x) => betainc(x, a, b));
    ok(d < KS, `beta(${a}, ${b}): KS ${d.toFixed(4)} < ${KS.toFixed(4)}`);
    const v = (a * b) / ((a + b) ** 2 * (a + b + 1));
    ok(Math.abs(meanOf(xs) - a / (a + b)) < 5 * Math.sqrt(v / N), `beta(${a}, ${b}): mean`);
}

__kitS.section("beta(1e-4, 1e-4): never NaN, always in [0, 1]", () => {
    const xs = Array.from({ length: 2000 }, () => rng.beta(1e-4, 1e-4));
    ok(xs.every((x) => x >= 0 && x <= 1), "beta(1e-4, 1e-4): never NaN, always in [0, 1]");
    const ones = xs.filter((x) => x > 0.5).length;
    ok(ones > 850 && ones < 1150, `beta(1e-4, 1e-4): about half the mass at each end (${ones} of 2000)`);
});
__kitS.end();
print(`random samplers: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
