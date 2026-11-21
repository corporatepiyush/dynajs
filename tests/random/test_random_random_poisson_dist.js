// covers: dyna:random.Random.poisson
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_poisson_dist");
import { Random } from "dyna:random";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function closeTo(got, want, tol, msg) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + msg + " (got " + got +
                        ", want " + want + " +- " + tol + ")");
}

const N = 100000;

for (const lambda of [0.5, 5, 29, 30, 100, 1000]) {
    const r = new Random(31337 + lambda);
    let sum = 0, sum2 = 0, count = 0;
    for (let i = 0; i < N; i++) {
        const k = r.poisson(lambda);
        assert(Number.isInteger(k), "poisson returns integers");
        assert(k >= 0, "poisson non-negative");
        sum += k;
        sum2 += k * k;
        count++;
    }
    const mean = sum / count, variance = sum2 / count - mean * mean;
    closeTo(mean, lambda, 5 * Math.sqrt(lambda / N),
            "poisson(" + lambda + ") mean");
    closeTo(variance, lambda, 5 * Math.sqrt(2 * lambda) + 5 * lambda / Math.sqrt(N) + 1e-9,
            "poisson(" + lambda + ") variance");
}

__kitS.section("poisson(0) is the point mass at 0", () => {
    const r = new Random(5);
    const s0 = r.getState().join(",");
    assert(r.poisson(0) === 0, "poisson(0) is the point mass at 0");
    assert(r.getState().join(",") === s0, "poisson(0) consumes no draws");
    const big = new Random(9).poisson(1e6);
    assert(Number.isInteger(big) && big > 990000 && big < 1010000,
        "poisson(1e6) lands within 10 sigma of its mean (" + big + ")");
    let threw = null;
    try { new Random(1).poisson(2147483648); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "poisson above 2^31-1 refused");
    threw = null;
    try { new Random(1).poisson(-1); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "negative lambda refused");
    threw = null;
    try { new Random(1).poisson(); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "missing lambda refused");
    threw = null;
    try { new Random(1).poisson(NaN); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "NaN lambda refused");
});
__kitS.end();

print("test_random_dist: all tests passed (" + n + " assertions)");
