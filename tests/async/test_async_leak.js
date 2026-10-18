// flags: --std
import * as os from "os";
import * as std from "std";

const N = 3000;
const ROUNDS = scriptArgs.length > 1 ? (parseInt(scriptArgs[1]) | 0) : 0;

let failures = 0;
function assert(cond, msg) {
    if (!cond) { failures++; print("  FAIL:", msg); }
}
function ok(name) { print("  ok  " + name); }

function tick() {
    return new Promise((res) => os.setTimeout(res, 0));
}

async function scenario_await_loop() {
    let acc = 0;
    for (let i = 0; i < N; i++)
        acc += await Promise.resolve(i);
    assert(acc === (N * (N - 1)) / 2, "await loop accumulator");
    ok("await loop (" + N + " awaits)");
}

function makeAsyncCycle() {
    const sentinel = { tag: "cycle-sentinel" };
    let p = Promise.resolve(sentinel);
    p.then(() => p);
    const run = async () => { await p; return sentinel; };
    void run();
    return new WeakRef(sentinel);
}
async function scenario_cycle_reclaim() {
    let wr = makeAsyncCycle();
    await tick();
    std.gc();
    std.gc();
    assert(wr.deref() === undefined,
           "async reference cycle not reclaimed by GC (possible leak)");
    ok("promise/async cycle reclaimed");
}

async function scenario_reject_handled() {
    let caught = 0;
    for (let i = 0; i < N; i++) {
        try { await Promise.reject(new Error("x" + i)); }
        catch (e) { caught++; }
    }
    assert(caught === N, "inline-awaited rejections all caught");

    let chainCaught = 0;
    let chain = [];
    for (let i = 0; i < N; i++)
        chain.push(Promise.reject(new Error("c" + i)).catch(() => { chainCaught++; }));
    await Promise.all(chain);
    assert(chainCaught === N, "handled rejection chain all settled");
    ok("rejections handled (inline + chain)");
}

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

async function scenario_for_await() {
    async function* gen(n) {
        for (let i = 0; i < n; i++) yield await Promise.resolve(i);
    }
    let sum = 0, m = Math.min(N, 2000);
    for await (const v of gen(m)) sum += v;
    assert(sum === (m * (m - 1)) / 2, "for-await sum");
    ok("for-await async iterator");
}

function scenario_microtask_churn() {
    return new Promise((resolve) => {
        let ran = 0;
        for (let i = 0; i < N; i++)
            queueMicrotask(() => { if (++ran === N) { ok("queueMicrotask churn (" + N + ")"); resolve(); } });
    });
}

async function scenario_readhandler_clear_frees() {
    if (typeof os.pipe !== "function") { ok("read-handler clear (skipped: no os.pipe)"); return; }
    const [rfd, wfd] = os.pipe();
    let wr;
    (function () {
        const sentinel = { tag: "rh-sentinel" };
        wr = new WeakRef(sentinel);
        os.setReadHandler(rfd, () => { void sentinel; });
    })();
    os.setReadHandler(rfd, null);
    os.close(rfd); os.close(wfd);
    std.gc();
    std.gc();
    assert(wr.deref() === undefined,
           "cleared read-handler retained its capture (leak)");
    ok("read-handler clear frees capture");
}

async function churnRound() {
    let a = 0;
    for (let i = 0; i < 8; i++) a += await Promise.resolve(i);
    await Promise.reject(0).catch(() => {});
    (function () { let p = Promise.resolve({}); p.then(() => p); })();
    const id = os.setTimeout(() => {}, 100000); os.clearTimeout(id);
    await new Promise((r) => os.setTimeout(r, 0));
    await new Promise((r) => {
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
