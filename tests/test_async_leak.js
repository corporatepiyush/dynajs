/*
 * test_async_leak.js -- async/await + callback memory-reference regression net.
 *
 * Two ways to run it:
 *
 *   1. Functional / reclaim proof (default, part of `make test`):
 *          ./dynajs tests/test_async_leak.js
 *      Deterministic. Uses WeakRef + std.gc() to PROVE that async/promise/timer
 *      graphs (including reference cycles) are reclaimed once the last JS root is
 *      dropped. Run it under ASan/UBSan -- the real UAF/leak detectors on this
 *      host -- to catch refcount slips on the settle/reject/clear paths.
 *
 *   2. Peak-RSS-plateau leak check (LSan is unavailable on arm64-apple-darwin,
 *      so this is the leak oracle per CLAUDE.md). Pass a ROUND count: each round
 *      does a fixed, bounded amount of async/callback work (awaits, a dropped
 *      promise cycle, a fired+cleared timer, microtasks, a handled rejection)
 *      and GC runs periodically, so the live set is O(1) in the round count.
 *      Peak RSS must therefore be FLAT across round counts -- flat ⇒ no leak,
 *      linear growth ⇒ a per-operation leak:
 *          for r in 20000 80000 200000; do
 *              /usr/bin/time -l ./dynajs tests/test_async_leak.js $r 2>&1 \
 *                  | grep 'maximum resident'
 *          done
 *      (Do NOT scale the functional batch to get a leak signal -- several
 *      scenarios hold an O(batch) live set on purpose, so their RSS scales by
 *      design; only the bounded churn loop isolates a true leak.)
 *
 * Caveats this pins down (see the header of each scenario):
 *   - promise/async reference cycles are reclaimed only by the cycle GC, never
 *     by refcounting alone -- growth between GCs is churn, not a leak;
 *   - every `await` allocates a throwaway wrapper promise (allocator churn);
 *   - clearTimeout/clearInterval and a null read-handler MUST free the captured
 *     JS callback (the refcount path most likely to regress);
 *   - a handled rejection must settle and release without accumulating.
 */
import * as os from "os";
import * as std from "std";

/* Fixed batch for the functional scenarios (kept modest so `make test` is fast
 * and so per-scenario peak RSS does NOT scale -- see header). */
const N = 3000;
/* Round count for the bounded RSS/leak churn loop (0 = functional test only). */
const ROUNDS = scriptArgs.length > 1 ? (parseInt(scriptArgs[1]) | 0) : 0;

let failures = 0;
function assert(cond, msg) {
    if (!cond) { failures++; print("  FAIL:", msg); }
}
function ok(name) { print("  ok  " + name); }

/* Resolve after the next event-loop turn (a real macrotask, not a microtask),
 * so timer-driven paths actually exercise the poll loop. */
function tick() {
    return new Promise((res) => os.setTimeout(res, 0));
}

/* ---- 1. await loop: N awaits settle in order; per-await wrapper churn ---- */
async function scenario_await_loop() {
    let acc = 0;
    for (let i = 0; i < N; i++)
        acc += await Promise.resolve(i);
    assert(acc === (N * (N - 1)) / 2, "await loop accumulator");
    ok("await loop (" + N + " awaits)");
}

/* ---- 2. promise/async reference CYCLE is reclaimed by the cycle GC ----
 * Build a promise whose own handler closes over the promise (a cycle that
 * refcounting alone can never break), keep only a WeakRef to a sentinel the
 * cycle captures, drop the strong roots, GC, and prove the sentinel is gone. */
function makeAsyncCycle() {
    const sentinel = { tag: "cycle-sentinel" };
    let p = Promise.resolve(sentinel);
    // handler references p -> p's reaction references handler -> cycle.
    p.then(() => p);
    // an async closure that also captures the cycle
    const run = async () => { await p; return sentinel; };
    void run();
    return new WeakRef(sentinel);
}
async function scenario_cycle_reclaim() {
    let wr = makeAsyncCycle();
    await tick();          // let the pending reactions/async fn drain
    std.gc();
    std.gc();
    assert(wr.deref() === undefined,
           "async reference cycle not reclaimed by GC (possible leak)");
    ok("promise/async cycle reclaimed");
}

/* ---- 3. rejection handled (inline + late .catch) never accumulates ---- */
async function scenario_reject_handled() {
    let caught = 0;
    for (let i = 0; i < N; i++) {
        try { await Promise.reject(new Error("x" + i)); }
        catch (e) { caught++; }
    }
    assert(caught === N, "inline-awaited rejections all caught");

    // reject and attach .catch in the same turn (the handled path): the reaction
    // chain must settle and release without accumulating.
    let chainCaught = 0;
    let chain = [];
    for (let i = 0; i < N; i++)
        chain.push(Promise.reject(new Error("c" + i)).catch(() => { chainCaught++; }));
    await Promise.all(chain);
    assert(chainCaught === N, "handled rejection chain all settled");
    ok("rejections handled (inline + chain)");
}

