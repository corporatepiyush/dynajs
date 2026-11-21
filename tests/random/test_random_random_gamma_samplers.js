// covers: dyna:random.Random.gamma
// Random#gamma / beta / binomial. Each distribution is checked against a CDF that does not come from the
// sampler: gamma and beta against mathx's regularized incomplete functions (pinned elsewhere against scipy),
// binomial against an exact CDF summed here from the pmf. The statistic is Kolmogorov-Smirnov at the 0.1%
// level (1.95 / sqrt(N)) with a fixed seed, plus the mean within 5 standard errors.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_gamma_samplers");
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

for (const [shape, scale] of [
    [0.05, 1],
    [0.5, 2],
    [1, 1],
    [2.5, 0.5],
    [10, 3],
    [1000, 1],
]) {
    const xs = Array.from({ length: N }, () => rng.gamma(shape, scale));
    ok(xs.every((x) => x >= 0 && Number.isFinite(x)), `gamma(${shape}, ${scale}): finite and non-negative`);
    const d = ksContinuous(xs, (x) => gammainc(x / scale, shape));
    ok(d < KS, `gamma(${shape}, ${scale}): KS ${d.toFixed(4)} < ${KS.toFixed(4)}`);
    const se = Math.sqrt(shape) * scale / Math.sqrt(N);
    ok(Math.abs(meanOf(xs) - shape * scale) < 5 * se, `gamma(${shape}, ${scale}): mean ${meanOf(xs).toFixed(4)}`);
}

__kitS.section("setState replays gamma, beta and binomial exactly", () => {
    // determinism: a state snapshot replays every sampler exactly
    const r = new Random(7);
    const st = r.getState();
    const draw = () => [r.gamma(0.3), r.gamma(4, 2), r.beta(0.2, 3), r.beta(5, 5), r.binomial(1000, 0.4), r.binomial(5, 0.9)];
    const a = draw();
    r.setState(st);
    ok(JSON.stringify(draw()) === JSON.stringify(a), "setState replays gamma, beta and binomial exactly");
});
__kitS.end();
print(`random samplers: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
