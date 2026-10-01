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

function phase2() {
    for (let i = 0; i < 16; i++) {
        try {
            pull(w);
            w.stop();
            pull(w);
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
            pull(w);
            schedule(finish, 150);
            return;
        } catch (e) {}
    }
}

const PRIMED = [pull, track, shape, later, schedule, report, finish, watchdog,
                phase2, phase3, setTimeout, std.exit, std.getenv,
                file.makeDir, file.writeFile, Path, Watcher];

const ROOT = `${std.getenv("TMPDIR") || "/tmp"}/dj_w_mini_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
try { file.makeDir(new Path(ROOT), { recursive: true }); } catch (e) {}
const w = new Watcher(new Path(ROOT), { debounceMs: 5 });
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
