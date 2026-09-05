/* test_native_memory.js -- the module-native accounting layer (audit E0208-01):
 * memoryUsage().nativeSize / .nativeLimit and setNativeMemoryLimit() on
 * dyna:sys.
 *
 * The engine's counters cannot see libc memory a module holds outside the JS
 * heap, so this instrument gets its own test: the counter must MOVE when
 * module-native memory is allocated, come back when it is released (both via
 * GC finalizers and via close()), and the cap must REFUSE allocation past the
 * limit with an out-of-memory throw -- leaving the engine fully functional
 * afterwards, because a cap that corrupts on refusal is worse than no cap.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y, full build -- imports dyna:sys AND
 * dyna:structures) tests/test_native_memory.js */

import { gc } from "std";
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import { BitSet } from "dyna:structures";
import { Compressor } from "dyna:compress";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function nativeSize() { return memoryUsage().nativeSize; }

try {
    /* ---- shape: the fields exist, are numbers, and start uncapped ---- */
    let u = memoryUsage();
    assert(typeof u.nativeSize === "number", "nativeSize is a number");
    assert(typeof u.nativeLimit === "number", "nativeLimit is a number");
    assert(u.nativeSize >= 0, "nativeSize is non-negative");
    assert(u.nativeLimit === 0, "the cap defaults to OFF (uncapped)");

    /* ---- leak-a: live payloads move the counter up, byte-plausibly ---- */
    const base = nativeSize();
    /* 64 sets x 2^20 bits = 64 x 128 KiB of words = 8 MiB, all retained.
     * The allocating loop is an IIFE on purpose: a bare top-level loop leaves
     * its last `b` local pinned by the script frame, and the free section
     * below would then be one object short of returning to baseline. */
    const N = 64, BITS = 1 << 20;
    const held = (function () {
        const arr = [];
        for (let i = 0; i < N; i++) {
            let b = new BitSet(BITS);
            b.set(i);                /* touch it: not just allocated, used */
            arr.push(b);
        }
        return arr;
    })();
    let grown = nativeSize();
    assert(grown - base >= N * (BITS >> 3),
           "nativeSize tracks " + N + " live BitSets (grew "
           + (grown - base) + " bytes, expected >= " + N * (BITS >> 3) + ")");
    /* and it is not a wild over-count: each set costs words + its struct */
    assert(grown - base <= N * ((BITS >> 3) + 64),
           "nativeSize is not inflated (grew " + (grown - base) + ")");

    /* ---- the cap never frees: held payloads keep working under a cap ---- */
    setNativeMemoryLimit(1 << 20);   /* 1 MiB, far below what `held` occupies */
    assert(held[0].get(0) === true && held[1].get(0) === false,
           "live payloads unaffected by a lower cap");
    assert(held[3].get(3) === true, "live payloads still answer reads");

    /* ---- cap: allocation past the limit is refused, LOUDLY ---- */
    /* Hold the ledger at exactly its current size, so every new allocation
     * is past the cap. The cap is ABSOLUTE, like the engine's
     * JS_SetMemoryLimit: once live bytes are over the limit, allocation
     * stays refused until something is freed -- a tiny allocation is as
     * past-the-limit as a huge one. */
    setNativeMemoryLimit(nativeSize());
    let threw = null;
    try { new BitSet(1 << 24); }     /* 2 MiB of words: far past the cap */
    catch (e) { threw = e; }
    assert(threw !== null, "over-cap allocation throws");
    assert(threw instanceof Error, "the throw is an Error ("
           + (threw && threw.constructor && threw.constructor.name) + ")");
    let threwSmall = null;
    try { new BitSet(1024); }        /* tiny, but still past the cap */
    catch (e) { threwSmall = e; }
    assert(threwSmall !== null,
           "any allocation is refused while over the cap");

    /* ---- ...and nothing is corrupt: live objects answer, JS carries on ---- */
    assert(held[1].get(1) === true, "state intact under the cap");
    let fib = 1, acc = 1;
    for (let i = 0; i < 50; i++) { const t = fib + acc; fib = acc; acc = t; }
    assert(acc > 0, "pure JS still runs while over the cap");

    setNativeMemoryLimit(0);
    let small = new BitSet(1024);    /* with the cap off this must work */
    small.set(7);
    assert(small.get(7) === true && small.get(8) === false,
           "allocation works again once uncapped");

    /* ---- fill to the edge: exactly-at-the-limit allocation succeeds ---- */
    /* Scoped like test_sys_memory's allocation blocks, so the object is
     * unreferenced (not merely nulled) when the collection happens. */
    let edgeOk = false, refused = false;
    (function () {
        const live = nativeSize();
        const cost = (BITS >> 3) + 16;     /* words + the BitSet struct */
        setNativeMemoryLimit(live + cost); /* room for exactly one more set */
        const edge = new BitSet(BITS);     /* ends AT the cap: must succeed */
        edgeOk = edge.get(0) === false;
        try { new BitSet(BITS); }          /* the ledger is now full */
        catch (e) { refused = true; }
    })();
    assert(edgeOk, "allocation ending AT the cap succeeds");
    assert(refused, "the next allocation past the full cap throws");

    /* ---- uncapping restores service ---- */
    setNativeMemoryLimit(0);
    assert(memoryUsage().nativeLimit === 0, "reset to 0 reads back 0");
    assert((new BitSet(1 << 24)).get(999) === false,
           "2 MiB allocation works again once uncapped");

    /* ---- free (GC): drop the references, collect, the ledger returns ---- */
    held.length = 0;
    small = null;
    /* scrub the machine stack the collector may still scan for stale
     * references to the freed-scope objects above, then collect */
    (function scrub() {
        const junk = [];
        for (let i = 0; i < 200; i++) junk.push(new BitSet(64));
        return junk.length;
    })();
    gc(); gc();                      /* the engine's own two-pass collection */
    let settled = nativeSize();
    assert(settled <= base + (1 << 16),
           "nativeSize returns after GC (baseline " + base + ", now "
           + settled + ")");

    /* ---- free (close): the resource box ledger balances exactly ---- */
    /* Every dyna:* resource carries a DynResource box tracked on the module
     * ledger (24 bytes here); Compressor is a close()-able resource. The
     * boxes leave the ledger when the object is finalized, so the whole
     * construct/close/collect cycle lives inside one scope. */
    const R = 20000;
    const before = nativeSize();
    let boxed = 0;
    (function () {
        const rs = [];
        for (let i = 0; i < R; i++) rs.push(new Compressor());
        boxed = nativeSize();
        for (let i = 0; i < R; i++) rs[i].close();
    })();
    assert(boxed - before >= R * 24,
           "resource boxes count on the ledger (grew " + (boxed - before)
           + ", expected >= " + R * 24 + ")");
    gc(); gc();                      /* collect the closed resources */
    let closed = nativeSize();
    assert(closed <= before + (1 << 14),
           "close() releases the boxes: ledger returns (before " + before
           + ", now " + closed + ")");

    /* ---- argument hygiene ---- */
    let threwNeg = false;
    try { setNativeMemoryLimit(-1); } catch (e) { threwNeg = true; }
    assert(threwNeg, "negative limit throws RangeError");
    assert(memoryUsage().nativeLimit === 0, "failed set leaves the cap alone");
} finally {
    /* The cap is process-wide: leave the process as we found it. */
    setNativeMemoryLimit(0);
}

print("test_native_memory: " + n + " assertions passed");
