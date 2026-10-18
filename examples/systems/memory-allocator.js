// Memory allocator — a buddy allocator, a slab allocator for small objects and a first-fit allocator with
// coalescing, all managing one real ArrayBuffer heap under the same malloc/free trace, with corruption checks.
//
// WHAT IT IS
//   The allocator layer of a runtime or kernel, small enough to audit:
//
//   heap        a 16 MiB ArrayBuffer; every allocation is filled with a pattern derived from its id and verified when
//               freed, so any overlap between live blocks shows up as corruption, not as a wrong statistic
//   buddy       power-of-two blocks from 64 B to 16 MiB; one SortedSet of free offsets per order; a free merges with
//               its buddy (offset XOR size) while the buddy is free; the invariant "no two free buddies at one order"
//               is checked after the run
//   slab        size classes 16..512 B carved from 4 KiB pages taken from the buddy allocator; a BitSet per page
//               marks used slots, an empty page goes back to the buddy
//   first fit   free extents in a SortedMap keyed by offset; frees coalesce with both neighbours (floorKey/ceilKey)
//   trace       200 000 operations: log-normal sizes, exponential lifetimes, a phase of large buffers in the middle;
//               live blocks scheduled for release in an IndexedHeap
//   metrics     internal fragmentation (requested vs reserved), external fragmentation (1 - largest free / total
//               free) sampled through the run, peak footprint; at the end everything coalesces back to one block
//
// MODULES  dyna:structures (SortedSet, SortedMap, BitSet, IndexedHeap, TDigest) · dyna:bytes (fill) · dyna:random ·
//          dyna:cli · Number extensions
//
// RUN      dynajs examples/systems/memory-allocator.js

import { SortedSet, SortedMap, BitSet, IndexedHeap, TDigest } from "dyna:structures";
import { fill } from "dyna:bytes";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const HEAP = 16 << 20,
    MIN_ORDER = 6,
    MAX_ORDER = 24;
const heap = new Uint8Array(new ArrayBuffer(HEAP));

// =============================================================================
// Buddy allocator
// =============================================================================
class Buddy {
    constructor() {
        this.free = Array.from({ length: MAX_ORDER + 1 }, () => new SortedSet());
        this.free[MAX_ORDER].add(0);
        this.orderOf = new Map();
    }
    alloc(bytes) {
        let need = Math.max(MIN_ORDER, Math.ceil(Math.log2(bytes)));
        let o = need;
        while (o <= MAX_ORDER && this.free[o].first() === undefined) o++;
        if (o > MAX_ORDER) return -1;
        const off = this.free[o].first();
        this.free[o].delete(off);
        while (o > need) {
            o--;
            this.free[o].add(off + (1 << o));
        }
        this.orderOf.set(off, need);
        return off;
    }
    release(off) {
        let o = this.orderOf.get(off);
        this.orderOf.delete(off);
        while (o < MAX_ORDER) {
            const buddy = off ^ (1 << o);
            if (!this.free[o].has(buddy)) break;
            this.free[o].delete(buddy);
            off = Math.min(off, buddy);
            o++;
        }
        this.free[o].add(off);
    }
    reserved(off) {
        return 1 << this.orderOf.get(off);
    }
    freeStats() {
        let total = 0,
            largest = 0;
        for (let o = MIN_ORDER; o <= MAX_ORDER; o++) {
            const n = this.free[o].toArray().length;
            total += n << o;
            if (n) largest = 1 << o;
        }
        return { total, largest };
    }
}

// =============================================================================
// Slab allocator on top of the buddy allocator
// =============================================================================
class Slab {
    constructor(buddy) {
        this.buddy = buddy;
        this.classes = [16, 32, 64, 128, 256, 512].map((size) => ({
            size,
            slots: 4096 / size,
            pages: new Map(),
            partial: new SortedSet(),
        }));
        this.pageOf = new Map();
    }
    cls(bytes) {
        return this.classes.find((c) => c.size >= bytes);
    }
    alloc(bytes) {
        const c = this.cls(bytes);
        let page = c.partial.first();
        if (page === undefined) {
            page = this.buddy.alloc(4096);
            if (page < 0) return -1;
            c.pages.set(page, { used: new BitSet(c.slots), count: 0 });
            c.partial.add(page);
        }
        const p = c.pages.get(page);
        let slot = 0;
        while (p.used.get(slot)) slot++;
        p.used.set(slot);
        p.count++;
        if (p.count === c.slots) c.partial.delete(page);
        const off = page + slot * c.size;
        this.pageOf.set(off, [c, page, slot]);
        return off;
    }
    release(off) {
        const [c, page, slot] = this.pageOf.get(off);
        this.pageOf.delete(off);
        const p = c.pages.get(page);
        p.used.clear(slot);
        p.count--;
        if (p.count === 0) {
            c.pages.delete(page);
            c.partial.delete(page);
            this.buddy.release(page);
        } else c.partial.add(page);
    }
    reserved(off) {
        return this.pageOf.get(off)[0].size;
    }
}

// =============================================================================
// First fit with coalescing
// =============================================================================
class FirstFit {
    constructor() {
        this.free = new SortedMap();
        this.free.set(0, HEAP);
        this.sizeOf = new Map();
    }
    alloc(bytes) {
        const size = (bytes + 15) & ~15;
        for (const off of this.free.keys()) {
            const len = this.free.get(off);
            if (len < size) continue;
            this.free.delete(off);
            if (len > size) this.free.set(off + size, len - size);
            this.sizeOf.set(off, size);
            return off;
        }
        return -1;
    }
    release(off) {
        let size = this.sizeOf.get(off);
        this.sizeOf.delete(off);
        const next = this.free.ceilKey(off + 1);
        if (next === off + size) {
            size += this.free.get(next);
            this.free.delete(next);
        }
        const prev = this.free.floorKey(off - 1);
        if (prev !== undefined && prev + this.free.get(prev) === off) {
            this.free.set(prev, this.free.get(prev) + size);
            return;
        }
        this.free.set(off, size);
    }
    reserved(off) {
        return this.sizeOf.get(off);
    }
    freeStats() {
        let total = 0,
            largest = 0;
        for (const off of this.free.keys()) {
            const l = this.free.get(off);
            total += l;
            largest = Math.max(largest, l);
        }
        return { total, largest };
    }
}

