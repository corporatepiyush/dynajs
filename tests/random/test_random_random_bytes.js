// flags: --std
// covers: dyna:random.Random.bytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_bytes");
import { Random } from "dyna:random";
import * as sys from "dyna:sys";
import * as std from "std";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("bytes(2^28) deterministic to the last byte", () => {
    const N = 2 ** 28, r = new Random(5);
    // A 256 MiB allocation crosses the GC threshold, so the collector runs inside bytes() and frees
    // whatever garbage the imports left; on glibc (malloc_usable_size slack is 8 bytes, macOS rounds
    // large blocks up to pages) that reclaim made the heap delta land a few KiB under N. Collect first
    // so the delta measures only the result.
    std.gc();
    const peak0 = sys.memoryUsage().peakRss, heap0 = sys.memoryUsage().mallocSize;
    let b = r.bytes(N);
    const u = sys.memoryUsage();
    assert(b.length === N && b[N - 1] === new Random(5).bytes(N)[N - 1], "bytes(2^28) deterministic to the last byte");
    assert(u.peakRss - peak0 < 1.5 * N, "bytes(n) fills its result in place: peak RSS grew " + (u.peakRss - peak0) + " for n = " + N + " (a scratch copy costs 2n)"); // not perf: a peak-RSS bound (memory), not time
    assert(u.mallocSize - heap0 >= N, "the result is charged to the engine heap");
    b = null;
    std.gc();
    sys.setNativeMemoryLimit(64 * 2 ** 20);
    const nat0 = sys.memoryUsage().nativeSize;
    let c = r.bytes(2 ** 27);
    assert(c.length === 2 ** 27, "a native-memory cap does not bound bytes(): the result is engine-heap memory");
    c = null;
    std.gc();
    assert(sys.memoryUsage().nativeSize === nat0, "a freed bytes() result leaves the native ledger where it was");
    sys.setNativeMemoryLimit(0);
});

__kitS.section("bytes length", () => {
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

    const cap0 = r.getState().join(",");
    const rejects = [2 ** 30 + 1, 2 ** 31, 2 ** 32 - 1, 2 ** 53 - 1, Infinity, NaN];
    const t0 = cpuNow();
    for (const bad of rejects) {
        threw = null;
        try { r.bytes(bad); } catch (e) { threw = e; }
        assert(threw instanceof RangeError, "bytes(" + bad + ") is a RangeError");
    }
    assert(cpuNow() - t0 < 250, "cap rejects throw before allocating (fast)");
    assert(r.getState().join(",") === cap0, "refused bytes(n) consumed no draws");
    assert(r.bytes(2 ** 30).length === 2 ** 30, "bytes(2^30) is the accepted cap boundary");
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
