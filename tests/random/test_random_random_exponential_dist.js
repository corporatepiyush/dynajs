// covers: dyna:random.Random.exponential
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_exponential_dist");
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

__kitS.section("exponential >= 0", () => {
for (const lambda of [0.25, 1, 7.5]) {
    const r = new Random(1000 + lambda * 10);
    let sum = 0, sum2 = 0, min = Infinity;
    for (let i = 0; i < N; i++) {
        const x = r.exponential(lambda);
        assert(Number.isFinite(x) && x >= 0, "exponential >= 0");
        if (x < min) min = x;
        sum += x;
        sum2 += x * x;
    }
    const mean = sum / N, variance = sum2 / N - mean * mean;
    closeTo(mean, 1 / lambda, 5 / lambda / Math.sqrt(N), "exponential mean");
    closeTo(variance, 1 / (lambda * lambda),
            5 / (lambda * lambda) * Math.sqrt(2 / N) + 1e-12, "exponential variance");
}
});
__kitS.end();

print("test_random_dist: all tests passed (" + n + " assertions)");
