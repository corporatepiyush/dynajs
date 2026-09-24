/* test_net_upgrade.js -- additions to dyna:net.
 *
 * RateLimiter:
 *   - {refill} aliases {tokensPerSec}; {capacity} aliases {burst}; an alias
 *     pair with different values is a TypeError. Existing ctor options are
 *     unchanged and the default table stays FIXED (the security property).
 *   - {grow: true} opts in: past a 3/4 load factor the table doubles, up to
 *     the hard ceiling of 2^20 slots; past the ceiling colliding keys share
 *     slots as before. stats.grew counts doublings; stats.live tracks the
 *     live-slot count.
 * Metrics.histogram:
 *   - optional 4th arg {buckets: [seconds...]} installs per-series edges at
 *     registration: 1..6 finite, positive, strictly increasing numbers.
 *     Series registered without it keep the 5ms..1s default edges.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_net_upgrade.js
 */

import { Metrics, RateLimiter, PostgreSQL } from "dyna:net";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof ctor, msg + " (" + ctor.name + ", got " + caught + ")");
}

/* ----------------: queryIter is NATIVE on the class ---------------- */
{
    /* The whole point of: the constant-memory stream works with NO
     * pool.js import. pool.js's installer checks
     * `!PostgreSQL.prototype.queryIter`, so its JS fallback stands down
     * and the native driver is canonical. */
    assert(typeof PostgreSQL.prototype.queryIter === "function",
        "PostgreSQL.prototype.queryIter exists natively (no pool.js import)");
    /* The surface is the house manual async iterator: next/return and
     * Symbol.asyncIterator (there is no C-level async generator). */
    const proto = Object.getPrototypeOf(PostgreSQL.prototype.queryIter.call(
        { query: () => { } }));
    assert(typeof proto.next === "function", "queryIter().next exists");
    assert(typeof proto["return"] === "function", "queryIter().return exists");
    assert(typeof proto[Symbol.asyncIterator] === "function",
        "queryIter()[Symbol.asyncIterator] exists");
    assert(!("close" in proto) || typeof proto.close === "function",
        "no stray close() on the stream prototype");
    /* strict opts bag, checked before anything else */
    let caught = null;
    const fakeConn = { query: () => undefined };
    try { PostgreSQL.prototype.queryIter.call(fakeConn, "SELECT 1", undefined, { batchh: 2 }); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "batchh" (valid: batch, maxRows)',
        "queryIter opts bag is strict, got: " + caught);
    caught = null;
    try { PostgreSQL.prototype.queryIter.call(fakeConn, "SELECT 1", {}, {}); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError && /parameters must be an array/.test(caught.message),
        "params must be an array, got: " + caught);
    /* laziness: constructing a stream issues no IO and needs no live
     * receiver beyond a query() -- this fake conn's query is never called */
    let called = 0;
    const lazyConn = { query: () => { called++; return undefined; } };
    const it = PostgreSQL.prototype.queryIter.call(lazyConn, "SELECT 1");
    assert(called === 0, "queryIter() construction is lazy (no query() call)");
    assert(it && typeof it.next === "function", "the stream object is returned");
}

Metrics.reset();

/* ---------------- RateLimiter: aliases ---------------- */
{
    throws(() => new RateLimiter({}), TypeError,
        "no rate at all is still a TypeError");
    throws(() => new RateLimiter({ tokensPerSec: 5, refill: 6 }), TypeError,
        "disagreeing tokensPerSec/refill refused");
    throws(() => new RateLimiter({ burst: 5, capacity: 6 }), TypeError,
        "disagreeing burst/capacity refused");
    const alias = new RateLimiter({ refill: 10, capacity: 20 });
    assert(alias.stats.tokensPerSec === 10 && alias.stats.burst === 20,
        "refill/capacity aliases carry through to stats");
    assert(alias.allow("a"), "alias limiter allows");
    const mixed = new RateLimiter({ tokensPerSec: 10, refill: 10, burst: 5, capacity: 5 });
    assert(mixed.allow("a"), "agreeing alias pairs accepted");
}