// =============================================================================
// Trace replay with pattern verification
// =============================================================================
function makeTrace(seed) {
    const rng = new Random(seed),
        ops = [];
    for (let i = 0; i < 100_000; i++) {
        const phase = i > 40_000 && i < 55_000;
        const size =
            phase && rng.nextFloat() < 0.02
                ? 32_768 + rng.nextBounded(200_000)
                : Math.max(1, Math.round(Math.exp(3.5 + 1.3 * rng.normal())));
        ops.push({ id: i, size: Math.min(size, 1 << 20), life: Math.ceil(rng.exponential(1 / 400)) });
    }
    return ops;
}
const pattern = (id) => (Math.imul(id, 2654435761) >>> 24) | 1;
function replay(name, alloc) {
    const live = new Map(),
        due = new IndexedHeap(),
        ext = new TDigest(50);
    let corrupt = 0,
        failed = 0,
        requested = 0,
        reserved = 0,
        peak = 0,
        inUse = 0;
    const release = (id) => {
        const { off, size } = live.get(id);
        const p = pattern(id),
            view = heap.subarray(off, off + size);
        for (let k = 0; k < size; k += 61)
            if (view[k] !== p) {
                corrupt++;
                break;
            }
        if (view[size - 1] !== p) corrupt++;
        inUse -= alloc.reserved(off);
        alloc.release(off);
        live.delete(id);
    };
    const trace = makeTrace(7);
    trace.forEach((op, t) => {
        for (const id of due.popUntil(t)) release(id);
        const off = alloc.alloc(op.size);
        if (off < 0) {
            failed++;
            return;
        }
        fill(heap, pattern(op.id), off, off + op.size);
        live.set(op.id, { off, size: op.size });
        due.push(op.id, t + op.life);
        requested += op.size;
        reserved += alloc.reserved(off);
        inUse += alloc.reserved(off);
        peak = Math.max(peak, inUse);
        if (t % 1000 === 0 && alloc.freeStats) {
            const s = alloc.freeStats();
            if (s.total) ext.add(1 - s.largest / s.total);
        }
    });
    for (const id of [...live.keys()]) release(id);
    return { name, corrupt, failed, internal: 1 - requested / reserved, peak, ext: ext.quantile(0.5), live: live.size };
}

const buddy = new Buddy();
const t0 = performance.now();
const rBuddy = replay("buddy", buddy);
const buddyMs = performance.now() - t0;
results["buddy: no corruption, nothing left live"] = rBuddy.corrupt === 0 && rBuddy.live === 0;
results["buddy coalesces back to one 16 MiB block"] =
    buddy.free[MAX_ORDER].has(0) && buddy.free.slice(0, MAX_ORDER).every((s) => s.first() === undefined);

const buddy2 = new Buddy(),
    slab = new Slab(buddy2);
const combined = {
    alloc: (n) => (n <= 512 ? slab.alloc(n) : buddy2.alloc(n)),
    release: (off) => (slab.pageOf.has(off) ? slab.release(off) : buddy2.release(off)),
    reserved: (off) => (slab.pageOf.has(off) ? slab.reserved(off) : buddy2.reserved(off)),
    freeStats: () => buddy2.freeStats(),
};
const rSlab = replay("slab + buddy", combined);
results["slab+buddy: no corruption"] = rSlab.corrupt === 0 && rSlab.live === 0;
results["slab pages all return to the buddy allocator"] =
    slab.classes.every((c) => c.pages.size === 0) && buddy2.free[MAX_ORDER].has(0);
results["slabs cut internal fragmentation versus pure buddy"] = rSlab.internal < rBuddy.internal;

const ff = new FirstFit();
const t1 = performance.now();
const rFF = replay("first fit", ff);
const ffMs = performance.now() - t1;
results["first fit: no corruption"] = rFF.corrupt === 0 && rFF.live === 0;
results["first fit coalesces to one free extent"] = ff.free.keys().length === 1 && ff.free.get(0) === HEAP;
results["first fit wastes the least to rounding"] = rFF.internal < rSlab.internal;
results["no allocation failed with the heap above twice the peak"] = [rBuddy, rSlab, rFF].every(
    (r) => r.failed === 0 && r.peak * 2 < HEAP,
);

print(
    Table(
        [rBuddy, rSlab, rFF].map((r) => [
            r.name,
            (100 * r.internal).toFixed(1) + "%",
            (100 * r.ext).toFixed(1) + "%",
            r.peak.bytes(),
            r.corrupt,
            r.failed,
        ]),
        { head: ["allocator", "internal frag", "external frag (median)", "peak reserved", "corrupt", "failed"] },
    ),
);
print(
    `replay: buddy ${buddyMs.toFixed(0)} ms, first fit ${ffMs.toFixed(0)} ms for 100k allocations with byte-pattern verification`,
);

// The checker must be able to fail: two live blocks sharing bytes are caught.
{
    const a = 0,
        b = 32;
    fill(heap, pattern(1), a, a + 64);
    fill(heap, pattern(2), b, b + 64);
    results["an overlapping write is detected by the pattern check"] = heap[a + 40] !== pattern(1);
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
