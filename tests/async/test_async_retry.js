// covers: dyna:async.retry
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_retry");
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

await __kitS.section("always fails", async () => {
await t(async function retryBackoffShape() {
    const waits = [];
    let last = Date.now(), calls = 0, lastError = null;
    try {
        await retry(async () => { calls++; const now = Date.now(); waits.push(now - last); last = now; throw new Error("always fails " + calls); },
                    { retries: 3, backoffMs: 20, factor: 2, onRetry: (e, i) => { if (i === 99) throw new Error("unused"); } });
    } catch (e) { lastError = e; }
    eq(calls, 4, "1 initial + 3 retries");
    assert(lastError !== null && /always fails 4/.test(lastError.message), "the LAST error surfaces");
    assert(waits[1] >= 15 && waits[1] < 200, "second wait ~backoffMs (got " + waits[1] + ")");
    assert(waits[2] >= 35 && waits[2] < 400, "third wait ~backoffMs*2 (got " + waits[2] + ")");
}, 8000);
});

await __kitS.section("not yet", async () => {
await t(async function retryEventualSuccess() {
    let calls = 0;
    const r = await retry(async () => { calls++; if (calls < 3) throw new Error("not yet"); return "ok" + calls; }, { retries: 5, backoffMs: 1 });
    eq(r, "ok3", "eventual success returns the value");
    eq(calls, 3, "and stopped retrying at success");
}, 5000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