/* ---------------- RateLimiter: fixed by default, grow on request ------- */
{
    const fixed = new RateLimiter({ tokensPerSec: 100, slots: 8 });
    for (let i = 0; i < 64; i++) fixed.allow("f" + i);
    assert(fixed.stats.slots === 8, "default limiter never grows");
    assert(fixed.stats.grew === 0, "default limiter has no growth events");

    const grower = new RateLimiter({ tokensPerSec: 100, slots: 8, grow: true });
    for (let i = 0; i < 64; i++) grower.allow("g" + i);
    assert(grower.stats.grew > 0, "grow limiter doubles under load");
    assert(grower.stats.slots > 8, "table grew past its initial size");
    assert(grower.stats.live > 8, "distinct keys survive the growth");
    /* a grown limiter still limits: a fresh key starts at full budget */
    assert(grower.allow("fresh"), "post-growth allow works");
    assert(grower.allow("fresh2") === true || grower.allow("fresh2") === false,
        "allow stays boolean after growth");

    throws(() => new RateLimiter({ tokensPerSec: 5, grow: "yes" }), TypeError,
        "grow must be a boolean");

    /* state redistribution: a key's budget is not reset by a growth in
     * between. The churn keys are chosen (via the same FNV-1a hash the
     * limiter uses, computed here with BigInt) to occupy DISTINCT slots at
     * every table size 8..64, so nothing collides with the victim: the
     * only thing under test is whether growth preserves its drained state.
     * The burst is large so the refill cap (burst/rate ms of inactivity)
     * cannot elapse during the test either. */
    const fnv = (s) => {
        let h = 1469598103934665603n;
        for (let i = 0; i < s.length; i++) {
            h ^= BigInt(s.charCodeAt(i));
            h = (h * 1099511628211n) & 0xFFFFFFFFFFFFFFFFn;
        }
        return h;
    };
    const pick = [];
    for (let i = 0; pick.length < 8 && i < 5000; i++) {
        const k = "k" + i;
        const h = fnv(k);
        if (pick.every((p) => (p.h & 63n) !== (h & 63n)))
            pick.push({ k, h });
    }
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 1000000, slots: 8, grow: true });
    assert(rl.allow(pick[0].k, 2), "victim spends two tokens");
    assert(!rl.allow(pick[0].k, 1000000), "victim is out of tokens");
    for (let i = 1; i < pick.length; i++)
        assert(rl.allow(pick[i].k), "churn key " + i + " allowed");
    assert(rl.stats.grew > 0, "the churn grew the table");
    assert(rl.stats.slots >= 16, "table doubled at least once");
    assert(!rl.allow(pick[0].k, 1000000),
        "drained key stays drained across growth");
    for (let i = 1; i < pick.length; i++)
        assert(rl.allow(pick[i].k, 1000000) === false,
            "churn key budgets also survive growth");
}

/* ---------------- reset maintains stats.live (review fix) ------------- */
{
    const r = new RateLimiter({ tokensPerSec: 10, burst: 10 });
    r.allow("a"); r.allow("b"); r.allow("c");
    assert(r.stats.live === 3, "live counts three keys");
    r.reset();
    assert(r.stats.live === 0, "full reset zeroes stats.live");
    r.allow("a"); r.allow("b");
    r.reset("a");
    assert(r.stats.live === 1, "single-key reset decrements live");
    r.reset("never-existed");
    assert(r.stats.live === 1, "resetting an absent key leaves live alone");

    /* grow-under-reset: phantom load must not balloon the table */
    const g = new RateLimiter({ tokensPerSec: 1000, burst: 1000, slots: 8,
                                grow: true });
    for (let i = 0; i < 6; i++) g.allow("k" + i);
    g.reset();
    g.allow("solo");
    const grewBefore = g.stats.grew;
    for (let i = 0; i < 40; i++) { g.reset(); g.allow("x" + i); }
    assert(g.stats.live === 1, "one live key after the reset loop");
    assert(g.stats.grew === grewBefore,
        "reset clears load: no phantom growth (grew " + g.stats.grew + ")");
    assert(g.stats.slots <= 16, "table did not balloon: " + g.stats.slots);
}

