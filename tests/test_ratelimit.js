/* RateLimiter: a token bucket over a FIXED table. The table not growing is the
   security property, so the test that matters is the one where a flood of
   distinct keys arrives -- memory must not move with the number of keys. */
import { RateLimiter } from "dyna:net";

let n = 0, fails = 0;
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}
function passMs(ms) { const t = Date.now(); while (Date.now() - t < ms) { } }

/* --- burst is spent, then refused --- */
{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 3 });
    eq(rl.allow("a"), true, "first of three");
    eq(rl.allow("a"), true, "second");
    eq(rl.allow("a"), true, "third");
    eq(rl.allow("a"), false, "the burst is spent");
    eq(rl.allow("b"), true, "a different key has its own budget");
}

/* --- refill is by elapsed time, not by call count --- */
{
    const rl = new RateLimiter({ tokensPerSec: 100, burst: 1 });
    eq(rl.allow("k"), true, "the one token");
    eq(rl.allow("k"), false, "and it is gone");
    passMs(60);                       /* 100/s => 6 tokens, capped at burst 1 */
    eq(rl.allow("k"), true, "time refilled it");
    ok(rl.tokens("k") <= 1, "refill is capped at the burst, never above");
}

/* --- cost --- */
{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 10 });
    eq(rl.allow("k", 4), true, "a cost of four");
    eq(rl.allow("k", 4), true, "and another");
    eq(rl.allow("k", 4), false, "the third does not fit in what is left");
    eq(rl.allow("k", 2), true, "but a smaller one does");
}

/* --- the table is FIXED: a flood of distinct keys must not grow it --- */
{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 1, slots: 16 });
    for (let i = 0; i < 100000; i++) rl.allow("key" + i);
    eq(rl.stats.slots, 16, "the table is still 16 slots after 100k keys");
    ok(rl.stats.live <= 16, "and cannot hold more entries than it has slots");
}

/* --- a COLLIDING key must not inherit the other key's spent budget. With a
   never-used slot the refill path fills it to burst by accident (last_ms is 0,
   so the elapsed time is enormous), which is why this needs a real collision:
   8 slots and 200 probes make one certain. --- */
{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 1, slots: 8 });
    eq(rl.allow("victim"), true, "the first key spends its token");
    eq(rl.allow("victim"), false, "and is empty");
    let least = Infinity;
    for (let i = 0; i < 200; i++) least = Math.min(least, rl.tokens("probe" + i));
    eq(least, 1, "a key landing on an occupied slot still gets a full bucket");
}

/* --- reset --- */
{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 1 });
    rl.allow("k");
    eq(rl.allow("k"), false, "spent");
    rl.reset("k");
    eq(rl.allow("k"), true, "reset gives the key a fresh bucket");
    rl.allow("z");
    rl.reset();
    eq(rl.allow("z"), true, "reset with no key clears everything");
}

/* --- counters --- */
{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 2 });
    rl.allow("k"); rl.allow("k"); rl.allow("k");
    eq(rl.stats.allowed, 2, "two allowed");
    eq(rl.stats.denied, 1, "one denied");
    eq(rl.stats.tokensPerSec, 1, "the configured rate is reported");
    eq(rl.stats.burst, 2, "and the burst");
}

/* --- refusals --- */
threw(() => new RateLimiter(), "no options");
threw(() => new RateLimiter({}), "no tokensPerSec");
threw(() => new RateLimiter({ tokensPerSec: 0 }), "a zero rate");
threw(() => new RateLimiter({ tokensPerSec: -1 }), "a negative rate");
threw(() => new RateLimiter({ tokensPerSec: 1, burst: 0 }), "a zero burst");
threw(() => new RateLimiter({ tokensPerSec: 1, slots: 4 }), "fewer slots than the minimum");
threw(() => new RateLimiter({ tokensPerSec: 1 }).allow(), "allow with no key");
threw(() => new RateLimiter({ tokensPerSec: 1 }).allow("k", 0), "a zero cost");

if (fails) {
    print("test_ratelimit: " + fails + " FAILED of " + n);
    throw new Error("test_ratelimit failed");
}
print("test_ratelimit: " + n + " assertions, 0 failures");
