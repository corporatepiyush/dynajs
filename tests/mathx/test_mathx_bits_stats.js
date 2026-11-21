// covers: dyna:mathx.bits
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_bits_stats");
import { stats, bits } from "dyna:mathx";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    if (!(actual === expected) && !Object.is(actual, expected))
        throw new Error("assertion failed: " + msg +
            " (got " + actual + ", expected " + expected + ")");
}
function assertNaN(actual, msg) {
    n++;
    if (!Number.isNaN(actual))
        throw new Error("assertion failed: " + msg + " (got " + actual + ", expected NaN)");
}
function assertThrows(fn, name, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        if (name && e.name !== name)
            throw new Error("assertion failed: " + msg + " (threw " +
                e.name + ": " + e.message + ", expected " + name + ")");
        return;
    }
    throw new Error("assertion failed: " + msg + " (threw nothing, expected " + name + ")");
}
function assertSameBits(a, b, msg) {
    n++;
    if (!Object.is(a, b) && !(typeof a === "number" && typeof b === "number" &&
                              Number.isNaN(a) && Number.isNaN(b)))
        throw new Error("assertion failed: " + msg +
            " (Float64Array path " + a + " vs number[] path " + b + ")");
}
function assertCloseRel(actual, expected, rel, msg) {
    n++;
    const scale = Math.max(1, Math.abs(expected));
    if (!(Math.abs(actual - expected) <= rel * scale))
        throw new Error("assertion failed: " + msg +
            " (got " + actual + ", expected ~" + expected + " rel " + rel + ")");
}

function lcg(seed) {
    let s = seed >>> 0;
    return function () {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s / 4294967296;
    };
}

function naiveSum(a) {
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return s;
}
function naiveMean(a) { return naiveSum(a) / a.length; }
function naiveVar(a, pop) {
    const m = naiveMean(a);
    let m2 = 0;
    for (let i = 0; i < a.length; i++) m2 += (a[i] - m) * (a[i] - m);
    return m2 / (pop ? a.length : a.length - 1);
}
function naiveCov(x, y, pop) {
    const mx = naiveMean(x), my = naiveMean(y);
    let c = 0;
    for (let i = 0; i < x.length; i++) c += (x[i] - mx) * (y[i] - my);
    return c / (pop ? x.length : x.length - 1);
}
function naiveCorr(x, y) {
    const mx = naiveMean(x), my = naiveMean(y);
    let c = 0, vx = 0, vy = 0;
    for (let i = 0; i < x.length; i++) {
        const dx = x[i] - mx, dy = y[i] - my;
        c += dx * dy; vx += dx * dx; vy += dy * dy;
    }
    return c / (Math.sqrt(vx) * Math.sqrt(vy));
}
function naiveQuantile(a, q) {
    const b = a.slice().sort(function (x, y) { return x - y; });
    const pos = q * (b.length - 1);
    const lo = Math.floor(pos);
    const frac = pos - lo;
    if (frac === 0 || lo + 1 >= b.length) return b[lo];
    return b[lo] + frac * (b[lo + 1] - b[lo]);
}

const DATA = [3.5, -2.25, 17.125, 0.0, 4.5, -2.25, 9.75, 3.5, -8.125, 6.0, 2.125, 12.375];
const DATA_F64 = new Float64Array(DATA);
const X2 = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];

__kitS.section("rotateRight8 known value", () => {
    assertEq(bits.rotateRight8(0b11000001, 2), 0b01110000, "rotateRight8 known value");
    assertEq(bits.rotateRight16(0x8001, 1), 0xC000, "rotateRight16 known value");
    assertEq(bits.rotateRight32(0x80000000, 1), 0x40000000, "rotateRight32 known value");
    assertEq(bits.rotateRight64(0x8000000000000000n, 1), 0x4000000000000000n,
             "rotateRight64 known value");
    assertEq(bits.rotateRight8(8, -1), 16, "rotateRight8(x, -1) == rotateLeft8(x, 1)");
    assertEq(bits.rotateRight8(0xA5, 8), 0xA5, "rotateRight8 by the width is identity");
    assertEq(bits.rotateRight64(123456789n, 64), 123456789n, "rotateRight64 by 64 is identity");

    const x8 = 0b10010110, x16 = 0b1011000011110001, x32 = 0xDEADBEEF, x64 = 0x0123456789ABCDEFn;
    for (let k = -70; k <= 70; k++) {
        assertEq(bits.rotateRight8(x8, k), bits.rotateLeft8(x8, -k),
                 "rotateRight8 mirrors rotateLeft8 at k=" + k);
        assertEq(bits.rotateRight16(x16, k), bits.rotateLeft16(x16, -k),
                 "rotateRight16 mirrors rotateLeft16 at k=" + k);
        assertEq(bits.rotateRight32(x32, k), bits.rotateLeft32(x32, -k),
                 "rotateRight32 mirrors rotateLeft32 at k=" + k);
        assertEq(bits.rotateRight64(x64, k), bits.rotateLeft64(x64, -k),
                 "rotateRight64 mirrors rotateLeft64 at k=" + k);
    }
    for (let k = 0; k < 8; k++) {
        assertEq(bits.rotateRight8(bits.rotateRight8(x8, k), 8 - k), x8,
                 "rotateRight8 round trip k=" + k);
    }
    for (let k = 0; k < 64; k += 7) {
        assertEq(bits.rotateRight64(bits.rotateRight64(x64, k), 64 - k), x64,
                 "rotateRight64 round trip k=" + k);
    }
});
__kitS.end();

print("test_mathx_stats: all tests passed (" + n + " assertions)");
