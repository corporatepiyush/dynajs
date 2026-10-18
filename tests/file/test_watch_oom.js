// flags: --std
import * as std from "std";

import { Watcher, Path } from "dyna:file";
import * as file from "dyna:file";

let attempts = 0, settled = 0, bad = 0, rejected = 0;

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
    p.then((v) => {
        if (!fired) { fired = true; settled++; if (!shape(v)) bad++; }
    }, () => {
        if (!fired) { fired = true; settled++; rejected++; }
    });
    return p;
}

function pull(w) {
    const p = w.next();
    return track(p);
}

const ROOT = `${std.getenv("TMPDIR") || "/tmp"}/dj_w_contract_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
file.makeDir(new Path(ROOT), { recursive: true });
const w = new Watcher(new Path(ROOT), { debounceMs: 5 });

const DEADLINE = setTimeout(() => {
    print("FAIL: the watch lifecycle never terminated (attempts=" + attempts +
          " settled=" + settled + ")");
    std.exit(1);
}, 40000);

w.start(() => {});
pull(w);
pull(w);
file.writeFile(new Path(ROOT + "/a.txt"), "1");
file.writeFile(new Path(ROOT + "/b.txt"), "2");

setTimeout(() => {
    pull(w);
    pull(w);
    w.stop();
    pull(w);
    const after = w.next();
    track(after);

    w.start();
    pull(w);
    file.writeFile(new Path(ROOT + "/c.txt"), "3");
    setTimeout(() => {
        pull(w);
        track(w.return());
        w.close();
        pull(w);
        setTimeout(finish, 250);
    }, 300);
}, 300);

function finish() {
    clearTimeout(DEADLINE);
    if (attempts < 8) throw new Error("lifecycle ran too few pulls: " + attempts);
    if (settled !== attempts)
        throw new Error("UNSETTLED pulls: " + settled + " of " + attempts);
    if (bad !== 0)
        throw new Error("POISONED settle arguments: " + bad);
    print("test_watch_oom: lifecycle settled " + settled + "/" + attempts +
          " pulls cleanly (" + rejected + " refusals)");
}
