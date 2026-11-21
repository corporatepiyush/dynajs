// covers: dyna:async.Queue
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_queue");
import { pmap, Pool, Channel, Queue, Semaphore, sleep, withTimeout, retry, debounce, throttle } from "dyna:async";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function sleepMs(ms) { return new Promise(res => setTimeout(res, ms)); }
function withTestTimeout(p, ms, what) {
    let timer;
    const deadline = new Promise((_, rej) => { timer = setTimeout(() => rej(new Error("TIMEOUT: " + what)), ms); });
    return Promise.race([p, deadline]).finally(() => clearTimeout(timer));
}
async function t(fn, ms) { await withTestTimeout(fn(), ms, fn.name); }

await __kitS.section("tryPush refuses at cap", async () => {
await t(async function queueSemantics() {
    const q = new Queue(2);
    q.push(1); q.push(2);
    eq(q.tryPush(3), false, "tryPush refuses at cap");
    let threw = false;
    try { q.push(3); } catch (e) { threw = true; }
    assert(threw, "push throws at cap (no silent drop)");
    eq(q.shift(), 1, "shift in FIFO order");
    eq(q.shift(), 2, "and shifts to empty");
    eq(q.shift(), undefined, "shift on empty is undefined");
    q.close();
    threw = false;
    try { q.push(4); } catch (e) { threw = true; }
    assert(threw, "push after close throws");
    eq(q.closed, true, "closed reports");
}, 5000);
});
await __kitS.section("FIFO order survives growth while the buffer is wrapped", async () => {
    const q = new Queue();
    const want = [];
    let next = 0, head = 0;
    for (let round = 0; round < 6; round++) {
        for (let i = 0; i < 3 + round * 5; i++) { q.push(next); want.push(next++); }
        for (let i = 0; i < 2 + round; i++) eq(q.shift(), want[head++], "shift in order (round " + round + ")");
    }
    while (q.length) eq(q.shift(), want[head++], "drain in order");
    eq(head, next, "every pushed value came back once");
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
