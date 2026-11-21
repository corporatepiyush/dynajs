// tests/lib/ledger.js -- does a native result hand its bytes back to the native-memory ledger?
//
// Once sys.setNativeMemoryLimit is set, module malloc goes through the tracking wrappers and
// sys.memoryUsage().nativeSize counts it. A module that hands such a buffer to the engine as an
// ArrayBuffer must move it off the ledger (dyn_nat_ab_take), because the engine frees it with the
// plain allocator: a buffer passed straight to JS_NewArrayBufferTake stays counted forever, so
// nativeSize climbs by one result per call and a later call fails with a false out-of-memory.
//
// settles(a, produce, bytes): with a limit of `limit`, call produce() enough times that the
// results would exceed the limit twice over if they stayed counted (bytes = the result size that
// would leak per call); assert no call throws and nativeSize is back at its baseline after gc.
import * as sys from "dyna:sys";
import * as std from "std";

export async function settles(a, produce, bytes, limit = 32 * 2 ** 20) {
    const rounds = 2 * Math.ceil(limit / bytes) + 1;
    sys.setNativeMemoryLimit(limit);
    try {
        let r = await produce();
        a.must(r !== undefined, "the warm-up call returned a result");
        r = null;
        std.gc();
        const base = sys.memoryUsage().nativeSize;
        let err = null;
        try {
            for (let i = 0; i < rounds; i++) {
                r = await produce();
                r = null;
                std.gc();
            }
        } catch (e) {
            err = String(e);
        }
        std.gc();
        a.eq(err, null, rounds + " results of " + bytes + " bytes under a " + limit + "-byte native limit never run out of memory");
        a.eq(sys.memoryUsage().nativeSize, base, "nativeSize returns to its baseline once the results are collected");
    } finally {
        sys.setNativeMemoryLimit(0);
    }
}
