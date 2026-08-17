// lru_cache.js — a Least-Recently-Used cache with optional per-entry TTL.
//
// Recency is tracked "for free" using a Map's guaranteed insertion order: a
// cache hit deletes and re-inserts the key so it becomes the newest entry, and
// eviction always removes the *first* key yielded by the Map iterator (the
// oldest). Expiry uses an injected clock, so TTL behaviour is tested
// deterministically without real waiting.
//
// Engine features exercised: Map insertion-order semantics + iterators,
// private fields, generators for recency views, and dependency injection of
// the clock.

import { test, run, assert, assertEqual, assertThrows } from "./harness.js";

/**
 * @template K, V
 */
export class LRUCache {
  #capacity;
  #ttl; // default time-to-live in ms (Infinity = never expire)
  #now; // injected clock
  #map = new Map(); // key -> { value, expiresAt }
  #stats = { hits: 0, misses: 0, evictions: 0, expirations: 0 };

  constructor({ capacity, ttl = Infinity, now = () => Date.now() } = {}) {
    if (!Number.isInteger(capacity) || capacity <= 0) {
      throw new RangeError("capacity must be a positive integer");
    }
    this.#capacity = capacity;
    this.#ttl = ttl;
    this.#now = now;
  }

  get size() { return this.#map.size; }
  get stats() { return { ...this.#stats }; }

  #isExpired(entry) {
    return entry.expiresAt <= this.#now();
  }

  /** Read a key, refreshing its recency. Returns undefined on miss/expiry. */
  get(key) {
    const entry = this.#map.get(key);
    if (entry === undefined) {
      this.#stats.misses++;
      return undefined;
    }
    if (this.#isExpired(entry)) {
      this.#map.delete(key);
      this.#stats.misses++;
      this.#stats.expirations++;
      return undefined;
    }
    // Move to most-recently-used: delete + re-insert puts it at the Map's tail.
    this.#map.delete(key);
    this.#map.set(key, entry);
    this.#stats.hits++;
    return entry.value;
  }

  /** Read without affecting recency (and without counting hit/miss stats). */
  peek(key) {
    const entry = this.#map.get(key);
    if (entry === undefined || this.#isExpired(entry)) return undefined;
    return entry.value;
  }

  has(key) {
    const entry = this.#map.get(key);
    if (entry === undefined) return false;
    if (this.#isExpired(entry)) {
      this.#map.delete(key);
      this.#stats.expirations++;
      return false;
    }
    return true;
  }

  /** Insert/update. Optional per-call ttl overrides the default. */
  set(key, value, { ttl = this.#ttl } = {}) {
    if (this.#map.has(key)) this.#map.delete(key); // reinsert at tail
    const expiresAt = ttl === Infinity ? Infinity : this.#now() + ttl;
    this.#map.set(key, { value, expiresAt });
    this.#evictIfNeeded();
    return this;
  }

  #evictIfNeeded() {
    while (this.#map.size > this.#capacity) {
      // The first key from the iterator is the least-recently-used.
      const oldest = this.#map.keys().next().value;
      this.#map.delete(oldest);
      this.#stats.evictions++;
    }
  }

  /** Memoize: return the cached value or compute, store, and return it. */
  getOrCompute(key, compute) {
    if (this.has(key)) return this.get(key);
    const value = compute(key);
    this.set(key, value);
    this.#stats.misses++; // getOrCompute miss (has() above returned false)
    return value;
  }

  delete(key) { return this.#map.delete(key); }
  clear() { this.#map.clear(); }

  /** Purge every expired entry; returns the number removed. */
  prune() {
    let removed = 0;
    for (const [key, entry] of this.#map) {
      if (this.#isExpired(entry)) {
        this.#map.delete(key);
        this.#stats.expirations++;
        removed++;
      }
    }
    return removed;
  }

  /** Keys from least- to most-recently-used. */
  *keysByRecency() {
    for (const key of this.#map.keys()) yield key;
  }

  /** Live (non-expired) entries as [key, value], newest last. */
  *entries() {
    for (const [key, entry] of this.#map) {
      if (!this.#isExpired(entry)) yield [key, entry.value];
    }
  }
}

// --- tests -------------------------------------------------------------------

/** A mutable fake clock for deterministic TTL tests. */
function fakeClock(start = 0) {
  let t = start;
  return { now: () => t, advance: (ms) => { t += ms; } };
}

test("rejects invalid capacity", () => {
  assertThrows(() => new LRUCache({ capacity: 0 }), "positive integer");
  assertThrows(() => new LRUCache({ capacity: -3 }), "positive integer");
  assertThrows(() => new LRUCache({ capacity: 1.5 }), "positive integer");
});

test("evicts the least-recently-used entry", () => {
  const c = new LRUCache({ capacity: 3 });
  c.set("a", 1).set("b", 2).set("c", 3);
  assertEqual([...c.keysByRecency()], ["a", "b", "c"]);
  c.get("a"); // touch a -> becomes newest
  assertEqual([...c.keysByRecency()], ["b", "c", "a"]);
  c.set("d", 4); // capacity exceeded -> evict oldest (b)
  assertEqual([...c.keysByRecency()], ["c", "a", "d"]);
  assertEqual(c.get("b"), undefined);
  assertEqual(c.stats.evictions, 1);
});

test("hit/miss statistics", () => {
  const c = new LRUCache({ capacity: 2 });
  c.set("x", 10);
  assertEqual(c.get("x"), 10); // hit
  assertEqual(c.get("y"), undefined); // miss
  assertEqual(c.get("x"), 10); // hit
  assertEqual(c.stats.hits, 2);
  assertEqual(c.stats.misses, 1);
});

test("peek does not change recency", () => {
  const c = new LRUCache({ capacity: 2 });
  c.set("a", 1).set("b", 2);
  assertEqual(c.peek("a"), 1);
  // a is still oldest, so inserting c evicts a (peek did not refresh it).
  c.set("c", 3);
  assertEqual(c.get("a"), undefined);
  assertEqual(c.get("b"), 2);
});

test("TTL expiry with an injected clock", () => {
  const clock = fakeClock();
  const c = new LRUCache({ capacity: 10, ttl: 100, now: clock.now });
  c.set("k", "v");
  assertEqual(c.get("k"), "v");
  clock.advance(50);
  assertEqual(c.get("k"), "v"); // still fresh
  clock.advance(60); // total 110 > 100
  assertEqual(c.get("k"), undefined); // expired
  assertEqual(c.stats.expirations, 1);
  assert(!c.has("k"));
});

test("per-entry TTL override and prune()", () => {
  const clock = fakeClock();
  const c = new LRUCache({ capacity: 10, ttl: Infinity, now: clock.now });
  c.set("permanent", 1);
  c.set("brief", 2, { ttl: 10 });
  clock.advance(20);
  assertEqual(c.prune(), 1); // only "brief" expired
  assertEqual([...c.entries()], [["permanent", 1]]);
});

test("getOrCompute memoizes", () => {
  const c = new LRUCache({ capacity: 5 });
  let computes = 0;
  const compute = (k) => { computes++; return k.toUpperCase(); };
  assertEqual(c.getOrCompute("hi", compute), "HI");
  assertEqual(c.getOrCompute("hi", compute), "HI");
  assertEqual(computes, 1); // second call served from cache
});

test("updating a key refreshes recency and value", () => {
  const c = new LRUCache({ capacity: 2 });
  c.set("a", 1).set("b", 2);
  c.set("a", 100); // update -> a becomes newest
  assertEqual([...c.keysByRecency()], ["b", "a"]);
  c.set("c", 3); // evicts b (oldest)
  assertEqual(c.peek("a"), 100);
  assertEqual(c.get("b"), undefined);
});

await run("LRU + TTL cache");
