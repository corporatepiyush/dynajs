import { RateLimiter } from "dyna:net";

let n = 0, fails = 0;
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}
function passMs(ms) { const t = Date.now(); while (Date.now() - t < ms) { } }

{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 3 });
    eq(rl.allow("a"), true, "first of three");
    eq(rl.allow("a"), true, "second");
    eq(rl.allow("a"), true, "third");
    eq(rl.allow("a"), false, "the burst is spent");
    eq(rl.allow("b"), true, "a different key has its own budget");
}

{
    const rl = new RateLimiter({ tokensPerSec: 100, burst: 1 });
    eq(rl.allow("k"), true, "the one token");
    eq(rl.allow("k"), false, "and it is gone");
    passMs(60);
    eq(rl.allow("k"), true, "time refilled it");
    ok(rl.tokens("k") <= 1, "refill is capped at the burst, never above");
}

{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 10 });
    eq(rl.allow("k", 4), true, "a cost of four");
    eq(rl.allow("k", 4), true, "and another");
    eq(rl.allow("k", 4), false, "the third does not fit in what is left");
    eq(rl.allow("k", 2), true, "but a smaller one does");
}

{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 1, slots: 16 });
    for (let i = 0; i < 100000; i++) rl.allow("key" + i);
    eq(rl.stats.slots, 16, "the table is still 16 slots after 100k keys");
    ok(rl.stats.live <= 16, "and cannot hold more entries than it has slots");
}

{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 1, slots: 8 });
    eq(rl.allow("victim"), true, "the first key spends its token");
    eq(rl.allow("victim"), false, "and is empty");
    let least = Infinity;
    for (let i = 0; i < 200; i++) least = Math.min(least, rl.tokens("probe" + i));
    eq(least, 1, "a key landing on an occupied slot still gets a full bucket");
}

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

{
    const rl = new RateLimiter({ tokensPerSec: 1, burst: 2 });
    rl.allow("k"); rl.allow("k"); rl.allow("k");
    eq(rl.stats.allowed, 2, "two allowed");
    eq(rl.stats.denied, 1, "one denied");
    eq(rl.stats.tokensPerSec, 1, "the configured rate is reported");
    eq(rl.stats.burst, 2, "and the burst");
}

threw(() => new RateLimiter(), "no options");
threw(() => new RateLimiter({}), "no tokensPerSec");
threw(() => new RateLimiter({ tokensPerSec: 0 }), "a zero rate");
threw(() => new RateLimiter({ tokensPerSec: -1 }), "a negative rate");
threw(() => new RateLimiter({ tokensPerSec: 1, burst: 0 }), "a zero burst");
threw(() => new RateLimiter({ tokensPerSec: 1, slots: 4 }), "fewer slots than the minimum");
threw(() => new RateLimiter({ tokensPerSec: 1 }).allow(), "allow with no key");
threw(() => new RateLimiter({ tokensPerSec: 1 }).allow("k", 0), "a zero cost");

{
    const rl = new RateLimiter({ tokensPerSec: 10, burst: 10 });
    const before = rl.tokens("k");
    eq(rl.allow("k", 9223372036854776), false,
       "an overflowing cost is DENIED, not allowed");
    ok(rl.tokens("k") <= before, "and the bucket is NOT refilled by the denial");
    eq(rl.allow("k"), true, "the limiter still works after the attack");
    eq(rl.allow("k", 11), false, "a cost above the burst is denied");
}


{
    const rl = new RateLimiter({ tokensPerSec: 10, burst: 5 });
    threw(() => rl.allow("k", 0));
    threw(() => rl.allow("k", -3));
    threw(() => rl.allow("k", NaN));
    ok(rl.allow("k", 5) === true, "a cost equal to the burst is granted");
    ok(rl.allow("k", 6) === false, "a cost above the burst is denied");
    let astro = "granted";
    try { astro = rl.allow("k", 1e19) ? "granted" : "denied"; }
    catch (e) { astro = "refused"; }
    ok(astro !== "granted", "an astronomical cost cannot be smuggled through (" + astro + ")");
}

if (fails) {
    print("test_ratelimit: " + fails + " FAILED of " + n);
    throw new Error("test_ratelimit failed");
}
print("test_ratelimit: " + n + " assertions, 0 failures");
