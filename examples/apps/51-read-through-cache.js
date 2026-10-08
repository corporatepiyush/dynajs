// 51 · Read-through cache — an LRU in front of a slow store, with TTLs, stampede protection and invalidation.
//
// WHAT IT SHOWS
//   - dyna:structures LRU: bounded memory, per-entry TTL, hit/miss counters
//   - single-flight loading: a hundred concurrent misses for one key cause ONE query, not a hundred
//   - negative caching, so a missing key cannot be used to hammer the database
//   - write-through invalidation, and jittered TTLs so entries do not all expire together
//   - serving the stale value when the backing store is down
//
// RUN      dynajs examples/apps/51-read-through-cache.js

import { LRU } from "dyna:structures";
import { SQLite } from "dyna:net";
import { Random } from "dyna:random";

const MISSING = Symbol("missing");

class ReadThroughCache {
    constructor(load, { capacity = 1000, ttlMs = 60000, negativeTtlMs = 5000, jitter = 0.1, seed } = {}) {
        this.load = load;
        this.ttlMs = ttlMs; this.negativeTtlMs = negativeTtlMs; this.jitter = jitter;
        this.fresh = new LRU(capacity);              // entries within their TTL
        this.stale = new LRU(capacity);              // last known values, kept for outages
        this.inFlight = new Map();                   // key -> Promise of the load in progress
        this.rng = new Random(seed);
        this.counters = { loads: 0, coalesced: 0, servedStale: 0 };
    }

    async get(key) {
        const hit = this.fresh.get(key);
        if (hit !== undefined) return hit === MISSING ? undefined : hit;

        // Someone is already loading this key: wait for their result instead
        // of sending a second identical query. This is what stops a cache
        // expiry under load from becoming a thundering herd.
        const pending = this.inFlight.get(key);
        if (pending) { this.counters.coalesced++; return pending; }

        const loading = (async () => {
            try {
                this.counters.loads++;
                const value = await this.load(key);
                this.#store(key, value);
                return value;
            } catch (e) {
                // The store is unavailable. An old answer usually beats an error.
                const old = this.stale.get(key);
                if (old === undefined) throw e;
                this.counters.servedStale++;
                return old === MISSING ? undefined : old;
            } finally {
                this.inFlight.delete(key);
            }
        })();
        this.inFlight.set(key, loading);
        return loading;
    }

    #store(key, value) {
        const base = value === undefined ? this.negativeTtlMs : this.ttlMs;
        // Spread expiries by +/- jitter so a burst of fills does not turn into
        // a burst of simultaneous reloads one TTL later.
        const ttl = base * (1 + (this.rng.nextFloat() * 2 - 1) * this.jitter);
        const cached = value === undefined ? MISSING : value;
        this.fresh.setWithTTL(key, cached, Math.max(1, Math.round(ttl)));
        this.stale.set(key, cached);
    }

    // Call after a write so readers do not see the old value for a whole TTL.
    invalidate(key) { this.fresh.delete(key); }
    get stats() { return { ...this.counters, ...this.fresh.stats }; }
}

// ---- a deliberately slow backing store --------------------------------------
const db = new SQLite(":memory:");
db.exec("CREATE TABLE products(sku TEXT PRIMARY KEY, name TEXT, price INTEGER)");
for (const [sku, name, price] of [["kb-75", "Keyboard", 12990], ["ms-01", "Mouse", 3990], ["hub-4", "USB hub", 2490]])
    db.exec("INSERT INTO products VALUES (?, ?, ?)", [sku, name, price]);

let queries = 0, outage = false;
async function loadProduct(sku) {
    queries++;
    await sleep(15);                                   // network + query time
    if (outage) throw new Error("database unavailable");
    return db.query("SELECT * FROM products WHERE sku = ?", [sku])[0];   // undefined when absent
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const cache = new ReadThroughCache(loadProduct, { capacity: 100, ttlMs: 80, negativeTtlMs: 80, seed: 1 });

// 1. Miss then hit.
check((await cache.get("kb-75")).name === "Keyboard" && queries === 1, "the first read loads from the store");
check((await cache.get("kb-75")).price === 12990 && queries === 1, "the second read is served from memory");

// 2. A stampede: 100 concurrent readers of a cold key.
const herd = await Promise.all(Array.from({ length: 100 }, () => cache.get("ms-01")));
check(herd.every((p) => p.name === "Mouse") && queries === 2, `100 concurrent misses cost one query (${queries - 1})`);
check(cache.stats.coalesced === 99, "99 readers shared the first reader's load");

// 3. Negative caching: a missing key is remembered too.
check(await cache.get("nope") === undefined && await cache.get("nope") === undefined && queries === 3, "an absent key is queried once");

// 4. Write-through: update, invalidate, read your own write.
db.exec("UPDATE products SET price = ? WHERE sku = ?", [9990, "kb-75"]);
check((await cache.get("kb-75")).price === 12990, "without invalidation the old price is still served");
cache.invalidate("kb-75");
check((await cache.get("kb-75")).price === 9990, "after invalidation the new price is read");

// 5. Expiry: after the TTL the value is reloaded.
const before = queries;
await sleep(120);
await cache.get("ms-01");
check(queries === before + 1, "an expired entry is reloaded once");

// 6. Outage: expired entries are served stale, unknown keys fail.
outage = true;
await sleep(120);
check((await cache.get("kb-75")).price === 9990 && cache.stats.servedStale === 1, "during an outage the last known value is served");
const failed = await cache.get("hub-4").then(() => "", (e) => e.message);
check(failed === "database unavailable", "a key never loaded has nothing to fall back on");
outage = false;
check((await cache.get("hub-4")).name === "USB hub", "the store recovering is picked up on the next read");

console.log("self-test passed:", JSON.stringify(cache.stats), "| store queries:", queries);
db.close();
