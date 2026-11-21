// covers: dyna:net.RateLimiter
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_rate_limiter_upgrade");
import { RateLimiter } from "dyna:net";

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

__kitS.section("no rate at all is still a TypeError", () => {
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
});

__kitS.section("default limiter never grows", () => {
    const fixed = new RateLimiter({ tokensPerSec: 100, slots: 8 });
    for (let i = 0; i < 64; i++) fixed.allow("f" + i);
    assert(fixed.stats.slots === 8, "default limiter never grows");
    assert(fixed.stats.grew === 0, "default limiter has no growth events");

    const grower = new RateLimiter({ tokensPerSec: 100, slots: 8, grow: true });
    for (let i = 0; i < 64; i++) grower.allow("g" + i);
    assert(grower.stats.grew > 0, "grow limiter doubles under load");
    assert(grower.stats.slots > 8, "table grew past its initial size");
    assert(grower.stats.live > 8, "distinct keys survive the growth");
    assert(grower.allow("fresh"), "post-growth allow works");
    assert(grower.allow("fresh2") === true || grower.allow("fresh2") === false,
        "allow stays boolean after growth");

    throws(() => new RateLimiter({ tokensPerSec: 5, grow: "yes" }), TypeError,
        "grow must be a boolean");

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
});

__kitS.section("live counts three keys", () => {
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
});
__kitS.section("unknown option \"toksPerSec\" (valid: tokensPerSec, refill, burst, capacity, grow, slots)", () => {
    let caught = null;
    try { new RateLimiter({ tokensPerSec: 10, toksPerSec: 10 }); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "toksPerSec" (valid: tokensPerSec, refill, burst, capacity, grow, slots)',
        "RateLimiter ctor names the key AND the valid set, got: " + caught);
    const ok = new RateLimiter({ tokensPerSec: 10, refill: 10, burst: 5, capacity: 5, grow: true, slots: 16 });
    assert(ok.allow("cc6"), "the full valid RateLimiter bag is unchanged");

});
__kitS.end();

print("test_net_rate_limiter_upgrade: all tests passed (" + n + " assertions)");
