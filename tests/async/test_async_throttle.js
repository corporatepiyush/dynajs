// covers: dyna:async.throttle
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_throttle");
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

await __kitS.section("leading call immediate", async () => {
await t(async function throttleShape() {
    let calls = 0;
    const th = throttle(() => calls++, 60);
    th(); th(); th();
    eq(calls, 1, "leading call immediate");
    await sleepMs(90);
    assert(calls === 2, "exactly one trailing call (got " + calls + ")");
    await sleepMs(90);
    eq(calls, 2, "and nothing further");
}, 8000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
