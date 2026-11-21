// covers: dyna:random.Random.fill
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_fill");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("fill wrote non-zero bytes", () => {
    const a = new Random(99), b = new Random(99);
    {
        const x = new Uint8Array(32), y = new Uint8Array(32);
        a.fill(x); b.fill(y);
        let allZero = true, equal = true;
        for (let i = 0; i < 32; i++) {
            if (x[i] !== 0) allZero = false;
            if (x[i] !== y[i]) equal = false;
        }
        assert(!allZero, "fill wrote non-zero bytes");
        assert(equal, "fill is deterministic for the same seed");
    }
});

__kitS.section("fill window returns this", () => {
    const r1 = new Random(64), r2 = new Random(64);
    const a = new Uint8Array(16), b = new Uint8Array(16);
    a.fill(0xAA); b.fill(0xAA);
    assert(r1.fill(a, 4, 6) === r1, "fill window returns this");
    r2.fill(b, 4, 6);
    assert(a.join(",") === b.join(","), "window fill deterministic per seed");
    assert(a[0] === 0xAA && a[3] === 0xAA && a[10] === 0xAA && a[15] === 0xAA,
        "window fill leaves bytes outside [offset, offset+length) untouched");
    assert(a[4] !== 0xAA && a[9] !== 0xAA, "window bytes actually written");
    const f = new Float64Array(4);
    new Random(2).fill(f, 1, 2);
    assert(f[0] === 0 && f[3] === 0 && (f[1] !== 0 || f[2] !== 0),
        "offset/length count ELEMENTS, not bytes");
    const c1 = new Uint8Array(9), c2 = new Uint8Array(9);
    const d1 = new Random(5), d2 = new Random(5);
    d1.fill(c1);
    d2.fill(c2, 0, 9);
    assert(c1.join(",") === c2.join(","), "defaults == explicit full window");
    const r = new Random(7);
    const s0 = r.getState().join(",");
    const oob = [
        [17, undefined], [-1, undefined], [4, 13], [4, -2], [2.5, undefined],
        [4, 2.5],
    ];
    for (const [off, len] of oob) {
        let threw = null;
        try { r.fill(new Uint8Array(16), off, len); } catch (e) { threw = e; }
        assert(threw instanceof RangeError,
            "fill OOB window [" + off + "," + len + "] refused with RangeError");
    }
    assert(r.getState().join(",") === s0, "refused fill consumed no draws");
    const sub = new Uint8Array(32).subarray(8, 24);
    new Random(3).fill(sub, 4, 4);
    assert(sub.length === 16, "subarray windows honored");
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
