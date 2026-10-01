// flags: --std
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
    let u = memoryUsage();
    assert(typeof u.nativeSize === "number", "nativeSize is a number");
    assert(typeof u.nativeLimit === "number", "nativeLimit is a number");
    assert(u.nativeSize >= 0, "nativeSize is non-negative");
    assert(u.nativeLimit === 0, "the cap defaults to OFF (uncapped)");

    const base = nativeSize();
    const N = 64, BITS = 1 << 20;
    const held = (function () {
        const arr = [];
        for (let i = 0; i < N; i++) {
            let b = new BitSet(BITS);
            b.set(i);
            arr.push(b);
        }
        return arr;
    })();
    let grown = nativeSize();
    assert(grown - base >= N * (BITS >> 3),
           "nativeSize tracks " + N + " live BitSets (grew "
           + (grown - base) + " bytes, expected >= " + N * (BITS >> 3) + ")");
    assert(grown - base <= N * ((BITS >> 3) + 64),
           "nativeSize is not inflated (grew " + (grown - base) + ")");

    setNativeMemoryLimit(1 << 20);
    assert(held[0].get(0) === true && held[1].get(0) === false,
           "live payloads unaffected by a lower cap");
    assert(held[3].get(3) === true, "live payloads still answer reads");

    setNativeMemoryLimit(nativeSize());
    let threw = null;
    try { new BitSet(1 << 24); }
    catch (e) { threw = e; }
    assert(threw !== null, "over-cap allocation throws");
    assert(threw instanceof Error, "the throw is an Error ("
           + (threw && threw.constructor && threw.constructor.name) + ")");
    let threwSmall = null;
    try { new BitSet(1024); }
    catch (e) { threwSmall = e; }
    assert(threwSmall !== null,
           "any allocation is refused while over the cap");

    assert(held[1].get(1) === true, "state intact under the cap");
    let fib = 1, acc = 1;
    for (let i = 0; i < 50; i++) { const t = fib + acc; fib = acc; acc = t; }
    assert(acc > 0, "pure JS still runs while over the cap");

    setNativeMemoryLimit(0);
    let small = new BitSet(1024);
    small.set(7);
    assert(small.get(7) === true && small.get(8) === false,
           "allocation works again once uncapped");

    let edgeOk = false, refused = false;
    (function () {
        const live = nativeSize();
        const cost = (BITS >> 3) + 16;
        setNativeMemoryLimit(live + cost);
        const edge = new BitSet(BITS);
        edgeOk = edge.get(0) === false;
        try { new BitSet(BITS); }
        catch (e) { refused = true; }
    })();
    assert(edgeOk, "allocation ending AT the cap succeeds");
    assert(refused, "the next allocation past the full cap throws");

    setNativeMemoryLimit(0);
    assert(memoryUsage().nativeLimit === 0, "reset to 0 reads back 0");
    assert((new BitSet(1 << 24)).get(999) === false,
           "2 MiB allocation works again once uncapped");

    held.length = 0;
    small = null;
    (function scrub() {
        const junk = [];
        for (let i = 0; i < 200; i++) junk.push(new BitSet(64));
        return junk.length;
    })();
    gc(); gc();
    let settled = nativeSize();
    assert(settled <= base + (1 << 16),
           "nativeSize returns after GC (baseline " + base + ", now "
           + settled + ")");

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
    gc(); gc();
    let closed = nativeSize();
    assert(closed <= before + (1 << 14),
           "close() releases the boxes: ledger returns (before " + before
           + ", now " + closed + ")");

    let threwNeg = false;
    try { setNativeMemoryLimit(-1); } catch (e) { threwNeg = true; }
    assert(threwNeg, "negative limit throws RangeError");
    assert(memoryUsage().nativeLimit === 0, "failed set leaves the cap alone");
} finally {
    setNativeMemoryLimit(0);
}

print("test_native_memory: " + n + " assertions passed");