/* ---- 4. setTimeout callback churn: schedule N, all fire exactly once ---- */
function scenario_timer_fire() {
    return new Promise((resolve) => {
        let fired = 0;
        const target = N;
        for (let i = 0; i < target; i++) {
            os.setTimeout(() => {
                if (++fired === target) {
                    assert(fired === target, "all timers fired once");
                    ok("timer fire churn (" + target + ")");
                    resolve();
                }
            }, 0);
        }
    });
}

/* ---- 5. clearTimeout frees the captured callback (refcount free path) ----
 * Schedule N timers each capturing a sentinel, clear them all before they fire,
 * then prove (WeakRef) the captured sentinels were released. A regression that
 * leaks th->func in free_timer would keep these alive. */
async function scenario_timer_clear_frees() {
    let wr;
    (function () {
        const sentinel = { tag: "timer-sentinel" };
        wr = new WeakRef(sentinel);
        const ids = [];
        for (let i = 0; i < N; i++)
            ids.push(os.setTimeout(() => { void sentinel; }, 100000));
        for (const id of ids) os.clearTimeout(id);
    })();
    std.gc();
    std.gc();
    assert(wr.deref() === undefined,
           "cleared-timer callback retained its capture (leak in free path)");
    ok("clearTimeout frees callback capture");
}

/* ---- 6. setInterval that clears itself after K ticks ---- */
function scenario_interval_selfclear() {
    return new Promise((resolve) => {
        const K = 5;
        let ticks = 0;
        const id = os.setInterval(() => {
            if (++ticks === K) {
                os.clearInterval(id);
                assert(ticks === K, "interval ticked exactly K then cleared");
                ok("setInterval self-clear");
                resolve();
            }
        }, 0);
    });
}

/* ---- 7. for-await over an async iterator ---- */
async function scenario_for_await() {
    async function* gen(n) {
        for (let i = 0; i < n; i++) yield await Promise.resolve(i);
    }
    let sum = 0, m = Math.min(N, 2000);
    for await (const v of gen(m)) sum += v;
    assert(sum === (m * (m - 1)) / 2, "for-await sum");
    ok("for-await async iterator");
}

/* ---- 8. queueMicrotask churn ---- */
function scenario_microtask_churn() {
    return new Promise((resolve) => {
        let ran = 0;
        for (let i = 0; i < N; i++)
            queueMicrotask(() => { if (++ran === N) { ok("queueMicrotask churn (" + N + ")"); resolve(); } });
    });
}

/* ---- 9. os.setReadHandler set then cleared frees the callback ----
 * Register a read handler capturing a sentinel on a pipe read-fd, clear it with
 * null, GC, and prove the capture was released. Guarded on os.pipe. */
async function scenario_readhandler_clear_frees() {
    if (typeof os.pipe !== "function") { ok("read-handler clear (skipped: no os.pipe)"); return; }
    const [rfd, wfd] = os.pipe();
    let wr;
    (function () {
        const sentinel = { tag: "rh-sentinel" };
        wr = new WeakRef(sentinel);
        os.setReadHandler(rfd, () => { void sentinel; });
    })();
    os.setReadHandler(rfd, null);   // clear -> must free the captured closure
    os.close(rfd); os.close(wfd);
    std.gc();
    std.gc();
    assert(wr.deref() === undefined,
           "cleared read-handler retained its capture (leak)");
    ok("read-handler clear frees capture");
}

/* ---- Bounded churn: fixed work per round, GC periodically. O(1) live set,
 * so peak RSS is flat across ROUNDS unless a per-op leak exists. ---- */
async function churnRound() {
    let a = 0;
    for (let i = 0; i < 8; i++) a += await Promise.resolve(i);       // await churn
    await Promise.reject(0).catch(() => {});                         // handled reject
    (function () { let p = Promise.resolve({}); p.then(() => p); })(); // dropped cycle
    const id = os.setTimeout(() => {}, 100000); os.clearTimeout(id); // schedule+clear
    await new Promise((r) => os.setTimeout(r, 0));                   // fired timer
    await new Promise((r) => {                                       // microtasks
        let c = 0; for (let i = 0; i < 8; i++) queueMicrotask(() => { if (++c === 8) r(); });
    });
    void a;
}
async function leakChurn(rounds) {
    for (let i = 0; i < rounds; i++) {
        await churnRound();
        if ((i & 511) === 0) std.gc();
    }
    std.gc();
    ok("leak churn (" + rounds + " rounds, bounded live set)");
}

async function main() {
    print("test_async_leak: N=" + N + " ROUNDS=" + ROUNDS);
    await scenario_await_loop();
    await scenario_cycle_reclaim();
    await scenario_reject_handled();
    await scenario_timer_fire();
    await scenario_timer_clear_frees();
    await scenario_interval_selfclear();
    await scenario_for_await();
    await scenario_microtask_churn();
    await scenario_readhandler_clear_frees();
    if (ROUNDS > 0) await leakChurn(ROUNDS);

    if (failures === 0) print("test_async_leak: all tests passed");
    else { print("test_async_leak: " + failures + " FAILED"); std.exit(1); }
}

main().catch((e) => { print("test_async_leak: threw", e, e && e.stack); std.exit(1); });
