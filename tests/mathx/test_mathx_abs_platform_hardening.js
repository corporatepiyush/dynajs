// timeout: 120
// covers: dyna:mathx.abs
// Split from tests/test_platform_hardening.js: the mathx sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_abs_platform_hardening");
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

__kitS.section("abs accepts the full negative uint64 magnitude", () => {
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
});
__kitS.end();
print("test_mathx_platform_hardening: all tests passed (" + n + " assertions)");
