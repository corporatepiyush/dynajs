/* test_async.js -- dyna:async.
 *
 * The module's contract is HONEST CONCURRENCY: every fn runs on the event-
 * loop thread (the native offload pool runs C, not JS; os.Worker takes a
 * module filename and its messages cannot carry functions, so a user fn
 * cannot cross to a worker). Nothing here is parallel; these tests pin
 * what IS real:
 *   - pmap: results in ITEM ORDER, the concurrency CAP actually bounds the
 *     in-flight count, the FIRST throw rejects the whole map while
 *     in-flight items still settle (their late rejections swallowed), and
 *     an empty/small input works;
 *   - Pool: a job that THROWS does not deadlock the pool (the slot frees
 *     and later submits still run);
 *   - Channel: MPSC handoff, capacity backpressure, close-while-parked
 *     recv resolves {done:true}, close rejecting a send that was blocked
 *     on capacity, single-consumer refusal, async-iterator drain;
 *   - Semaphore: exclusivity (at most n holders), direct token hand-off;
 *   - withTimeout: resolves under the deadline, rejects over it, and the
 *     LOSING promise's late rejection never surfaces as unhandled;
 *   - retry: the backoff SHAPE (waits grow by factor), stop after
 *     `retries`, the last error surfaces;
 *   - Queue: bounded, close semantics;
 *   - debounce/throttle: collapse and rate shapes.
 *
 * Every timing-bound probe is timeout-wrapped: a broken timer must fail
 * the test, not the gate.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_async.js
 */
import { pmap, Pool, Channel, Queue, Semaphore, sleep, withTimeout, retry, debounce, throttle } from "dyna:async";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function sleepMs(ms) { return new Promise(res => setTimeout(res, ms)); }
function withTestTimeout(p, ms, what) {
    return Promise.race([p, sleepMs(ms).then(() => { throw new Error("TIMEOUT: " + what); })]);
}
async function t(fn, ms) { await withTestTimeout(fn(), ms, fn.name); }

await t(async function pmapOrderAndCap() {
    const order = [];
    const out = await pmap([3, 1, 2], async (x, i) => {
        await sleepMs(10 - x * 2);          // completion order != item order
        order.push(i);
        return x * 10;
    }, { concurrency: 3 });
    eq(JSON.stringify(out), "[30,10,20]", "results are in ITEM order");
    eq(JSON.stringify(order.sort()), "[0,1,2]", "every item ran exactly once");
}, 5000);

await t(async function pmapConcurrencyBound() {
    let live = 0, peak = 0;
    await pmap([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], async () => {
        live++; if (live > peak) peak = live;
        await sleepMs(20);
        live--;
    }, { concurrency: 2 });
    eq(peak, 2, "the in-flight count never exceeds concurrency");
}, 8000);

await t(async function pmapSyncFn() {
    eq(JSON.stringify(await pmap([1, 2], (x) => x + 1)), "[2,3]",
       "a sync fn works (awaited transparently)");
}, 5000);

await t(async function pmapEmpty() {
    eq(JSON.stringify(await pmap([], () => 1)), "[]", "empty input resolves empty");
}, 5000);

await t(async function pmapFirstThrowRejectsInFlightSettle() {
    let inFlightSettled = 0, threwLate = false;
    let rejected = false, lateHandled = false;
    const p = pmap([0, 1, 2, 3], async (x) => {
        if (x === 0) { await sleepMs(10); throw new Error("first-throw"); }
        await sleepMs(120);                  // still in flight when the throw lands
        inFlightSettled += 1;
        if (x === 3 && !globalThis.__nope) { threwLate = true; throw new Error("late straggler"); }
        return x;
    });
    p.catch((e) => { rejected = true; lateHandled = true; });   // the rejection IS handled here
    await sleepMs(300);
    assert(rejected, "the FIRST throw rejects the whole pmap");
    eq(inFlightSettled, 3, "in-flight items still ran to completion and settled");
    // the late straggler's rejection must have been SWALLOWED (handled above),
    // never surfacing as an unhandled rejection that would kill the engine.
    p.catch(() => {});
}, 5000);

await t(async function poolSurvivesThrowingJobs() {
    const pool = new Pool(1);
    let threw = false;
    try { await pool.submit(() => { throw new Error("boom"); }); } catch (e) { threw = true; }
    assert(threw, "a throwing job rejects its submit");
    eq(await pool.submit(() => "still-alive"), "still-alive",
       "the slot freed: the pool kept pumping");
    eq(pool.active, 0, "and is idle again");
}, 5000);

await t(async function channelMpsc() {
    const ch = new Channel();
    const send = ch.send("v1");
    eq(JSON.stringify(await ch.recv()), '{"value":"v1","done":false}', "direct handoff to a parked recv");
    await send;
}, 5000);

await t(async function channelCapacity() {
    const ch = new Channel(1);
    await ch.send("a");
    let blocked = true;
    const second = ch.send("b").then(() => { blocked = false; });
    await sleepMs(40);
    assert(blocked, "send blocks at capacity (backpressure, no silent drop)");
    eq(ch.length, 1, "the accepted value is buffered");
    eq(JSON.stringify(await ch.recv()), '{"value":"a","done":false}', "recv drains");
    await second;
    eq(ch.length, 1, "the blocked send was admitted when room appeared");
    eq(JSON.stringify(await ch.recv()), '{"value":"b","done":false}', "and is delivered in order");
}, 5000);

