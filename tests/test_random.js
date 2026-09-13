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

/* --- nextBounded: integral bounds only, full u64 range via BigInt --- */
{
    const r = new Random(1);
    {
        let threw = false;
        try { r.nextBounded(6.5); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "nextBounded(6.5) throws RangeError (no silent truncation)");
        threw = false;
        try { r.nextBounded(0n); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "nextBounded(0n) throws RangeError");
    }
    /* [2^63, 2^64) bounds arrive negative from JS_ToBigInt64's mod-2^64
     * reduction; the bit pattern is the bound the caller wrote. */
    {
        const B = 2n ** 64n - 1n;
        for (let i = 0; i < 100; i++) {
            const v = r.nextBounded(B);
            assert(typeof v === "bigint" && v >= 0n && v < B,
                   "nextBounded(2**64-1) in range @" + i);
        }
    }
    {
        const r1 = new Random(9n);
        for (let i = 0; i < 50; i++)
            assert(r1.nextBounded(1n) === 0n, "nextBounded(1n) is always 0 @" + i);
    }
}

/* --- state checkpoint/resume: getState/setState --- */
{
    const r = new Random(42n);
    const st = r.getState();
    assert(st instanceof Uint8Array && st.length === 32,
           "getState returns Uint8Array(32)");
    const pulled = [];
    for (let i = 0; i < 10; i++) pulled.push(r.nextU64());
    r.setState(st);
    for (let i = 0; i < 10; i++)
        assert(r.nextU64() === pulled[i], "setState replays the stream @" + i);
    /* a checkpoint transfers to a different generator, too */
    const r2 = new Random(7);
    r2.setState(st);
    for (let i = 0; i < 10; i++)
        assert(r2.nextU64() === pulled[i], "state transfers across generators @" + i);
    /* an offset view (subarray) is decoded from ITS bytes, not the buffer start */
    const padded = new Uint8Array(64);
    padded.set(st, 16);
    r.setState(padded.subarray(16, 48));
    assert(r.nextU64() === pulled[0] && r.nextU64() === pulled[1],
           "setState accepts an offset subarray view");
    /* the all-zero state is xoshiro256**'s fixed point: refused, not accepted */
    let threw = false;
    try { r.setState(new Uint8Array(32)); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "setState(all zeros) throws RangeError");
    threw = false;
    try { r.setState(new Uint8Array(31)); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "setState(31 bytes) throws RangeError");
}

/* --- golden regression vectors, read from this engine on darwin/arm64.
 * These are pins against accidental stream changes, NOT published vectors.
 * fill() memcpy's native-order u64 chunks, so its byte stream is
 * little-endian on this host (and would differ on big-endian). --- */
{
    const want = [12966619160104079557n, 9600361134598540522n, 10590380919521690900n,
                  7218738570589545383n, 12860671823995680371n, 2648436617965840162n,
                  1310552918490157286n, 7031611932980406429n];
    const r = new Random(1n);
    for (let i = 0; i < want.length; i++)
        assert(r.nextU64() === want[i], "golden u64 #" + i + " of seed 1n");
    const r2 = new Random(1n);
    const b = new Uint8Array(4);
    r2.fill(b);
    assert(b[0] === 197 && b[1] === 16 && b[2] === 199 && b[3] === 15,
           "golden first 4 fill() bytes of seed 1n");
}

/* uuid v4 is dyna:uuid's -- tests/test_uuid.js covers it at 100k draws, which
 * is strictly stronger than the 1k check that used to live here. */

print("test_random: all tests passed (" + n + " assertions)");
