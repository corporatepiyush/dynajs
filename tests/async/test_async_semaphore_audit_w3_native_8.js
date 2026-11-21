// covers: dyna:async.Semaphore
// Split from tests/test_audit_w3_native_8.js: the async sections.
import { sections as __kitSections } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
const __kitS = __kitSections("test_async_semaphore_audit_w3_native_8");
import { Path, makeTempDir, removeAll } from "dyna:file";
import { Semaphore } from "dyna:async";
import { cpuNow } from "../lib/cpuclock.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- C1-22: dyna:async queues -------------------------------------------
// ORACLE: handing a permit to the next waiter is a queue pop; its cost cannot
// depend on how many waiters remain. The original bug was a linear
// Array.prototype.shift queue. Each size is best of 3, and the bound is 8x: a
// 20x larger queue also costs cache misses, and at host load ~20 the healthy
// ratio reached 4.3x. Mutation proof: a loop over the waiters inside _release
// is red (5.7 us at 2k vs 96 us at 40k, 17x), and the restored code is green.
// CONTROL: waiters are served in FIFO order.

await __kitS.section("C1-22: semaphore hand-off cost is flat", async () => {
    const drain = async (n) => {
        const s = new Semaphore(1);
        const rel = [await s.acquire()];
        const seen = [];
        for (let i = 0; i < n; i++) s.acquire().then((r) => { seen.push(i); rel.push(r); });
        const t = cpuNow();
        for (let i = 0; i <= n; i++) { rel[i](); await null; await null; }
        return { per: (cpuNow() - t) / n, seen };
    };
    await drain(500);
    const best = async (n) => {
        let r = await drain(n);
        for (let k = 0; k < 2; k++) {
            const x = await drain(n);
            if (x.per < r.per) r = x;
        }
        return r;
    };
    const a = await best(2000), b = await best(40000);
    perfCheck(ok, b.per < a.per * 8, "C1-22: semaphore hand-off cost is flat (" + (a.per * 1e6).toFixed(0) + " ns at 2k, " + (b.per * 1e6).toFixed(0) + " ns at 40k)");
    ok(a.seen.length === 2000 && a.seen.every((v, i) => v === i), "control: waiters are served first come, first served");
});
__kitS.end();
removeAll(new Path(String(T)));
console.log("test_async_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_async_audit_w3_native_8: " + fail + " failures");
