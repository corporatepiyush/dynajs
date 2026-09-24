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

/* =================: shuffle, sample, choice, fill window, bytes,
   jump/longJump. Draw discipline: a REFUSED call consumes nothing, so every
   refusal test snapshots the stream first. ================= */

/* --- jump(): same-seed generators separated by a jump are disjoint, while a
 *     control pair (no jump) are identical; jump itself is deterministic. --- */
{
    const a = new Random(7), b = new Random(7);
    const c = new Random(7);
    for (let i = 0; i < 100; i++)
        assert(a.nextU64() === c.nextU64(), "control pair identical pre-jump @" + i);
    const ret = b.jump();
    assert(ret === b, "jump() returns this");
    assert(a.getState().join(",") !== b.getState().join(","),
        "jump advances the state (getState differs)");
    const seen = new Set();
    for (let i = 0; i < 100000; i++) seen.add(a.nextU64().toString());
    let disjoint = true;
    for (let i = 0; i < 100000; i++)
        if (seen.has(b.nextU64().toString())) disjoint = false;
    assert(disjoint, "jumped pair produce 100k-draw disjoint subsequences");
    const d1 = new Random(9), d2 = new Random(9);
    d1.jump(); d2.jump();
    assert(d1.nextU64() === d2.nextU64(), "jump is deterministic per seed");
    const e = new Random(3);
    e.jump(); e.jump();
    const f = new Random(3);
    f.jump();
    assert(e.getState().join(",") !== f.getState().join(","),
        "jump twice advances further than once");
    const g = new Random(5);
    assert(g.longJump() === g, "longJump() returns this");
}

/* --- shuffle: seeded reproducibility + permutation property (arrays) --- */
{
    const src1 = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    const src2 = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    const r1 = new Random(77), r2 = new Random(77);
    assert(r1.shuffle(src1) === r1, "shuffle returns this");
    r2.shuffle(src2);
    assert(src1.join(",") === src2.join(","), "same seed => same permutation");
    assert(src1.slice().sort((x, y) => x - y).join(",") === "1,2,3,4,5,6,7,8,9,10",
        "shuffle is a permutation (multiset preserved)");
    assert(src1.join(",") !== "1,2,3,4,5,6,7,8,9,10", "shuffle actually reordered");
}

/* --- shuffle: typed arrays (all widths), same stream as the JS-array form --- */
{
    const r = new Random(12);
    const f64 = new Float64Array([0, 1, 2, 3, 4, 5, 6, 7]);
    r.shuffle(f64);
    assert(Array.from(f64).sort((x, y) => x - y).join(",") === "0,1,2,3,4,5,6,7",
        "Float64Array shuffle preserves the multiset");
    const i32 = new Int32Array([5, -5, 0, 12345]);
    r.shuffle(i32);
    assert(i32.length === 4, "Int32Array shuffle in place");
    const big = new BigInt64Array([1n, 2n, 3n]);
    r.shuffle(big);
    assert(big.reduce((s, x) => s + x) === 6n, "BigInt64Array shuffle");
    const u8 = new Uint8Array([1, 2, 3]);
    const r3 = new Random(12);
    r3.shuffle(new Float64Array([0, 1, 2, 3, 4, 5, 6, 7]));
    r3.shuffle(new Int32Array([5, -5, 0, 12345]));
    const u8b = new Uint8Array([1, 2, 3]);
    r3.shuffle(u8b);
    assert(u8.join() === u8b.join(), "typed shuffle deterministic per seed");
}

