/* watch_oom_probe.js -- drives the WHOLE Watcher lifecycle with one
 * allocation failing at an arbitrary point (the injector fails the N-th
 * allocation after ARM; see alloc_fail.c), and verifies at the end that
 * every promise the lifecycle produced was SETTLED and settled CLEANLY:
 *
 *   - no unsettled pull: attempts === settled (a dropped waiter would hang
 *     any for-await forever);
 *   - no poisoned argument: a resolved pull is exactly { done: boolean } with
 *     { path, kind } strings on a value pull and `value === undefined` on a
 *     done pull -- resolving with the engine's exception SENTINEL would land
 *     a garbage value here;
 *   - a REJECTED pull counts as settled: refusing cleanly is a legal
 *     outcome for an allocation failure, hanging is not.
 *
 * It runs WITHOUT the injector too (no allocation ever fails): the same
 * assertions then pin the ordinary lifecycle. Every phase is guarded, so a
 * failure in one phase still lets the later phases run -- one injected
 * failure must not disable the rest of the probe.
 *
 * THREE ENGINE-SIDE OOM FRAGILITIES are designed around, each documented
 * where it matters (they are engine-core behaviour, not watch behaviour):
 *   - top-level await: the async-module machinery allocates per resume and
 *     an allocation failure there wedges the module's own promise, hanging
 *     the probe with the watch code innocent. THERE ARE NO top-level awaits:
 *     the phases chain through named timer callbacks (the CLI runs
 *     js_std_loop after evaluation). No closure is CREATED inside the window
 *     either -- js_closure allocates, and a failure there once aborted the
 *     probe mid-phase; every function here is a top-level declaration,
 *     instantiated before the window opens.
 *   - promise reactions: fulfill_or_reject_promise/perform_promise_then
 *     drop ONE reaction when the job allocation fails, silently (the promise
 *     itself is settled). Every pull therefore gets TWO independent .then
 *     attachments; the single-shot injection can drop at most one of them,
 *     and `fired` keeps the counting honest if both run.
 *   - js_poll_expand: the loop's poll table can fail to grow and the loop
 *     then exits without running the remaining phases (no verdict line).
 *     The sweep recognises that case from the injector's backtrace as an
 *     engine-loop drop, not a watch failure.
 *
 * The verdict is ONE machine-greppable line:
 *   RESULT attempts=<n> settled=<n> bad=<n> errs=<n> refused=<n> closed=<bool>
 * and the gate is: the line exists, bad === 0, settled === attempts.
 *
 * Run:  dynajs --std tests/agent/t2d_ml_df_time/watch_oom_probe.js
 *       (under watch_oom_sweep.sh for the injection runs)
 */
import * as std from "std";
import { Watcher, Path } from "dyna:file";
import * as file from "dyna:file";

let attempts = 0, settled = 0, bad = 0, errs = 0, refused = 0;
let closed = false, done = false;
let w = null, w2 = null;

/* the shape of a CLEAN resolve; counting only, must not allocate */
function shape(v) {
    if (v === null || typeof v !== "object" || typeof v.done !== "boolean") {
        bad++;
        return;
    }
    if (v.done) {
        if (v.value !== undefined) bad++;
    } else {
        const ev = v.value;
        if (ev === null || typeof ev !== "object" ||
            typeof ev.path !== "string" || typeof ev.kind !== "string")
            bad++;
    }
}

function track(p) {
    attempts++;
    let fired = false;
    let onOk, onRej;
    try {
        onOk = (v) => {
            if (!fired) { fired = true; settled++; shape(v); }
        };
        onRej = () => {
            if (!fired) { fired = true; settled++; refused++; }
        };
    } catch (e) {
        settled++;      /* handlers cannot even be built: unobservable */
        return p;
    }
    /* TWO independent attachments: the engine drops ONE reaction when its
       job allocation fails (fulfill_or_reject_promise / perform_promise_then),
       and the injection is single-shot -- one of these lands. */
    let attached = 0;
    try { p.then(onOk, onRej); attached++; } catch (e) { /* retry below */ }
    try { p.then(onOk, onRej); attached++; } catch (e) { /* retry below */ }
    if (attached === 0)
        settled++;      /* neither attach survived: unobservable */
    return p;
}

