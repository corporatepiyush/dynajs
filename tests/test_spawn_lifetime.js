// flags: --std
/* test_spawn_lifetime.js — Spawn's reference discipline under a collector.
 * The failure classes this file exists for:
 *   - a parked operation whose state lives in C must survive the JS heap
 *     dropping every handle (the frame clears `p` at last use), so an
 *     awaited read()/wait() never hangs and never reads freed memory;
 *   - several GC objects marking one C-pinned value underflows the
 *     collector's trial refcounts (gc_decref_child abort), so the Spawn
 *     object is the ONLY marker;
 *   - a park must not leak the struct: the last reference tears the child
 *     down, and close() is prompt and real even with views outstanding.
 * Every probe here drops the handle it started from on purpose.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_spawn_lifetime.js */

import { Spawn } from "dyna:sys";
import * as std from "std";

let n = 0, fails = 0;
function eq(a, b, m) {
    n++;
    if (!Object.is(a, b)) { fails++; print("FAIL: " + m + " (got " + a + ", want " + b + ")"); }
}
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function assert(c, m) { n++; if (!c) { fails++; throw new Error("assert: " + m); } }
const sleepMs = (ms) => new Promise((r) => setTimeout(r, ms));
function withTimeout(p, ms, what) {
    return new Promise((res, rej) => {
        const to = setTimeout(() => rej(new Error("timeout: " + what)), ms);
        p.then((v) => { clearTimeout(to); res(v); }, (e) => { clearTimeout(to); rej(e); });
    });
}
async function readAll(src) {
    const chunks = [];
    for (;;) {
        const b = new Uint8Array(4096);
        const k = await src.read(b);
        if (k === 0) break;
        chunks.push(b.subarray(0, k));
    }
    let total = 0;
    for (const c of chunks) total += c.length;
    const out = new Uint8Array(total);
    let o = 0;
    for (const c of chunks) { out.set(c, o); o += c.length; }
    return new TextDecoder().decode(out);
}
function collect() { if (std && std.gc) std.gc(); }

/* A fresh collector cycle needs several live objects (one or two never
 * trigger one), which is why each probe below runs a burst, not a single
 * case, and forces a collection while the parks are outstanding. */
await (async function droppedSpawnParkedWaitStillSettles() {
    const jobs = [];
    for (let i = 0; i < 8; i++) {
        let p = new Spawn("sh", ["-c", "sleep 0.15; exit " + i]);
        const w = p.wait();
        p = null;                       /* drop the only handle */
        jobs.push(w);
    }
    collect();                          /* collect with 8 parks outstanding */
    const rs = await withTimeout(Promise.all(jobs), 15000, "dropped awaited waits");
    for (let i = 0; i < 8; i++) eq(rs[i].code, i, "dropped spawn's wait() settled with the real code " + i);
})();

await (async function droppedSpawnParkedReadStillSettles() {
    const jobs = [];
    for (let i = 0; i < 8; i++) {
        let p = new Spawn("sh", ["-c", "sleep 0.1; echo r-" + i]);
        const r = readAll(p.stdout);
        p = null;
        jobs.push(r);
    }
    collect();
    const outs = await withTimeout(Promise.all(jobs), 15000, "dropped awaited reads");
    for (let i = 0; i < 8; i++) eq(outs[i].trim(), "r-" + i, "dropped spawn's read() delivered " + i);
})();

await (async function viewHeldAfterObjectDropped() {
    let p = new Spawn("sh", ["-c", "echo via-view"]);
    const view = p.stdout;              /* the only surviving handle */
    p = null;
    collect();
    const out = await withTimeout(readAll(view), 10000, "view read");
    eq(out.trim(), "via-view", "a stdio view alone keeps the pipe readable");
})();

await (async function thirtyTwoConcurrentDroppedReads() {
    /* the reviewer's m7 at 2.7x its original concurrency: one marker per
     * pass, no per-view marks, C refcounts hold the pins */
    const jobs = Array.from({ length: 32 }, (_, i) => (async () => {
        let p = new Spawn("sh", ["-c", "printf 'j-" + i + "x'"]);
        const o = await readAll(p.stdout);
        const w = p.wait();
        p = null;
        const r = await w;
        return o.trim() + "/" + r.code;
    })());
    collect();
    const outs = await withTimeout(Promise.all(jobs), 25000, "32 dropped reads");
    for (let i = 0; i < 32; i++) eq(outs[i], "j-" + i + "x/0", "32-job result " + i);
})();

await (async function closeIsPromptWithParkedWaitAndViews() {
    const p = new Spawn("sh", ["-c", "trap '' TERM; sleep 30"]);
    const buf = new Uint8Array(64);
    const r = p.stdout.read(buf);       /* parked read + live view */
    r.catch(() => {});
    const w = p.wait();                 /* parked wait */
    w.catch(() => {});
    await sleepMs(60);
    const t0 = Date.now();
    p.close();
    const dt = Date.now() - t0;
    ok(dt < 3000, "close() with a parked wait is prompt (got " + dt + " ms)");
    const res = await withTimeout(w, 5000, "wait settles on close");
    eq(res.signal, "SIGKILL", "parked wait() reports the SIGKILL close() delivered");
    let threw = false;
    try { await r; } catch (e) { threw = /close/.test(e.message); }
    ok(threw, "the parked read rejects on close()");
})();

await (async function noZombieAmongOurPidsAfterChurn() {
    /* 120 spawn/close cycles: a leaked struct or a missed reap shows up as a
     * zombie among OUR pids, never as a global count (siblings run beside). */
    const pids = [];
    for (let i = 0; i < 120; i++) {
        const p = new Spawn("true");
        pids.push(p.pid);
        p.close();
    }
    await sleepMs(400);
    const chk = new Spawn("/bin/sh", ["-c",
        "for p in " + pids.join(" ") + "; do ps -p $p -o stat= 2>/dev/null | grep -q Z && echo $p; done; true"]);
    const z = (await readAll(chk.stdout)).trim();
    await chk.wait();
    eq(z, "", "no zombie among the 120 closed pids");
    eq(pids.length, 120, "all 120 spawns got a pid");
})();

await (async function fdBudgetSurvivesChurn() {
    /* A descriptor leak is invisible at the default limit; under a lowered
     * one it is fatal. The limit is set by the RUNNER (see the Makefile's
     * spawn-lifetime target); here the cycle count is what would exhaust it. */
    for (let i = 0; i < 60; i++) {
        const p = new Spawn("sh", ["-c", "echo x"]);
        const out = await readAll(p.stdout);
        eq(out.trim(), "x", "churn read " + i);
        await p.wait();
    }
})();

await (async function stdinFlushSurvivesDroppedObject() {
    let p = new Spawn("cat", [], { stdin: "pipe" });
    const w = p.stdin.write("hello");
    const f = p.stdin.flush();
    const view = p.stdin;
    const o = readAll(p.stdout);
    p = null;                           /* the views above are the handles */
    collect();
    eq(await withTimeout(w, 5000, "write settles"), 5, "write() accepted 5 bytes");
    await withTimeout(f, 5000, "flush settles");
    view.close();                       /* EOF FIRST: cat echoes, then exits */
    eq((await withTimeout(o, 10000, "cat echoes")).trim(), "hello", "the child received the bytes");
})();

print("test_spawn_lifetime: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_spawn_lifetime failed");
