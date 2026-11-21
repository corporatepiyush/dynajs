// covers: dyna:async.pmap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_pmap");
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

await t(async function pmapOrderAndCap() {
    const order = [];
    const out = await pmap([3, 1, 2], async (x, i) => {
        await sleepMs(10 - x * 2);
        order.push(i);
        return x * 10;
    }, { concurrency: 3 });
    eq(JSON.stringify(out), "[30,10,20]", "results are in ITEM order");
    eq(JSON.stringify(order.sort()), "[0,1,2]", "every item ran exactly once");
}, 5000);

await __kitS.section("the in-flight count never exceeds concurrency", async () => {
await t(async function pmapConcurrencyBound() {
    let live = 0, peak = 0;
    await pmap([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], async () => {
        live++; if (live > peak) peak = live;
        await sleepMs(20);
        live--;
    }, { concurrency: 2 });
    eq(peak, 2, "the in-flight count never exceeds concurrency");
}, 8000);
});

await __kitS.section("a sync fn works (awaited transparently)", async () => {
await t(async function pmapSyncFn() {
    eq(JSON.stringify(await pmap([1, 2], (x) => x + 1)), "[2,3]",
       "a sync fn works (awaited transparently)");
}, 5000);
});

await __kitS.section("empty input resolves empty", async () => {
await t(async function pmapEmpty() {
    eq(JSON.stringify(await pmap([], () => 1)), "[]", "empty input resolves empty");
}, 5000);
});

await t(async function pmapFirstThrowRejectsInFlightSettle() {
    let inFlightSettled = 0, threwLate = false;
    let rejected = false, lateHandled = false;
    const p = pmap([0, 1, 2, 3], async (x) => {
        if (x === 0) { await sleepMs(10); throw new Error("first-throw"); }
        await sleepMs(120);
        inFlightSettled += 1;
        if (x === 3 && !globalThis.__nope) { threwLate = true; throw new Error("late straggler"); }
        return x;
    });
    p.catch((e) => { rejected = true; lateHandled = true; });
    await sleepMs(300);
    assert(rejected, "the FIRST throw rejects the whole pmap");
    eq(inFlightSettled, 3, "in-flight items still ran to completion and settled");
    p.catch(() => {});
}, 5000);

await __kitS.section("pmap refuses non-arrays", async () => {
await t(async function argumentRefusals() {
    let threw = false;
    try { pmap("nope", (x) => x); } catch (e) { threw = true; }
    assert(threw, "pmap refuses non-arrays");
    threw = false;
    try { await pmap([1], (x) => x, { concurrency: 0 }); } catch (e) { threw = true; }
    assert(threw, "pmap refuses concurrency < 1");
    threw = false;
    try { new Semaphore(0); } catch (e) { threw = true; }
    assert(threw, "Semaphore refuses n < 1");
    threw = false;
    try { await withTimeout("x", 5); } catch (e) { threw = true; }
    assert(threw, "withTimeout refuses non-promises");
    threw = false;
    try { await sleep(-1); } catch (e) { threw = true; }
    assert(threw, "sleep refuses negative ms");
    threw = false;
    try { new Channel(0); } catch (e) { threw = true; }
    assert(threw, "Channel refuses cap < 1");
}, 5000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