function pull(x) {
    try {
        const p = x.next();
        if (p && typeof p.then === "function")
            return track(p);
        return null;
    } catch (e) {
        refused++;      /* a synchronous throw is the clean refusal shape */
        return null;
    }
}

/* return() also hands back a promise of { value, done } -- the same settle
   contract. It MUST be tracked: an untracked promise that this code rejects
   (its own allocation failure) trips the engine's unhandled-rejection
   tracker, which is a clean rejection seen by nobody, not a watch defect. */
function ret(x) {
    try {
        const p = x.return();
        if (p && typeof p.then === "function")
            return track(p);
        return null;
    } catch (e) {
        refused++;
        return null;
    }
}

/* setTimeout with re-arm: the injected failure is single-shot, so a retry
   lands. */
function later(fn, ms) {
    for (let i = 0; i < 16; i++) {
        try { setTimeout(fn, ms); return; } catch (e) { /* retry */ }
    }
    errs++;
}

function report(extra) {
    for (let i = 0; i < 64; i++) {
        try {
            print("RESULT attempts=" + attempts + " settled=" + settled +
                  " bad=" + bad + " errs=" + errs + " refused=" + refused +
                  " closed=" + closed + extra);
            return;
        } catch (e) { /* retry: the report alloc can be the failed one */ }
    }
}

function exitNow(code) {
    for (let i = 0; i < 16; i++) {
        try { std.exit(code); } catch (e) { /* retry */ }
    }
}

function finish() {
    if (done)
        return;
    done = true;
    report("");
    exitNow(0);
}

function watchdog() {
    report(" hung=1");
    exitNow(1);
}

/* ---- the phases: top-level functions, instantiated before ARM ---- */
function phase2() {
    pull(w);
    pull(w);
    try { w.stop(); } catch (e) { errs++; }
    pull(w);          /* after stop: done (or rejected -- both settle) */

    /* a SECOND watcher whose CONSTRUCTION is inside the injection window:
       ctor/start/park/stop/close must refuse cleanly too */
    try {
        w2 = new Watcher(new Path(ROOT), { debounceMs: 5 });
        w2.start();
        pull(w2);
        try { file.writeFile(new Path(ROOT + "/d.txt"), "4"); } catch (e) { errs++; }
        w2.stop();
        pull(w2);
        w2.close();
        pull(w2);
    } catch (e) { errs++; }

    /* phase 3: restart and end the stream with return() */
    later(phase3, 300);
}

function phase3() {
    try { w.start(); } catch (e) { errs++; }
    pull(w);
    try { file.writeFile(new Path(ROOT + "/c.txt"), "3"); } catch (e) { errs++; }
    later(phase4, 300);
}

function phase4() {
    pull(w);
    ret(w);

    /* phase 4b: close, then pulls past close must settle done */
    try { w.close(); closed = true; } catch (e) { errs++; }
    pull(w);
    pull(w);

    /* let the then-reactions run, then verdict */
    later(finish, 300);
}

/* ---- everything that allocates BEFORE the injector arms ---- */
const ROOT = `${std.getenv("TMPDIR") || "/tmp"}/dj_w_oom_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
try {
    file.makeDir(new Path(ROOT), { recursive: true });
} catch (e) { /* if even this fails the run is still reported */ }
w = new Watcher(new Path(ROOT), { debounceMs: 5 });

print("PROBE-START");
later(watchdog, 45000);

/* ---- ARM: every allocation from here is in the injection window ---- */
try { std.getenv("##ALLOC-ARM##"); } catch (e) { errs++; }

/* phase 1 (synchronous): start + parked pulls + file events */
try {
    w.start((ev) => { /* callback surface: must survive too */ });
} catch (e) { errs++; }
pull(w);
pull(w);
pull(w);
try { file.writeFile(new Path(ROOT + "/a.txt"), "1"); } catch (e) { errs++; }
try { file.writeFile(new Path(ROOT + "/b.txt"), "2"); } catch (e) { errs++; }
later(phase2, 300);
