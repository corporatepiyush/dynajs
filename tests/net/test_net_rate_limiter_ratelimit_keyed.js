// covers: dyna:net.RateLimiter
// tests/net/test_ratelimit_keyed.js -- RateLimiter hashes keys with SipHash-1-3
// under a per-instance random key (B14). With the old unkeyed FNV-1a, a client
// could precompute keys that all land on one slot (FNV-1a's low bits depend
// only on low bits, so 16-bit collisions cost a few hundred tries each; the old
// code seeded it with 1469598103934665603, whose low 32 bits are 0x739d0383) and
// turn every allow() into a scan of one long probe cluster. The keys built
// here collide under FNV-1a; keyed, they spread like any others, so inserting
// and re-querying them costs about the same as unrelated keys.
import { RateLimiter } from "dyna:net";
import { perfCheck } from "../lib/perf.js";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

const P = 0x1b3;
function fnvLow32(str, h) {
    for (let i = 0; i < str.length; i++) h = Math.imul((h ^ str.charCodeAt(i)) >>> 0, P) >>> 0;
    return h;
}
let pinv = P;
for (let i = 0; i < 5; i++) pinv = Math.imul(pinv, (2 - Math.imul(P, pinv)) >>> 0) >>> 0;

const TARGET = 0x1234;
const g = Math.imul(TARGET, pinv) & 0xffff;
function collidingKey(i) {
    const prefix = "k" + i + "-";
    const h0 = fnvLow32(prefix, 0x739d0383);
    for (let c0 = 0x21; c0 < 0x7f; c0++) {
        for (let c1 = 0x21; c1 < 0x7f; c1++) {
            const h2 = Math.imul((Math.imul((h0 ^ c0) >>> 0, P) ^ c1) >>> 0, P) & 0xffff;
            if ((h2 >> 8) !== (g >> 8)) continue;
            const c2 = (h2 ^ g) & 0xff;
            if (c2 < 0x21 || c2 > 0x7e) continue;
            return prefix + String.fromCharCode(c0, c1, c2);
        }
    }
    return null;
}

const COUNT = 10000;
const crafted = [];
for (let i = 0; crafted.length < COUNT; i++) {
    const k = collidingKey(i);
    if (k) crafted.push(k);
}
check(crafted.every((k) => (fnvLow32(k, 0x739d0383) & 0xffff) === TARGET), "every crafted key collides on the low 16 bits under FNV-1a");
const plain = Array.from({ length: COUNT }, (_, i) => "plain-key-" + i * 7919);

function cost(keys) {
    const rl = new RateLimiter({ tokensPerSec: 1000, burst: 1000, slots: 65536 });
    const t0 = cpuNow();
    for (let r = 0; r < 3; r++) for (const k of keys) rl.allow(k);
    const ms = cpuNow() - t0;
    check(rl.stats.live === keys.length, "every key has its own slot (" + rl.stats.live + ")");
    return ms;
}
cost(plain);
const plainMs = cost(plain);
const craftedMs = cost(crafted);
print("  plain " + plainMs.toFixed(1) + " ms, FNV-colliding " + craftedMs.toFixed(1) + " ms");
perfCheck(check, craftedMs < plainMs * 5 + 40, "keys built to collide under FNV-1a cost about what plain keys do (" +
    craftedMs.toFixed(1) + " ms vs " + plainMs.toFixed(1) + " ms)");

print("test_ratelimit_keyed: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_ratelimit_keyed: " + failures + " failures");
