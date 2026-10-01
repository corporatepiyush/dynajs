import * as std from "std";
import { Watcher, Path } from "dyna:file";
import * as file from "dyna:file";

let attempts = 0, settled = 0, bad = 0, errs = 0, refused = 0;
let closed = false, done = false;
let w = null, w2 = null;

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
        settled++;
        return p;
    }
    let attached = 0;
    try { p.then(onOk, onRej); attached++; } catch (e) {  }
    try { p.then(onOk, onRej); attached++; } catch (e) {  }
    if (attached === 0)
        settled++;
    return p;
}

function pull(x) {
    try {
        const p = x.next();
        if (p && typeof p.then === "function")
            return track(p);
        return null;
    } catch (e) {
        refused++;
        return null;
    }
}

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

function later(fn, ms) {
    for (let i = 0; i < 16; i++) {
        try { setTimeout(fn, ms); return; } catch (e) {  }
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
        } catch (e) {  }
    }
}

function exitNow(code) {
    for (let i = 0; i < 16; i++) {
        try { std.exit(code); } catch (e) {  }
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

function phase2() {
    pull(w);
    pull(w);
    try { w.stop(); } catch (e) { errs++; }
    pull(w);

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

    try { w.close(); closed = true; } catch (e) { errs++; }
    pull(w);
    pull(w);

    later(finish, 300);
}

const ROOT = `${std.getenv("TMPDIR") || "/tmp"}/dj_w_oom_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
try {
    file.makeDir(new Path(ROOT), { recursive: true });
} catch (e) {  }
w = new Watcher(new Path(ROOT), { debounceMs: 5 });

print("PROBE-START");
later(watchdog, 45000);

try { std.getenv("##ALLOC-ARM##"); } catch (e) { errs++; }

try {
    w.start((ev) => {  });
} catch (e) { errs++; }
pull(w);
pull(w);
pull(w);
try { file.writeFile(new Path(ROOT + "/a.txt"), "1"); } catch (e) { errs++; }
try { file.writeFile(new Path(ROOT + "/b.txt"), "2"); } catch (e) { errs++; }
later(phase2, 300);
