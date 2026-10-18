// flags: --std
// Split from tests/test_audit_w3_native_8.js: the async sections.
import { Path, makeTempDir, removeAll } from "dyna:file";
import { Semaphore } from "dyna:async";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- C1-22: dyna:async queues -------------------------------------------
// ORACLE: handing a permit to the next waiter is a queue pop; its cost cannot
// depend on how many waiters remain. Array.prototype.shift is linear in this
// engine (1.4 us at 10k elements, 15 us at 100k), which made it so.
// CONTROL: waiters are served in FIFO order.
{
    const drain = async (n) => {
        const s = new Semaphore(1);
        const rel = [await s.acquire()];
        const seen = [];
        for (let i = 0; i < n; i++) s.acquire().then((r) => { seen.push(i); rel.push(r); });
        const t = performance.now();
        for (let i = 0; i <= n; i++) { rel[i](); await null; await null; }
        return { per: (performance.now() - t) / n, seen };
    };
    await drain(500);
    const a = await drain(2000), b = await drain(40000);
    ok(b.per < a.per * 4, "C1-22: semaphore hand-off cost is flat (" + (a.per * 1e6).toFixed(0) + " ns at 2k, " + (b.per * 1e6).toFixed(0) + " ns at 40k)");
    ok(a.seen.length === 2000 && a.seen.every((v, i) => v === i), "control: waiters are served first come, first served");
}
removeAll(new Path(String(T)));
console.log("test_async_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_async_audit_w3_native_8: " + fail + " failures");
