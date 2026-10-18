// timeout: 120
// flags: --std
// Split from tests/test_platform_hardening.js: the mathx sections.
import { stats, cumsum, cumprod, diff, abs, bitLen, popcount, isPrime, nchoosek, besselk, besselkScaled } from "dyna:mathx";

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
{
    const huge = new Array(4294967295);
    const t0 = Date.now();
    throwsRange(() => stats.sum(huge), "stats.sum on a 2^32-1 array-like throws RangeError");
    const dt = Date.now() - t0;
    assert(dt < 2000, "stats.sum refuses before iterating (" + dt + " ms)");
    assert(stats.sum([1, 2, 3]) === 6, "stats.sum still sums small arrays");
    assert(stats.mean(new Float64Array([1, 2, 3])) === 2, "stats.mean typed fast path intact");
    throwsRange(() => cumsum(huge), "cumsum refuses an oversized array-like too");
    throwsRange(() => cumprod(huge), "cumprod refuses an oversized array-like too");
    throwsRange(() => diff(huge), "diff refuses an oversized array-like too");
}
{
    const t0 = Date.now();
    assert(besselk(1e9, 0.1) === Infinity, "huge-order besselk answers +Infinity");
    assert(besselkScaled(3e9, 0.1) === Infinity, "huge-order besselkScaled answers +Infinity");
    assert(nchoosek(1e18, 5e17) === Infinity, "nchoosek overflows promptly");
    assert(Date.now() - t0 < 2000, "DoS probes stay fast");
}
{
    const M = (1n << 64n) - 1n;
    assert(abs(-M) === M, "abs accepts the full negative uint64 magnitude");
    assert(bitLen(-M) === 64, "bitLen of -2^64+1 magnitude is 64");
    assert(popcount(-M) === 64, "popcount of -2^64+1 magnitude is 64");
    assert(abs(-5n) === 5n, "abs(-5n) is 5n");
    assert(isPrime(-5n) === false, "isPrime(-5n) is false, not 2^64-5");
    assert(isPrime(7n) === true, "isPrime(7n) still true");
    assert(abs(3n) === 3n && bitLen(8n) === 4 && popcount(7n) === 3, "small BigInt cases intact");
    let over = false;
    try { abs(-(M + 1n)); } catch (e) { over = e instanceof RangeError; }
    assert(over, "an over-2^64 magnitude is still refused");
}
print("test_mathx_platform_hardening: all tests passed (" + n + " assertions)");