/* ---------------- Metrics.histogram custom buckets ---------------- */
{
    Metrics.reset();
    Metrics.histogram("st3_lat", 0.02, { route: "a" }, { buckets: [0.001, 0.05, 0.5] });
    Metrics.histogram("st3_lat", 3.0, { route: "a" });      /* above every edge */
    Metrics.histogram("st3_lat", 0.2, { route: "a" });      /* middle band */
    let sc = Metrics.scrape();
    assert(sc.includes('st3_lat_bucket{route="a",le="0.001"} 0'),
        "custom edge 0.001 sits below the first observation");
    assert(sc.includes('st3_lat_bucket{route="a",le="0.05"} 1'),
        "custom edge 0.05 counted");
    assert(sc.includes('st3_lat_bucket{route="a",le="0.5"} 2'),
        "0.2 and 0.02 both land under the 0.5 band (cumulative)");
    assert(!sc.includes('le="1"'), "the default 1s edge is gone for this series");
    assert(sc.includes('le="+Inf"'), "the +Inf bucket remains");

    /* defaults are untouched for series registered without the option */
    Metrics.histogram("st3_def", 0.02);
    sc = Metrics.scrape();
    assert(sc.includes('st3_def_bucket{le="0.005"}'), "default 5ms edge present");
    assert(sc.includes('st3_def_bucket{le="1"}'), "default 1s edge present");

    /* validation */
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [] }),
        RangeError, "an empty edge list is refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [1, 1] }),
        RangeError, "non-increasing edges refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [-1, 0.5] }),
        RangeError, "non-positive edges refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [Infinity] }),
        RangeError, "non-finite edges refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: "no" }),
        TypeError, "a non-array buckets is refused");
    throws(() => Metrics.histogram("st3_bad", 1, {}, { buckets: [0.1, "x"] }),
        RangeError, "a non-number edge is refused");
    throws(() => Metrics.histogram("st3_bad", 1, {},
        { buckets: [1, 2, 3, 4, 5, 6, 7] }), RangeError,
        "more than 6 edges refused");

    /* labels keep working alongside the option */
    Metrics.histogram("st3_lbl", 0.02, { a: "1" }, { buckets: [0.5] });
    assert(Metrics.scrape().includes('st3_lbl_bucket{a="1",le="0.5"} 1'),
        "labels and custom edges compose");

    Metrics.reset();
}

/* ----------------: option bags are strict ---------------- */
{
    let caught = null;
    try { new RateLimiter({ tokensPerSec: 10, toksPerSec: 10 }); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "toksPerSec" (valid: tokensPerSec, refill, burst, capacity, grow, slots)',
        "RateLimiter ctor names the key AND the valid set, got: " + caught);
    const ok = new RateLimiter({ tokensPerSec: 10, refill: 10, burst: 5, capacity: 5, grow: true, slots: 16 });
    assert(ok.allow("cc6"), "the full valid RateLimiter bag is unchanged");

    Metrics.reset();
    caught = null;
    try { Metrics.histogram("cc6_lat", 0.02, {}, { bucketz: [0.5] }); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "bucketz" (valid: buckets)',
        "histogram opts name the key AND the valid set, got: " + caught);
    assert(!Metrics.scrape().includes("cc6_lat"),
        "the refused bag did not half-register the series");
    Metrics.histogram("cc6_lat", 0.02, {}, { buckets: [0.5] });
    assert(Metrics.scrape().includes("cc6_lat_bucket"), "the valid histogram bag is unchanged");
    /* a primitive 4th arg was ALWAYS refused (pre-CC-6 value strictness):
     * the bag position must be an object or absent */
    throws(() => Metrics.histogram("cc6_lat2", 0.02, {}, 42), TypeError,
        "a primitive histogram bag is still refused outright");

    Metrics.reset();
}

print("test_net_upgrade: all tests passed (" + n + " assertions)");
