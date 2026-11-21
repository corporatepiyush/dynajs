// flags: --std
// covers: contract dyna:compress.* dyna:decimal.* dyna:structures.* dyna:sys.* -- setNativeMemoryLimit accounting across the native classes of several modules
import { sections as __kitSections } from "../lib/kit.js";
import { gc } from "std";
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import { BitSet, BTree } from "dyna:structures";
import { Compressor } from "dyna:compress";
import { Decimal } from "dyna:decimal";
const __kitS = __kitSections("test_native_memory", { chain: true });

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function nativeSize() { return memoryUsage().nativeSize; }

try {
let u, base, held, grown, small, settled, before, closed;
__kitS.section("nativeSize is a number", () => {
    u = memoryUsage();
    assert(typeof u.nativeSize === "number", "nativeSize is a number");
    assert(typeof u.nativeLimit === "number", "nativeLimit is a number");
    assert(u.nativeSize >= 0, "nativeSize is non-negative");
    assert(u.nativeLimit === 0, "the cap defaults to OFF (uncapped)");
});
    const N = 64, BITS = 1 << 20;
__kitS.section("dyna:structures allocates through the dyn-ds library, which the ledger", () => {
    // dyna:structures allocates through the dyn-ds library, which the ledger
    // counts only once a limit has been set (arming is sticky); arm it first.
    setNativeMemoryLimit(2 ** 40);

    base = nativeSize();
    held = (function () {
        const arr = [];
        for (let i = 0; i < N; i++) {
            let b = new BitSet(BITS);
            b.set(i);
            arr.push(b);
        }
        return arr;
    })();
    grown = nativeSize();
    assert(grown - base >= N * (BITS >> 3),
           "nativeSize tracks " + N + " live BitSets (grew "
           + (grown - base) + " bytes, expected >= " + N * (BITS >> 3) + ")");
});
__kitS.section("the ledger counts usable size: glibc serves a 128 KiB block from a", () => {
    // the ledger counts usable size: glibc serves a 128 KiB block from a
    // page-rounded mmap chunk, so allow one page of rounding per block
    assert(grown - base <= N * ((BITS >> 3) + 4096 + 64),
           "nativeSize is not inflated (grew " + (grown - base) + ")");
});

__kitS.section("live payloads unaffected by a lower cap", () => {
    setNativeMemoryLimit(1 << 20);
    assert(held[0].get(0) === true && held[1].get(0) === false,
           "live payloads unaffected by a lower cap");
    assert(held[3].get(3) === true, "live payloads still answer reads");
});
    let threw = null;
    let threwSmall = null;

__kitS.section("over-cap allocation throws", () => {
    setNativeMemoryLimit(nativeSize());
    try { new BitSet(1 << 24); }
    catch (e) { threw = e; }
    assert(threw !== null, "over-cap allocation throws");
    assert(threw instanceof Error, "the throw is an Error ("
           + (threw && threw.constructor && threw.constructor.name) + ")");
});
    let fib = 1, acc = 1;
__kitS.section("any allocation is refused while over the cap", () => {
    try { new BitSet(1024); }
    catch (e) { threwSmall = e; }
    assert(threwSmall !== null,
           "any allocation is refused while over the cap");

    assert(held[1].get(1) === true, "state intact under the cap");
});
__kitS.section("pure JS still runs while over the cap", () => {
    for (let i = 0; i < 50; i++) { const t = fib + acc; fib = acc; acc = t; }
    assert(acc > 0, "pure JS still runs while over the cap");
});

    let edgeOk = false, refused = false;

__kitS.section("allocation works again once uncapped", () => {
    setNativeMemoryLimit(0);
    small = new BitSet(1024);
    small.set(7);
    assert(small.get(7) === true && small.get(8) === false,
           "allocation works again once uncapped");
});
__kitS.section("allocation ending AT the cap succeeds", () => {
    (function () {
        const live = nativeSize();
        const cost = (BITS >> 3) + 64;
        setNativeMemoryLimit(live + cost);
        const edge = new BitSet(BITS);
        edgeOk = edge.get(0) === false;
        try { new BitSet(BITS); }
        catch (e) { refused = true; }
    })();
    assert(edgeOk, "allocation ending AT the cap succeeds");
});
__kitS.section("the next allocation past the full cap throws", () => {
    assert(refused, "the next allocation past the full cap throws");
});

__kitS.section("reset to 0 reads back 0", () => {
    setNativeMemoryLimit(0);
    assert(memoryUsage().nativeLimit === 0, "reset to 0 reads back 0");
    assert((new BitSet(1 << 24)).get(999) === false,
           "2 MiB allocation works again once uncapped");
});

    const R = 20000;

__kitS.section("nativeSize returns after GC", () => {
    held.length = 0;
    small = null;
    (function scrub() {
        const junk = [];
        for (let i = 0; i < 200; i++) junk.push(new BitSet(64));
        return junk.length;
    })();
    gc(); gc();
    settled = nativeSize();
    assert(settled <= base + (1 << 16),
           "nativeSize returns after GC (baseline " + base + ", now "
           + settled + ")");
});
    let boxed = 0;
__kitS.section("resource boxes count on the ledger", () => {
    before = nativeSize();
    (function () {
        const rs = [];
        for (let i = 0; i < R; i++) rs.push(new Compressor());
        boxed = nativeSize();
        for (let i = 0; i < R; i++) rs[i].close();
    })();
    assert(boxed - before >= R * 24,
           "resource boxes count on the ledger (grew " + (boxed - before)
           + ", expected >= " + R * 24 + ")");
});
__kitS.section("close() releases the boxes: ledger returns", () => {
    gc(); gc();
    closed = nativeSize();
    assert(closed <= before + (1 << 14),
           "close() releases the boxes: ledger returns (before " + before
           + ", now " + closed + ")");
});

    {
let dbase, dgrown;
        let decs = [];
__kitS.section("Decimal digit buffers count on the ledger", () => {
        dbase = nativeSize();
        for (let i = 0; i < 100; i++)
            decs.push(new Decimal("123456789".repeat(500)));
        dgrown = nativeSize();
        assert(dgrown - dbase >= 100 * 4000,
               "Decimal digit buffers count on the ledger (grew "
               + (dgrown - dbase) + ")");
});
__kitS.section("Decimal payloads return after GC", () => {
        decs = null;
        (function decScrub() {
            const junk = [];
            for (let i = 0; i < 20; i++) junk.push(new Decimal("9".repeat(1000)));
            return junk.length;
        })();
        gc(); gc();
        assert(nativeSize() <= dbase + (1 << 16),
               "Decimal payloads return after GC (baseline " + dbase + ", now "
               + nativeSize() + ")");
});
    }

    {
let base, t, grown;
__kitS.section("with a limit armed, a 100000-key BTree (core library) is counted as native memory", () => {
        setNativeMemoryLimit(1 << 30);
        gc();
        base = nativeSize();
        t = new BTree();
        for (let i = 0; i < 100000; i++) t.set(i, i);
        grown = nativeSize() - base;
        assert(grown > (1 << 20), "with a limit armed, a 100000-key BTree (core library) is counted as native memory (" + grown + " bytes)");
});
__kitS.section("and returned after GC", () => {
        t = null;
        gc(); gc();
        assert(nativeSize() <= base + (1 << 16), "and returned after GC (now " + (nativeSize() - base) + " over baseline)");
        setNativeMemoryLimit(0);
});
    }

    let threwNeg = false;
__kitS.section("negative limit throws RangeError", () => {
    try { setNativeMemoryLimit(-1); } catch (e) { threwNeg = true; }
    assert(threwNeg, "negative limit throws RangeError");
    assert(memoryUsage().nativeLimit === 0, "failed set leaves the cap alone");
});
} finally { if (!__kitS.listing) {
    setNativeMemoryLimit(0);
} }
__kitS.end();
if (!__kitS.listing) {

print("test_native_memory: " + n + " assertions passed");
}