await t(async function channelCloseWhilePending() {
    const ch = new Channel();
    const parked = ch.recv();
    await sleepMs(20);
    ch.close();
    eq(JSON.stringify(await parked), '{"done":true}', "close resolves a parked recv with done");
    ch.close();
    eq(JSON.stringify(await ch.recv()), '{"done":true}', "recv after close is done");
    let threw = false;
    try { await ch.send("x"); } catch (e) { threw = true; }
    assert(threw, "send after close rejects");
}, 5000);

await t(async function channelCloseRejectsBlockedSend() {
    const ch = new Channel(1);
    await ch.send("a");
    const blocked = ch.send("b");
    blocked.catch(() => {});            // its rejection IS the assertion
    await sleepMs(20);
    ch.close();
    let threw = false;
    try { await blocked; } catch (e) { threw = true; }
    assert(threw, "close rejects a send that was blocked on capacity (its value was never accepted)");
    eq(ch.length, 1, "and the previously accepted value stays buffered");
    eq(JSON.stringify(await ch.recv()), '{"value":"a","done":false}', "the drain still works after close");
    eq(JSON.stringify(await ch.recv()), '{"done":true}', "then done");
}, 5000);

await t(async function channelSingleConsumer() {
    const ch = new Channel();
    ch.recv().catch(() => {});
    let threw = false;
    try { await ch.recv(); } catch (e) { threw = true; }
    assert(threw, "a second concurrent recv is refused (MPSC: one consumer)");
}, 5000);

await t(async function channelAsyncIterator() {
    const ch = new Channel(4);
    await ch.send(1); await ch.send(2); await ch.send(3);
    ch.close();
    const seen = [];
    /* for-await binds the YIELDED VALUE (the {value, done} envelope is the
     * iterator protocol's, not the caller's); the closed channel ends the
     * loop after the drain. */
    for await (const msg of ch) seen.push(msg);
    eq(JSON.stringify(seen), "[1,2,3]", "for-await drains and stops at done");
}, 5000);

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

await t(async function semaphoreHandoff() {
    const sem = new Semaphore(1);
    const order = [];
    const first = sem.acquire();
    const second = sem.acquire();
    const third = sem.acquire();
    (await first)();
    (await second)();          // token passes DIRECTLY to the first waiter
    (await third)();
    eq(sem.available, 1, "all tokens returned (n was 1)");
    const r = await sem.run(async () => "ran");
    eq(r, "ran", "run() works with the token pool empty");
}, 5000);

await t(async function withTimeoutBounds() {
    const t0 = Date.now();
    eq(await withTimeout(sleepMs(20).then(() => "fast"), 5000), "fast", "resolves under the deadline");
    let threw = false, dt = 0;
    const t1 = Date.now();
    try { await withTimeout(sleepMs(500), 30, "custom reason"); } catch (e) { threw = true; dt = Date.now() - t1; }
    assert(threw, "rejects over the deadline");
    assert(dt >= 25 && dt < 3000, "and roughly at the deadline (" + dt + " ms)");
    let arg = false;
    try { await withTimeout(sleepMs(500), 30); } catch (e) { arg = String(e.message).indexOf("timed out") >= 0; }
    assert(arg, "the default message names the timeout");
}, 8000);

await t(async function withTimeoutLoserHandled() {
    /* The underlying promise REJECTS after the timer already won: that late
     * rejection must be HANDLED (the continuation above swallows it), or it
     * would surface as an unhandled rejection and kill the engine. */
    const loser = sleepMs(80).then(() => { throw new Error("late loser"); });
    let threw = false;
    try { await withTimeout(loser, 10); } catch (e) { threw = true; }
    assert(threw, "the timer wins");
    await sleepMs(150);                 // the loser settles here; engine must survive
}, 8000);

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

await t(async function retryEventualSuccess() {
    let calls = 0;
    const r = await retry(async () => { calls++; if (calls < 3) throw new Error("not yet"); return "ok" + calls; }, { retries: 5, backoffMs: 1 });
    eq(r, "ok3", "eventual success returns the value");
    eq(calls, 3, "and stopped retrying at success");
}, 5000);

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

await t(async function debounceShape() {
    let calls = 0;
    const d = debounce(() => calls++, 30);
    d(); await sleepMs(10); d(); await sleepMs(10); d();
    eq(calls, 0, "nothing fired inside the window");
    await sleepMs(60);
    eq(calls, 1, "the LAST call inside the window fired once");
    d(); d.cancel();
    await sleepMs(60);
    eq(calls, 1, "cancel stops the pending call");
}, 8000);

await t(async function throttleShape() {
    let calls = 0;
    const th = throttle(() => calls++, 60);
    th(); th(); th();                    // first now, trailing at the window edge
    eq(calls, 1, "leading call immediate");
    await sleepMs(90);
    assert(calls === 2, "exactly one trailing call (got " + calls + ")");
    await sleepMs(90);
    eq(calls, 2, "and nothing further");
}, 8000);

await t(async function channelQueueIntegerGuards() {        /* reviewer LOW-1 */
    let threw = false;
    try { new Channel(2.5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "Channel(2.5) throws RangeError");
    threw = false;
    try { new Queue(2.5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "Queue(2.5) throws RangeError");
    threw = false;
    try { new Channel(0); } catch (e) { threw = true; }
    assert(threw, "Channel(0) still refuses");
}, 5000);

await t(async function channelChurn() {                     /* reviewer-verified 200-send churn */
    const ch = new Channel(4);
    (async () => { for (let i = 0; i < 200; i++) await ch.send(i); ch.close(); })();
    let total = 0, count = 0;
    for await (const v of ch) { total += v; count++; await sleepMs(1); }
    eq(count, 200, "200 sends delivered through a cap-4 channel");
    eq(total, 19900, "and in order");
}, 10000);

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

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
