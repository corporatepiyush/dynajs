import { test, run, assert, assertEqual, assertThrows } from "./harness.js";

export class LRUCache {
  #capacity;
  #ttl;
  #now;
  #map = new Map();
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
    this.#map.delete(key);
    this.#map.set(key, entry);
    this.#stats.hits++;
    return entry.value;
  }

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

  set(key, value, { ttl = this.#ttl } = {}) {
    if (this.#map.has(key)) this.#map.delete(key);
    const expiresAt = ttl === Infinity ? Infinity : this.#now() + ttl;
    this.#map.set(key, { value, expiresAt });
    this.#evictIfNeeded();
    return this;
  }

  #evictIfNeeded() {
    while (this.#map.size > this.#capacity) {
      const oldest = this.#map.keys().next().value;
      this.#map.delete(oldest);
      this.#stats.evictions++;
    }
  }

  getOrCompute(key, compute) {
    if (this.has(key)) return this.get(key);
    const value = compute(key);
    this.set(key, value);
    this.#stats.misses++;
    return value;
  }

  delete(key) { return this.#map.delete(key); }
  clear() { this.#map.clear(); }

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

  *keysByRecency() {
    for (const key of this.#map.keys()) yield key;
  }

  *entries() {
    for (const [key, entry] of this.#map) {
      if (!this.#isExpired(entry)) yield [key, entry.value];
    }
  }
}

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
  c.get("a");
  assertEqual([...c.keysByRecency()], ["b", "c", "a"]);
  c.set("d", 4);
  assertEqual([...c.keysByRecency()], ["c", "a", "d"]);
  assertEqual(c.get("b"), undefined);
  assertEqual(c.stats.evictions, 1);
});

test("hit/miss statistics", () => {
  const c = new LRUCache({ capacity: 2 });
  c.set("x", 10);
  assertEqual(c.get("x"), 10);
  assertEqual(c.get("y"), undefined);
  assertEqual(c.get("x"), 10);
  assertEqual(c.stats.hits, 2);
  assertEqual(c.stats.misses, 1);
});

test("peek does not change recency", () => {
  const c = new LRUCache({ capacity: 2 });
  c.set("a", 1).set("b", 2);
  assertEqual(c.peek("a"), 1);
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
  assertEqual(c.get("k"), "v");
  clock.advance(60);
  assertEqual(c.get("k"), undefined);
  assertEqual(c.stats.expirations, 1);
  assert(!c.has("k"));
});

test("per-entry TTL override and prune()", () => {
  const clock = fakeClock();
  const c = new LRUCache({ capacity: 10, ttl: Infinity, now: clock.now });
  c.set("permanent", 1);
  c.set("brief", 2, { ttl: 10 });
  clock.advance(20);
  assertEqual(c.prune(), 1);
  assertEqual([...c.entries()], [["permanent", 1]]);
});

test("getOrCompute memoizes", () => {
  const c = new LRUCache({ capacity: 5 });
  let computes = 0;
  const compute = (k) => { computes++; return k.toUpperCase(); };
  assertEqual(c.getOrCompute("hi", compute), "HI");
  assertEqual(c.getOrCompute("hi", compute), "HI");
  assertEqual(computes, 1);
});

test("updating a key refreshes recency and value", () => {
  const c = new LRUCache({ capacity: 2 });
  c.set("a", 1).set("b", 2);
  c.set("a", 100);
  assertEqual([...c.keysByRecency()], ["b", "a"]);
  c.set("c", 3);
  assertEqual(c.peek("a"), 100);
  assertEqual(c.get("b"), undefined);
});

await run("LRU + TTL cache");
