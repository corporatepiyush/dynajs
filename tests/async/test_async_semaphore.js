// covers: dyna:async.Semaphore
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_semaphore");
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

await __kitS.section("semaphore jobs", async () => {
await t(async function semaphoreExclusivity() {
    const sem = new Semaphore(2);
    let live = 0, peak = 0;
    await sem.run(async () => { live++; if (live > peak) peak = live; await sleepMs(30); live--; });
    const jobs = [];
    for (let i = 0; i < 6; i++) jobs.push(sem.run(async () => { live++; if (live > peak) peak = live; await sleepMs(20); live--; }));
    await withTestTimeout(Promise.all(jobs), 5000, "semaphore jobs");
    eq(peak, 2, "at most n holders at once");
    eq(sem.available, 2, "all tokens returned");
}, 8000);
});

await __kitS.section("all tokens returned (n was 1)", async () => {
await t(async function semaphoreHandoff() {
    const sem = new Semaphore(1);
    const order = [];
    const first = sem.acquire();
    const second = sem.acquire();
    const third = sem.acquire();
    (await first)();
    (await second)();
    (await third)();
    eq(sem.available, 1, "all tokens returned (n was 1)");
    const r = await sem.run(async () => "ran");
    eq(r, "ran", "run() works with the token pool empty");
}, 5000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
