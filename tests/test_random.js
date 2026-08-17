/* test_random.js — dyna:random (in-repo xoshiro256**).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_random.js
 * Prints "test_random: all tests passed" on success; throws on failure. */

import { Random } from "dyna:random";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

/* --- determinism: same seed => same stream (Number and BigInt seed alias) --- */
{
    const a = new Random(42);
    const b = new Random(42n);
    {
        for (let i = 0; i < 100; i++)
            assert(a.nextU64() === b.nextU64(), "42 and 42n produce same stream @" + i);
    }
}

/* --- different seeds diverge --- */
{
    const a = new Random(1), b = new Random(2);
    {
        let same = 0;
        for (let i = 0; i < 50; i++) if (a.nextU64() === b.nextU64()) same++;
        assert(same < 3, "distinct seeds should rarely collide (" + same + ")");
    }
}

/* --- types and ranges --- */
{
    const r = new Random(7);
    {
        assert(typeof r.nextU64() === "bigint", "nextU64 is a BigInt");
        const u53 = r.nextU53();
        assert(typeof u53 === "number" && Number.isInteger(u53), "nextU53 integer");
        assert(u53 >= 0 && u53 < 2 ** 53, "nextU53 in [0,2^53)");
        for (let i = 0; i < 1000; i++) {
            const f = r.nextFloat();
            assert(f >= 0 && f < 1, "nextFloat in [0,1)");
        }
        for (let i = 0; i < 1000; i++) {
            const d = r.nextBounded(6);
            assert(typeof d === "number" && d >= 0 && d < 6, "d6 in [0,6)");
        }
        assert(typeof r.nextBounded(10n) === "bigint", "BigInt bound => BigInt");
    }
}

/* --- bound validation --- */
{
    const r = new Random(1);
    {
        let threw = false;
        try { r.nextBounded(0); } catch { threw = true; }
        assert(threw, "nextBounded(0) throws RangeError");
        threw = false;
        try { r.nextBounded(-5); } catch { threw = true; }
        assert(threw, "nextBounded(-5) throws");
    }
}

/* --- fill() writes bytes into a JS-owned buffer, deterministically --- */
{
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
}

/* --- distribution smoke: nextBounded(2) is roughly balanced --- */
{
    const r = new Random(123);
    {
        let ones = 0;
        for (let i = 0; i < 10000; i++) ones += r.nextBounded(2);
        assert(ones > 4500 && ones < 5500, "coin flips ~balanced (" + ones + "/10000)");
    }
}

/* --- Random is a PLAIN GC object: no close()/closed surface at all --- */
{
    const r = new Random(1);
    assert(r.close === undefined, "no close(): the whole state is one integer");
    assert(r.closed === undefined, "no closed getter");
    assert(r[Symbol.dispose] === undefined, "not a disposable");
    /* it stays usable forever; the GC reclaims it when unreachable, like a Map */
    for (let i = 0; i < 100; i++) r.nextU64();
    assert(typeof r.nextU64() === "bigint", "still usable after arbitrary use");
}

/* --- a hostile valueOf can no longer free the generator, because nothing frees
 * it early: the reentrancy hazard the close() surface created is gone with it.
 * The coercion still has to happen before the state is touched, which this pins
 * by checking the draw is a valid one. --- */
{
    const r = new Random(1);
    let calls = 0;
    const v = r.nextBounded({ valueOf() { calls++; return 6; } });
    assert(calls === 1, "the argument was coerced exactly once");
    assert(v >= 0 && v < 6, "and the draw is in range: " + v);
}

/* uuid v4 is dyna:uuid's -- tests/test_uuid.js covers it at 100k draws, which
 * is strictly stronger than the 1k check that used to live here. */

print("test_random: all tests passed (" + n + " assertions)");
