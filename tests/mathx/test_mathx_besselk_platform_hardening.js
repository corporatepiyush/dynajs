// timeout: 120
// covers: dyna:mathx.besselk
// Split from tests/test_platform_hardening.js: the mathx sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_besselk_platform_hardening");
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

__kitS.section("huge-order besselk answers +Infinity", () => {
    const t0 = cpuNow();
    assert(besselk(1e9, 0.1) === Infinity, "huge-order besselk answers +Infinity");
    assert(besselkScaled(3e9, 0.1) === Infinity, "huge-order besselkScaled answers +Infinity");
    assert(nchoosek(1e18, 5e17) === Infinity, "nchoosek overflows promptly");
    assert(cpuNow() - t0 < 2000, "DoS probes stay fast");
});
__kitS.end();
print("test_mathx_platform_hardening: all tests passed (" + n + " assertions)");
