// timeout: 120
// covers: dyna:mathx.stats
// Split from tests/test_platform_hardening.js: the mathx sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_stats_platform_hardening");
import { stats, cumsum, cumprod, diff, abs, bitLen, popcount, isPrime, nchoosek, besselk, besselkScaled } from "dyna:mathx";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throwsRange(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof RangeError; }
    assert(t, msg);
}
function throwsType(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof TypeError; }
    assert(t, msg);
}

__kitS.section("stats.sum on a 2^32-1 array-like throws RangeError", () => {
    const huge = new Array(4294967295);
    const t0 = cpuNow();
    throwsRange(() => stats.sum(huge), "stats.sum on a 2^32-1 array-like throws RangeError");
    const dt = cpuNow() - t0;
    assert(dt < 2000, "stats.sum refuses before iterating (" + dt + " ms)");
    assert(stats.sum([1, 2, 3]) === 6, "stats.sum still sums small arrays");
    assert(stats.mean(new Float64Array([1, 2, 3])) === 2, "stats.mean typed fast path intact");
    throwsRange(() => cumsum(huge), "cumsum refuses an oversized array-like too");
    throwsRange(() => cumprod(huge), "cumprod refuses an oversized array-like too");
    throwsRange(() => diff(huge), "diff refuses an oversized array-like too");
});
__kitS.end();
print("test_mathx_platform_hardening: all tests passed (" + n + " assertions)");
