import { LRU } from "dyna:structures";

let n = 0, fails = 0;
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}
function passMs(ms) { const t = Date.now(); while (Date.now() - t < ms) { } }

{
    const c = new LRU(8);
    c.setWithTTL("a", 1, 20);
    c.put("b", 2);
    eq(c.get("a"), 1, "a is readable before it expires");
    passMs(40);
    eq(c.get("a"), undefined, "a is gone after its TTL");
    eq(c.get("b"), 2, "b has no TTL and survives");
    eq(c.size, 1, "the expired entry was reclaimed on access");
}

{
    const c = new LRU(8, { ttlMs: 20 });
    c.put("k", "v");
    passMs(40);
    eq(c.has("k"), false, "an untouched cache still expires its entry");
    eq(c.get("k"), undefined, "and get agrees");
}

{
    // 400 ms TTL with 250 ms steps: the entry is 500 ms old by first-write
    // time (expired) but 250 ms by rewrite time (alive). A busy-wait can be
    // descheduled for longer than the slack under gate load, so the wait is
    // MEASURED: an attempt whose second step overran the TTL proves nothing
    // and is repeated instead of being scored.
    let scored = false;
    for (let attempt = 0; attempt < 10 && !scored; attempt++) {
        const c = new LRU(8, { ttlMs: 400 });
        c.put("k", 1);
        passMs(250);
        const t = Date.now();
        c.put("k", 2);
        passMs(250);
        const v = c.get("k");
        if (Date.now() - t < 370) {
            eq(v, 2, "writing a key again restarts its clock");
            scored = true;
        }
    }
    eq(scored, true, "the restart case ran inside its time window at least once");
}

{
    const seen = [];
    const c = new LRU(2, { onEvict: (k, v) => seen.push(k + "=" + v) });
    c.put("a", 1); c.put("b", 2); c.put("c", 3);
    eq(seen.join(","), "a=1", "onEvict receives the evicted key and value");
    eq(c.stats.evictions, 1, "one capacity eviction");
    eq(c.stats.expired, 0, "and nothing expired");
}

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

{
    let inner = null;
    const c = new LRU(1, { onEvict: () => { try { c.put("z", 9); } catch (e) { inner = e; } } });
    c.put("a", 1);
    c.put("b", 2);
    ok(inner !== null, "a reentrant put from onEvict is refused");
    ok(/onEvict/.test(String(inner && inner.message)), "and the message names onEvict");
}

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

threw(() => new LRU(4, { ttlMs: 0 }), "a zero ttlMs");
threw(() => new LRU(4, { ttlMs: -1 }), "a negative ttlMs");
threw(() => new LRU(4, { onEvict: 42 }), "a non-function onEvict");
threw(() => new LRU(4).setWithTTL("k", 1), "setWithTTL without a duration");
threw(() => new LRU(4).setWithTTL("k", 1, 0), "setWithTTL with a zero duration");

{
    const held = [];
    for (let i = 0; i < 8; i++) {
        const c = new LRU(2, { onEvict: () => held.length });
        c.put("a", i); c.put("b", i); c.put("c", i);
        held.push(c);
    }
    eq(held.length, 8, "many caches with callbacks stay alive");
}

{
    let m = "";
    try { new LRU(4, { ttlmMs: 30 }); } catch (e) { m = e.message; }
    eq(m, 'unknown option "ttlmMs" (valid: ttlMs, onEvict)',
       "bag names the key AND the valid set");
    m = "";
    try { new LRU(4, { ttl: 30 }); } catch (e) { m = e.message; }
    eq(m, 'unknown option "ttl" (valid: ttlMs, onEvict)',
       "the silent-expiry typo class is closed");
    eq(new LRU(4, { ttlMs: 20 }).capacity, 4, "a valid ttlMs bag is unchanged");
    eq(new LRU(4, { onEvict: () => {} }).capacity, 4, "a valid onEvict bag is unchanged");
    eq(new LRU(4).capacity, 4, "no bag still means defaults");
    eq(new LRU(4, null).capacity, 4, "null bag reads as absent");
    eq(new LRU(4, 42).capacity, 4, "a primitive bag reads as absent");
    const sym = Symbol("s");
    eq(new LRU(4, { ttlMs: 5, [sym]: 1 }).capacity, 4, "symbol keys are invisible");
}

if (fails) {
    print("test_lru_ttl: " + fails + " FAILED of " + n);
    throw new Error("test_lru_ttl failed");
}
print("test_lru_ttl: " + n + " assertions, 0 failures");
