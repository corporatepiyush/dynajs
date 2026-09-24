// flags: --std
/* test_watch_oom.js -- the Watcher iteration's SETTLE CONTRACT, plain mode.
 *
 * Every promise the lifecycle produces must SETTLE, and settle CLEANLY:
 * a resolved pull is exactly { done } with { path, kind } strings on a value
 * pull and `value === undefined` on a done pull; a rejected pull is a legal
 * refusal. Nothing may hang and nothing may arrive as a garbage value.
 *
 * This is the NO-INJECTION leg: it pins the shapes and the accounting on the
 * ordinary lifecycle. The allocation-failure leg -- fail-the-Nth-allocation
 * across the same lifecycle -- is tests/agent/t2d_ml_df_time/watch_oom_sweep.sh
 * (1164 injection points, every run required to terminate with every pull
 * settled and zero poisoned values).
 *
 * Timers drive the phases (no top-level await): the same discipline the
 * injection probe uses, so both legs run the identical lifecycle.
 *
 * Run: dynajs --std tests/test_watch_oom.js
 */
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
    w.stop();                 /* settles every parked pull done */
    pull(w);                  /* after stop + drain: done */
    const after = w.next();
    track(after);

    /* the restart + return() + close() shapes */
    w.start();
    pull(w);
    file.writeFile(new Path(ROOT + "/c.txt"), "3");
    setTimeout(() => {
        pull(w);
        track(w.return());    /* return() promises { value: undefined, done } */
        w.close();
        pull(w);              /* past close: done */
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
