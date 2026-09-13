/* LRU expiry, eviction callback and counters. Expiry is on the MONOTONIC
   clock, so these sleep by spinning on Date.now() only to pass real time --
   no assertion here depends on a duration, which would fail for reasons that
   are not bugs. */
import { LRU } from "dyna:structures";

let n = 0, fails = 0;
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}
function passMs(ms) { const t = Date.now(); while (Date.now() - t < ms) { } }

/* --- explicit per-entry TTL --- */
{
    const c = new LRU(8);
    c.setWithTTL("a", 1, 20);
    c.put("b", 2);                       /* no TTL: never expires */
    eq(c.get("a"), 1, "a is readable before it expires");
    passMs(40);
    eq(c.get("a"), undefined, "a is gone after its TTL");
    eq(c.get("b"), 2, "b has no TTL and survives");
    eq(c.size, 1, "the expired entry was reclaimed on access");
}

/* --- the QUIET cache: nothing else touches it, and the entry must still be
   unreadable. A busy cache would pass this either way. --- */
{
    const c = new LRU(8, { ttlMs: 20 });
    c.put("k", "v");
    passMs(40);
    eq(c.has("k"), false, "an untouched cache still expires its entry");
    eq(c.get("k"), undefined, "and get agrees");
}

/* --- ttlMs applies to every put, and a re-put restamps --- */
{
    const c = new LRU(8, { ttlMs: 40 });
    c.put("k", 1);
    passMs(25);
    c.put("k", 2);                       /* restamps: 40ms from now */
    passMs(25);
    eq(c.get("k"), 2, "writing a key again restarts its clock");
}

/* --- onEvict on capacity, with the key and value --- */
{
    const seen = [];
    const c = new LRU(2, { onEvict: (k, v) => seen.push(k + "=" + v) });
    c.put("a", 1); c.put("b", 2); c.put("c", 3);   /* a is the LRU victim */
    eq(seen.join(","), "a=1", "onEvict receives the evicted key and value");
    eq(c.stats.evictions, 1, "one capacity eviction");
    eq(c.stats.expired, 0, "and nothing expired");
}

/* --- onEvict on expiry, counted separately from a capacity eviction --- */
{
    const seen = [];
    const c = new LRU(8, { ttlMs: 20, onEvict: (k) => seen.push(k) });
    c.put("x", 1);
    passMs(40);
    eq(c.purgeExpired(), 1, "purgeExpired reports what it removed");
    eq(seen.join(","), "x", "onEvict fires for an expired entry too");
    eq(c.stats.expired, 1, "expired is counted apart from evictions");
    eq(c.stats.evictions, 0, "capacity evicted nothing");
    eq(c.size, 0, "and the memory is reclaimed");
}

/* --- reentrancy: a callback that mutates the cache it is called from. The
   entry is already detached when the callback runs, so the structure is
   consistent; mutating it anyway is refused rather than corrupting it. --- */
{
    let inner = null;
    const c = new LRU(1, { onEvict: () => { try { c.put("z", 9); } catch (e) { inner = e; } } });
    c.put("a", 1);
    c.put("b", 2);                       /* evicts a, runs the callback */
    ok(inner !== null, "a reentrant put from onEvict is refused");
    ok(/onEvict/.test(String(inner && inner.message)), "and the message names onEvict");
}

/* --- counters --- */
{
    const c = new LRU(4);
    c.put("a", 1);
    c.get("a"); c.get("a"); c.get("nope");
    const s = c.stats;
    eq(s.hits, 2, "two hits");
    eq(s.misses, 1, "one miss");
    eq(s.size, 1, "size");
    eq(s.capacity, 4, "capacity");
}

/* --- refusals --- */
threw(() => new LRU(4, { ttlMs: 0 }), "a zero ttlMs");
threw(() => new LRU(4, { ttlMs: -1 }), "a negative ttlMs");
threw(() => new LRU(4, { onEvict: 42 }), "a non-function onEvict");
threw(() => new LRU(4).setWithTTL("k", 1), "setWithTTL without a duration");
threw(() => new LRU(4).setWithTTL("k", 1, 0), "setWithTTL with a zero duration");

/* --- the callback is reachable from the cache, so the collector must see it:
   several live instances are what triggers a cycle collection. --- */
{
    const held = [];
    for (let i = 0; i < 8; i++) {
        const c = new LRU(2, { onEvict: () => held.length });
        c.put("a", i); c.put("b", i); c.put("c", i);
        held.push(c);
    }
    eq(held.length, 8, "many caches with callbacks stay alive");
}

if (fails) {
    print("test_lru_ttl: " + fails + " FAILED of " + n);
    throw new Error("test_lru_ttl failed");
}
print("test_lru_ttl: " + n + " assertions, 0 failures");
