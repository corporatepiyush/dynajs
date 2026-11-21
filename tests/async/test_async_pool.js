// covers: dyna:async.Pool
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_pool");
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

await __kitS.section("a throwing job rejects its submit", async () => {
await t(async function poolSurvivesThrowingJobs() {
    const pool = new Pool(1);
    let threw = false;
    try { await pool.submit(() => { throw new Error("boom"); }); } catch (e) { threw = true; }
    assert(threw, "a throwing job rejects its submit");
    eq(await pool.submit(() => "still-alive"), "still-alive",
       "the slot freed: the pool kept pumping");
    eq(pool.active, 0, "and is idle again");
}, 5000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