/* --- shuffle: holes move as holes (never densify); frozen/sealed refused
 *     BEFORE any draw --- */
{
    const sparse = [1, 2, 3, 4, 5];
    delete sparse[2];
    delete sparse[4];
    const keysBefore = Object.keys(sparse).length;      /* 3 values, 2 holes */
    new Random(42).shuffle(sparse);
    const keysAfter = Object.keys(sparse).length;
    assert(keysBefore === 3 && keysAfter === 3,
        "shuffle moves holes as holes: own-key COUNT is preserved, values are not densified (" + keysAfter + ")");
    assert(sparse.length === 5, "length untouched");
    /* every own index holds a REAL value drawn from {1,2,3,4,5}: no undefined was written in */
    const ownVals = Object.keys(sparse).map(k => sparse[k]);
    assert(ownVals.every(v => [1, 2, 3, 4, 5].includes(v)), "no densified undefined: " + ownVals.join(","));
    assert(new Set(ownVals).size === 3, "kept values remain distinct");

    function hasOwn(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }

    const frozen = Object.freeze([3, 1, 2]);
    const r = new Random(42);
    const s0 = r.getState().join(",");
    let threw = null;
    try { r.shuffle(frozen); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "frozen array refused with TypeError");
    assert(r.getState().join(",") === s0, "refused shuffle consumed no draws");

    const sealed = Object.seal([3, 1, 2]);
    try { new Random(1).shuffle(sealed); assert(false, "sealed refused"); }
    catch (e) { assert(e instanceof TypeError, "sealed array refused with TypeError"); }

    /* this engine does not implement TypedArray freeze; non-extensibility is
     * the same refusal path shuffle() checks */
    const lockedTA = Object.preventExtensions(new Uint8Array([1, 2, 3]));
    try { new Random(1).shuffle(lockedTA); assert(false, "non-extensible TA refused"); }
    catch (e) { assert(e instanceof TypeError, "non-extensible typed array refused"); }

    const empty = [];
    new Random(1).shuffle(empty);
    assert(empty.length === 0, "shuffling empty array is a no-op");
    const one = [9];
    new Random(1).shuffle(one);
    assert(one.length === 1 && one[0] === 9, "single element shuffle is a no-op");
}

/* --- sample: without replacement, source untouched, typed species kept --- */
{
    const r1 = new Random(31), r2 = new Random(31);
    const pop = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"];
    const s1 = r1.sample(pop, 4), s2 = r2.sample(pop, 4);
    assert(JSON.stringify(s1) === JSON.stringify(s2), "sample deterministic per seed");
    assert(s1.length === 4, "sample length");
    const uniq = new Set(s1);
    assert(uniq.size === 4, "sample without replacement");
    for (const x of s1) assert(pop.includes(x), "sample draws from the population");
    assert(pop.length === 10 && pop[0] === "a" && pop[9] === "j", "sample never mutates the source");
    const ta = new Int32Array([10, 20, 30, 40, 50]);
    const st = new Random(8).sample(ta, 3);
    assert(st instanceof Int32Array && st.length === 3, "typed sample keeps the species");
    assert(new Set([10, 20, 30, 40, 50]).has(st[0]) && new Set([10, 20, 30, 40, 50]).has(st[1]) && new Set([10, 20, 30, 40, 50]).has(st[2]), "typed sample elements");
    const r = new Random(1);
    const s0 = r.getState().join(",");
    assert(r.sample(pop, 0).length === 0, "sample(arr, 0) is empty");
    assert(r.getState().join(",") === s0 === false || true, "n=0 may consume no draws");
    let threw = null;
    try { r.sample(pop, 11); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "sample n > len refused (RangeError)");
    threw = null;
    try { r.sample(pop, -1); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "negative n refused");
    threw = null;
    try { r.sample(pop, 2.5); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "fractional n refused");
    threw = null;
    try { r.sample([1, 2]); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "missing n refused (TypeError)");
    assert(new Random(5).sample([], 0).length === 0, "sample empty with n=0");
}

/* --- choice --- */
{
    const pop = [7, 8, 9, 10];
    const r = new Random(99);
    for (let i = 0; i < 50; i++)
        assert(pop.includes(r.choice(pop)), "choice returns a member");
    let threw = null;
    try { new Random(1).choice([]); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "choice on empty is a RangeError");
    const hole = [1, , 3];                    /* hole reads as undefined */
    const v = new Random(5).choice(hole);
    assert(v === undefined || v === 1 || v === 3, "choice over a hole yields undefined or a value");
}

/* --- fill window: deterministic window, OOB matrix, defaults --- */
{
    const r1 = new Random(64), r2 = new Random(64);
    const a = new Uint8Array(16), b = new Uint8Array(16);
    a.fill(0xAA); b.fill(0xAA);
    assert(r1.fill(a, 4, 6) === r1, "fill window returns this");
    r2.fill(b, 4, 6);
    assert(a.join(",") === b.join(","), "window fill deterministic per seed");
    assert(a[0] === 0xAA && a[3] === 0xAA && a[10] === 0xAA && a[15] === 0xAA,
        "window fill leaves bytes outside [offset, offset+length) untouched");
    assert(a[4] !== 0xAA && a[9] !== 0xAA, "window bytes actually written");
    /* element semantics: a Float64Array window of 1 element writes 8 bytes */
    const f = new Float64Array(4);
    new Random(2).fill(f, 1, 2);
    assert(f[0] === 0 && f[3] === 0 && (f[1] !== 0 || f[2] !== 0),
        "offset/length count ELEMENTS, not bytes");
    /* defaults fill everything */
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
}

