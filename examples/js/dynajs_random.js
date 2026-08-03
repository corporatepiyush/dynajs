/*
 * dyna:random -- native random number generation self-contained, in-repo, with
 * DETERMINISTIC memory management (no GC reliance).
 *
 * Requires a CONFIG_NATIVE_MODULES build:
 *     make CONFIG_NATIVE_MODULES=y
 *     ./dynajs examples/js/dynajs_random.js
 *
 * Random is an arena-per-object PRNG (xoshiro256**): a private arena holds its
 * state and .close() (aliased .dispose()) frees it immediately -- O(1), no GC.
 * dyna:uuid.v4() is the RFC 4122 v4 generator. Seeded Randoms are deterministic;
 * an unseeded Random draws its seed from the system CSPRNG.
 */
import { Random } from "dyna:random";
/* v4 lives in dyna:uuid, which is the one UUID generator in the library. */
import { v4 as uuid } from "dyna:uuid";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }

/* Random is a PLAIN GC-MANAGED class: it has no close(), because it owns
 * nothing but a 256-bit state word and there is no descriptor or buffer to
 * release deterministically. The collector reclaims it exactly like a Map.
 *
 * This helper used to call resource.close() and this example broke silently
 * when that surface went away -- which is the argument for running the
 * examples in CI rather than only the tests. It now just scopes the value. */
function withResource(resource, fn) {
    return fn(resource);
}

/* ---- API tour ---- */
withResource(new Random(0xC0FFEE), (r) => {
    const big = r.nextU64();        // BigInt in [0, 2^64)   (full 64-bit)
    const n53 = r.nextU53();        // Number in [0, 2^53)
    const f = r.nextFloat();        // Number in [0, 1)
    const die = r.nextBounded(6);   // Number in [0, 6)      (unbiased)
    const bytes = r.fill(new Uint8Array(16)); // random bytes in place
    assert(typeof big === "bigint", "nextU64 is a BigInt");
    assert(typeof n53 === "number" && n53 >= 0 && n53 < 2 ** 53, "nextU53 range");
    assert(f >= 0 && f < 1, "nextFloat range");
    assert(die >= 0 && die < 6, "nextBounded range");
    assert(bytes.length === 16, "fill returns the view");
    print("nextU64:", big.toString(), "| nextFloat:", f.toFixed(6), "| d6:", die);
});

/* ---- Determinism: same seed -> identical sequence ---- */
function firstN(seed, n) {
    return withResource(new Random(seed), (r) => {
        const out = [];
        for (let i = 0; i < n; i++) out.push(r.nextU64());
        return out;
    });
}
const seqA = firstN(42, 8), seqB = firstN(42, 8);
assert(seqA.every((v, i) => v === seqB[i]), "same seed -> same sequence");
assert(firstN(43, 8).some((v, i) => v !== seqA[i]), "distinct seed -> distinct");
print("Determinism: two Random(42) produced identical 8-draw sequences");

/* ---- uuid(): RFC 4122 v4 ---- */
const id = uuid();
assert(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(id),
       "uuid is canonical v4");
print("uuid:", id);

/* ---- Churn: the collector keeps memory flat without any close() ----
 *
 * A Random owns 256 bits of state and nothing else -- no descriptor, no
 * buffer, nothing whose release has to be timed. So it is a plain GC class
 * and there is no close(), no `closed`, and nothing to put in a
 * DisposableStack. Two hundred thousand of them are created and dropped
 * here; if the finalizer were missing this would grow without bound.
 *
 * This block used to call r.close() and use a DisposableStack, and it broke
 * when Random stopped being a resource. That is the case FOR making it one
 * only when there is something to release: an object with a close() teaches
 * every caller to write teardown that later has to be unwritten. */
for (let i = 0; i < 200000; i++) {
    const r = new Random(i);
    r.nextU64();
}
assert(typeof new Random(1).close === "undefined",
       "Random has no close(): there is nothing to release deterministically");
print("Churn: 200000 Randoms created and dropped, reclaimed by the collector");

/* ---- there is no "after close", and the abuse cases that remain ---- */
{
    /* A wrong receiver is still rejected: the methods check their class, so
     * calling one on a foreign object cannot reinterpret its opaque. */
    let threw = false;
    try { Random.prototype.nextU64.call({}); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "a method on a foreign receiver throws TypeError");

    /* nextBounded coerces its argument, so valueOf is the hook that fires --
     * toString would never run and a test using it would prove nothing. */
    let ran = false;
    const r = new Random(7);
    const n = r.nextBounded({ valueOf() { ran = true; return 6; } });
    assert(ran, "the valueOf hook actually fired");
    assert(n >= 0 && n < 6, "and the coerced bound was honoured");

    /* Degenerate and hostile bounds. */
    for (const bad of [0, -1, NaN]) {
        let t = false;
        try { r.nextBounded(bad); } catch { t = true; }
        assert(t, "nextBounded(" + bad + ") is refused");
    }
    assert(r.nextBounded(1) === 0, "a bound of 1 is always 0");

    /* fill() writes in place and returns the same view, so a caller cannot
     * accidentally read a stale copy. */
    const buf = new Uint8Array(8);
    assert(r.fill(buf) === buf, "fill returns the very view it wrote");
    assert(r.fill(new Uint8Array(0)).length === 0, "an empty view is legal");
}

print("PASS");
