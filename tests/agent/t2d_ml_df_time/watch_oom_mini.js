/* watch_oom_mini.js -- the COMPACT settle-contract probe: same paths as
 * watch_oom_probe.js (event pump settling parked pulls, settle_done on
 * stop(), the done-pulls after stop and after close, the reject-on-OOM
 * settles) with a fraction of the allocation traffic -- so a mutation run
 * over the injection points finishes in minutes while still covering every
 * settle path the fix touched. The release gate is the FULL sweep over
 * watch_oom_probe.js; this probe is what the mutation rows run.
 *
 * THE PROBE'S OWN DRIVER CODE IS PART OF THE INJECTION TARGET, and the
 * failure mode is nastier than a lost call: a global function (like the
 * `pull`/`later`/`setTimeout` this file calls) and a module-namespace member
 * (std.exit, file.writeFile...) are instantiated LAZILY on their first read,
 * and that instantiation ALLOCATES. An injection aimed at it fails the read,
 * and the engine then leaves the name NOT A FUNCTION for good -- measured at
 * n=221: iteration 0 catches "InternalError: out of memory", every later
 * iteration catches "TypeError: not a function", so retrying the statement
 * cannot recover it and the probe dies with no RESULT line.
 *
 * The fix has two halves, both probe-side:
 *   1. PRIME the bindings below, BEFORE the arm marker. Their instantiation
 *      allocations then happen outside the injection window, and every later
 *      read is a plain property read that cannot fail.
 *   2. RETRY every step with the try wrapping the WHOLE statement (binding
 *      lookup, property lookup and call alike), because a method read off the
 *      Watcher allocates too. The injection fails exactly ONE allocation, so
 *      one retry gets through, and a step that already succeeded before the
 *      failing one is safe to repeat: pull() just asks for another result
 *      (counted), stop()/close() are idempotent, and schedule() only ever
 *      reaches its timer. */
import * as std from "std";
import { Watcher, Path } from "dyna:file";
import * as file from "dyna:file";

let attempts = 0, settled = 0, bad = 0, refused = 0;
let closed = false, done = false;

function shape(v) {
    if (v === null || typeof v !== "object" || typeof v.done !== "boolean")
        return false;
    if (v.done)
        return v.value === undefined;
    const ev = v.value;
    return ev !== null && typeof ev === "object" &&
           typeof ev.path === "string" && typeof ev.kind === "string";
}

function track(p) {
    attempts++;
    let fired = false;
    let onOk, onRej;
    try {
        onOk = (v) => { if (!fired) { fired = true; settled++; if (!shape(v)) bad++; } };
        onRej = () => { if (!fired) { fired = true; settled++; refused++; } };
    } catch (e) { settled++; return p; }
    let attached = 0;
    try { p.then(onOk, onRej); attached++; } catch (e) {}
    try { p.then(onOk, onRej); attached++; } catch (e) {}
    if (attached === 0) settled++;
    return p;
}

function pull(x) {
    try {
        const p = x.next();
        if (p && typeof p.then === "function") return track(p);
        return null;
    } catch (e) { refused++; return null; }
}

function later(fn, ms) {
    for (let i = 0; i < 16; i++) {
        try { setTimeout(fn, ms); return; } catch (e) {}
    }
}

/* Schedule a timer, retrying the WHOLE call -- including the `later` binding
 * lookup itself, which allocates on first read and so throws OUTSIDE later()'s
 * own try. */
function schedule(fn, ms) {
    for (let i = 0; i < 16; i++) {
        try { later(fn, ms); return true; } catch (e) {}
    }
    return false;
}

function report(extra) {
    for (let i = 0; i < 64; i++) {
        try {
            print("RESULT attempts=" + attempts + " settled=" + settled +
                  " bad=" + bad + " errs=0 refused=" + refused +
                  " closed=" + closed + extra);
            return;
        } catch (e) {}
    }
}

function finish() {
    if (done) return;
    done = true;
    for (let i = 0; i < 16; i++) {
        try { report(""); break; } catch (e) {}
    }
    for (let i = 0; i < 16; i++) { try { std.exit(0); } catch (e) {} }
}

function watchdog() {
    for (let i = 0; i < 16; i++) {
        try { report(" hung=1"); break; } catch (e) {}
    }
    for (let i = 0; i < 16; i++) { try { std.exit(1); } catch (e) {} }
}

/* Each phase's WHOLE body is the retry unit (see the header). */
function phase2() {
    for (let i = 0; i < 16; i++) {
        try {
            pull(w);              /* pump settles this one from the event */
            w.stop();             /* settle_done settles the rest */
            pull(w);              /* done after stop */
            schedule(phase3, 150);
            return;
        } catch (e) {}
    }
}

function phase3() {
    for (let i = 0; i < 16; i++) {
        try {
            pull(w);
            try { w.close(); closed = true; } catch (e) {}
            pull(w);              /* done past close */
            schedule(finish, 150);
            return;
        } catch (e) {}
    }
}

/* (1) prime every lazily-instantiated binding this file reads after the arm
 * marker -- see the header. The array is only a handle: reading each name
 * once here is what matters. The Watcher's own METHODS are in the list too:
 * a method read off the instance is created lazily as well, and `w.stop()` --
 * not a global -- is what the injection at n=221 breaks (measured: the first
 * phase2 retry catches "out of memory", every later one "not a function"
 * while every global still reads as a function). */
const PRIMED = [pull, track, shape, later, schedule, report, finish, watchdog,
                phase2, phase3, setTimeout, std.exit, std.getenv,
                file.makeDir, file.writeFile, Path, Watcher];

const ROOT = `${std.getenv("TMPDIR") || "/tmp"}/dj_w_mini_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
try { file.makeDir(new Path(ROOT), { recursive: true }); } catch (e) {}
const w = new Watcher(new Path(ROOT), { debounceMs: 5 });
/* the Watcher's own methods are lazily instantiated on first read as well --
   and `w.stop()` is exactly what an injection at n=221 breaks (leaving it
   "not a function" for good). Read them once, still before the arm. */
const PRIMED_M = [w.start, w.stop, w.close, w.next, w.return, w.stats];
print("PROBE-START");
schedule(watchdog, 30000);
try { std.getenv("##ALLOC-ARM##"); } catch (e) {}
for (let i = 0; i < 16; i++) { try { w.start(() => {}); break; } catch (e) {} }
for (let i = 0; i < 16; i++) { try { pull(w); break; } catch (e) {} }
for (let i = 0; i < 16; i++) { try { pull(w); break; } catch (e) {} }
for (let i = 0; i < 16; i++) {
    try { file.writeFile(new Path(ROOT + "/a.txt"), "1"); break; } catch (e) {}
}
schedule(phase2, 250);
