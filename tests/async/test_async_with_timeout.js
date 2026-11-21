// covers: dyna:async.withTimeout
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_with_timeout");
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

await __kitS.section("resolves under the deadline", async () => {
await t(async function withTimeoutBounds() {
    const t0 = Date.now();
    eq(await withTimeout(sleepMs(20).then(() => "fast"), 5000), "fast", "resolves under the deadline");
    let threw = false, dt = 0;
    const t1 = Date.now();
    try { await withTimeout(sleepMs(500), 30, "custom reason"); } catch (e) { threw = true; dt = Date.now() - t1; }
    assert(threw, "rejects over the deadline");
    // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
    assert(dt >= 25 && dt < 3000, "and roughly at the deadline (" + dt + " ms)");
    let arg = false;
    try { await withTimeout(sleepMs(500), 30); } catch (e) { arg = String(e.message).indexOf("timed out") >= 0; }
    assert(arg, "the default message names the timeout");
}, 8000);
});

await __kitS.section("late loser", async () => {
await t(async function withTimeoutLoserHandled() {
    const loser = sleepMs(80).then(() => { throw new Error("late loser"); });
    let threw = false;
    try { await withTimeout(loser, 10); } catch (e) { threw = true; }
    assert(threw, "the timer wins");
    await sleepMs(150);
}, 8000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