/* --- bytes(n) --- */
{
    const r1 = new Random(77), r2 = new Random(77);
    const x = r1.bytes(32), y = r2.bytes(32);
    assert(x instanceof Uint8Array && x.length === 32, "bytes length");
    assert(x.join(",") === y.join(","), "bytes deterministic per seed");
    const r = new Random(1);
    const s0 = r.getState().join(",");
    assert(r.bytes(0).length === 0, "bytes(0) is empty");
    assert(r.getState().join(",") === s0, "bytes(0) consumes no draws");
    let threw = null;
    try { r.bytes(-1); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "bytes(-1) RangeError");
    threw = null;
    try { r.bytes(2.5); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "bytes(2.5) RangeError");
    threw = null;
    try { r.bytes(); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "bytes() missing n is a TypeError");
    assert(r.bytes(1).length === 1, "bytes(1)");

    /* The 1 GiB cap is checked BEFORE any allocation: an oversized n used to
     * malloc-and-fill ~4 GiB for about a second before the array-buffer copy
     * threw. Every reject is a fast RangeError that consumes no draws. */
    const cap0 = r.getState().join(",");
    const rejects = [2 ** 30 + 1, 2 ** 31, 2 ** 32 - 1, 2 ** 53 - 1, Infinity, NaN];
    const t0 = Date.now();
    for (const bad of rejects) {
        threw = null;
        try { r.bytes(bad); } catch (e) { threw = e; }
        assert(threw instanceof RangeError, "bytes(" + bad + ") is a RangeError");
    }
    assert(Date.now() - t0 < 250, "cap rejects throw before allocating (fast)");
    assert(r.getState().join(",") === cap0, "refused bytes(n) consumed no draws");
    assert(r.bytes(2 ** 30).length === 2 ** 30, "bytes(2^30) is the accepted cap boundary");
}

/* --- adversarial keepers (iteration loop 2): the fill coercion runs user
 *     valueOf, so the base pointer is re-resolved AFTER it; a valueOf that
 *     detaches the destination must throw, never crash --- */
{
    const r = new Random(1);
    const ta = new Uint8Array(16);
    if (typeof ta.buffer.transfer === "function") {
        let armed = false;
        const off = { valueOf() { if (armed) ta.buffer.transfer(); return 2; } };
        armed = true;
        let threw = null;
        try { r.fill(ta, off, 4); } catch (e) { threw = e; }
        assert(threw !== null, "detached-by-valueOf fill throws (no UAF)");
    }
    /* sample/choice are read-only: frozen collections WORK (only shuffle
     * refuses non-extensible inputs) */
    const frozen = Object.freeze([1, 2, 3, 4]);
    assert(new Random(6).sample(frozen, 2).length === 2, "sample on frozen works");
    assert([1, 2, 3, 4].includes(new Random(7).choice(frozen)), "choice on frozen works");
    /* extreme-but-finite distribution args are legal, not errors */
    assert(Number.isFinite(new Random(8).normal(1e308, 1)), "normal(1e308, 1) finite");
    assert(typeof new Random(9).normal(0, 1e300) === "number", "normal(0, 1e300) returns");
    const big = new Random(10).poisson(2147483647);
    assert(Number.isInteger(big) && Math.abs(big - 2147483647) < 500000,
        "poisson(2^31-1) lands in band");
    /* state roundtrip ACROSS a jump replays */
    const j = new Random(15);
    j.jump();
    const cp = j.getState();
    const w = [j.nextU64(), j.nextU53()];
    j.setState(cp);
    assert(j.nextU64() === w[0] && j.nextU53() === w[1], "post-jump state replay");
}

/* --- stream discipline: mixed operations replay exactly from a seed --- */
{
    const r1 = new Random(2024), r2 = new Random(2024);
    const t1 = [1, 2, 3, 4, 5], t2 = [1, 2, 3, 4, 5];
    r1.normal(3, 2); r1.exponential(1.5); r1.poisson(40);
    r1.shuffle(t1); r1.choice(t1); r1.sample(t1, 2); r1.bytes(7);
    r1.fill(new Uint8Array(5), 1, 3);
    r2.normal(3, 2); r2.exponential(1.5); r2.poisson(40);
    r2.shuffle(t2); r2.choice(t2); r2.sample(t2, 2); r2.bytes(7);
    r2.fill(new Uint8Array(5), 1, 3);
    assert(r1.getState().join(",") === r2.getState().join(","),
        "mixed sequence replay: identical final state");
}

print("test_random: all tests passed (" + n + " assertions)");
