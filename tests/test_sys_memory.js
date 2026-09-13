/* test_sys_memory.js -- dyna:sys.memoryUsage(), the measurement the cost gate
 * is built on.
 *
 * A benchmark harness is only as trustworthy as its instrument, so the
 * instrument gets a test: the counters must MOVE in the right direction when
 * memory is allocated, come back when it is released, and report bytes rather
 * than kilobytes (the getrusage unit differs between Linux and the BSDs, and
 * getting it wrong makes a 1024x difference look like a regression).
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_sys_memory.js */

import { memoryUsage } from "dyna:sys";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

const u = memoryUsage();
for (const k of ["mallocCount", "mallocSize", "memoryUsedCount", "memoryUsedSize",
                 "objCount", "objSize", "strCount", "strSize", "propCount",
                 "shapeCount", "arrayCount", "peakRss",
                 "nativeSize", "nativeLimit"]) {
    assert(k in u, "reports " + k);
    assert(typeof u[k] === "number", k + " is a number");
    assert(u[k] >= 0, k + " is non-negative (" + u[k] + ")");
}
assert(u.mallocSize > 0, "some memory is allocated");
assert(u.objCount > 0, "some objects exist");

/* peakRss is BYTES on every platform. A JS runtime that has done anything at
 * all is past 1 MB and nowhere near 1 TB; a kilobyte/byte unit error lands
 * outside that window on one side or the other. */
assert(u.peakRss > (1 << 20), "peakRss looks like bytes, not kilobytes (" + u.peakRss + ")");
/* 2**40, not 1<<40: JS bitwise shifts are 32-bit, so `1 << 40` is 256 --
 * which this assertion caught on its first run. */
assert(u.peakRss < 2 ** 40, "peakRss looks like bytes, not something larger");

/* The module-native ledger (audit E0208-01): the cap ships OFF, and the
 * counter is a plain non-negative byte count. Behavior under load is the
 * dedicated test_native_memory.js's job (it needs dyna:structures, which a
 * slim sys binary does not carry); this only pins the shape. */
assert(u.nativeLimit === 0, "the native cap defaults to OFF");
assert(u.nativeSize >= 0 && u.nativeSize < 2 ** 40,
       "nativeSize is a plausible byte count (" + u.nativeSize + ")");

/* The counters must respond to allocation. 100k objects is far above any
 * sampling noise. */
{
    const before = memoryUsage();
    const keep = [];
    for (let i = 0; i < 100000; i++) keep.push({ a: i, b: i, c: i });
    const during = memoryUsage();
    assert(during.objCount - before.objCount >= 90000,
           "objCount tracks live objects (+" + (during.objCount - before.objCount) + ")");
    assert(during.mallocSize > before.mallocSize,
           "mallocSize grows with live data");
    assert(during.mallocCount > before.mallocCount,
           "mallocCount grows with live allocations");
    /* keep is still reachable here, which is the point: the counters are LIVE
     * memory, not cumulative churn */
    assert(keep.length === 100000, "the array is still alive");
}

/* ...and it must come back down once the objects are unreachable and collected.
 * Without a collection the numbers are meaningless as a leak signal, which is
 * why the cost gate calls gc() on both sides of every sample. */
{
    const before = memoryUsage();
    (function () {
        const tmp = [];
        for (let i = 0; i < 100000; i++) tmp.push({ a: i, b: i, c: i });
        return tmp.length;
    })();
    if (typeof globalThis.gc === "function") globalThis.gc();
    const after = memoryUsage();
    /* not an equality: the runtime legitimately retains shapes and atoms it
     * created. The assertion is that the bulk came back, not that nothing did. */
    assert(after.objCount - before.objCount < 10000,
           "unreachable objects are not counted after a collection (+" +
           (after.objCount - before.objCount) + ")");
}

/* peak RSS is monotone by definition -- it is a high-water mark */
{
    const a = memoryUsage().peakRss;
    const junk = new Array(200000).fill(0).map((_, i) => ({ i }));
    assert(junk.length === 200000, "allocated");
    const b = memoryUsage().peakRss;
    assert(b >= a, "peakRss never decreases");
}

console.log("test_sys_memory.js: " + n + " assertions passed");
