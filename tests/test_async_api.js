/*
 * test_async_api.js -- capability + contract probe for the async/callback/GC
 * surface. Fast, self-checking; part of `make test`. Grown out of the ad-hoc
 * probes used while auditing async memory behavior, kept in-repo so a future
 * change that drops or breaks one of these primitives fails loudly here (the
 * heavier reclaim/leak stress lives in test_async_leak.js).
 */
import * as os from "os";
import * as std from "std";

let failures = 0;
function check(cond, msg) { if (!cond) { failures++; print("  FAIL:", msg); } }
function has(obj, name, kind) { check(typeof obj[name] === (kind || "function"), name + " is " + (kind || "function")); }

/* ---- 1. the primitives exist with the expected types ---- */
has(globalThis, "Promise");
has(globalThis, "queueMicrotask");
has(globalThis, "WeakRef");
has(globalThis, "FinalizationRegistry");
has(globalThis, "Symbol");
check(typeof Symbol.dispose === "symbol", "Symbol.dispose");
check(typeof Symbol.asyncDispose === "symbol", "Symbol.asyncDispose");
has(os, "setTimeout"); has(os, "clearTimeout");
has(os, "setInterval"); has(os, "clearInterval");
has(os, "setReadHandler"); has(os, "setWriteHandler");
has(std, "gc");

/* ---- 2. minimal behavioral contracts (not just presence) ---- */
async function main() {
    // Promise ordering: microtasks drain before the next macrotask.
    let order = [];
    await new Promise((resolve) => {
        os.setTimeout(() => { order.push("timeout"); resolve(); }, 0);
        queueMicrotask(() => order.push("micro"));
        Promise.resolve().then(() => order.push("then"));
    });
    check(order[0] === "micro" && order[1] === "then" && order[2] === "timeout",
          "microtasks run before the timer macrotask (got " + order.join(",") + ")");

    // clearTimeout actually prevents the callback from firing.
    let fired = false;
    const id = os.setTimeout(() => { fired = true; }, 0);
    os.clearTimeout(id);
    await new Promise((r) => os.setTimeout(r, 0));
    check(!fired, "clearTimeout prevents the callback");

    // async throw surfaces as a rejection catchable with try/await.
    let caught = null;
    try { await (async () => { throw new Error("boom"); })(); }
    catch (e) { caught = e.message; }
    check(caught === "boom", "async throw -> awaitable rejection");

    // WeakRef + std.gc reclaim an unreferenced object (drives the leak tests).
    let wr = (function () { return new WeakRef({ x: 1 }); })();
    std.gc(); std.gc();
    check(wr.deref() === undefined, "WeakRef target reclaimed after gc");

    // Promise.all / allSettled / race resolve as specified.
    check((await Promise.all([1, Promise.resolve(2)]))[1] === 2, "Promise.all");
    check((await Promise.race([Promise.resolve("a"), new Promise(() => {})])) === "a", "Promise.race");
    const settled = await Promise.allSettled([Promise.resolve(1), Promise.reject(2)]);
    check(settled[0].status === "fulfilled" && settled[1].status === "rejected", "Promise.allSettled");

    if (failures === 0) print("test_async_api: all tests passed");
    else { print("test_async_api: " + failures + " FAILED"); std.exit(1); }
}

main().catch((e) => { print("test_async_api: threw", e, e && e.stack); std.exit(1); });
