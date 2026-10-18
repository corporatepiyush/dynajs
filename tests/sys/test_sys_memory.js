import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";

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

assert(u.peakRss > (1 << 20), "peakRss looks like bytes, not kilobytes (" + u.peakRss + ")");
assert(u.peakRss < 2 ** 40, "peakRss looks like bytes, not something larger");

assert(u.nativeLimit === 0, "the native cap defaults to OFF");
assert(u.nativeSize >= 0 && u.nativeSize < 2 ** 40,
       "nativeSize is a plausible byte count (" + u.nativeSize + ")");

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
    assert(keep.length === 100000, "the array is still alive");
}

{
    const before = memoryUsage();
    (function () {
        const tmp = [];
        for (let i = 0; i < 100000; i++) tmp.push({ a: i, b: i, c: i });
        return tmp.length;
    })();
    if (typeof globalThis.gc === "function") globalThis.gc();
    const after = memoryUsage();
    assert(after.objCount - before.objCount < 10000,
           "unreachable objects are not counted after a collection (+" +
           (after.objCount - before.objCount) + ")");
}

{
    const a = memoryUsage().peakRss;
    const junk = new Array(200000).fill(0).map((_, i) => ({ i }));
    assert(junk.length === 200000, "allocated");
    const b = memoryUsage().peakRss;
    assert(b >= a, "peakRss never decreases");
}

{
    let threw = (f) => { try { f(); return true; } catch (e) { return e instanceof TypeError; } };
    assert(threw(() => memoryUsage), "sanity: a TypeError is detectable");
    for (const bad of [NaN, Infinity, -Infinity, "1024", null, true, {}, 1024n]) {
        assert(threw(() => setNativeMemoryLimit(bad)),
               "a non-finite/non-number limit (" + String(bad) + ") is refused");
    }
    assert(memoryUsage().nativeLimit === 0,
           "the refusals left the uncapped default intact");
    setNativeMemoryLimit(4096);
    assert(memoryUsage().nativeLimit === 4096, "an integer limit is installed");
    setNativeMemoryLimit(4096.9);
    assert(memoryUsage().nativeLimit === 4096,
           "a fractional limit truncates to its integer part");
    let ranged = false;
    try { setNativeMemoryLimit(-1); } catch (e) {
        ranged = e instanceof RangeError;
    }
    assert(ranged, "a negative limit is a RangeError, not a silent 0");
    setNativeMemoryLimit(0);
    assert(memoryUsage().nativeLimit === 0, "0 restores uncapped");
}

console.log("test_sys_memory.js: " + n + " assertions passed");
