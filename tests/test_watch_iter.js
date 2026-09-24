// flags: --std
/* test_watch_iter.js --: Watcher.stop, the async iteration, and the
 * typed event object.
 *
 *   - the event is { path, kind }, kind one of change/add/addDir/unlink/
 *     unlinkDir, delivered to BOTH surfaces (the start() callback and the
 *     for-await queue);
 *   - for await (const ev of w) -- the house manual-iterator shape
 *     (next()/return()/[Symbol.asyncIterator], a promise of { value, done }
 *     per pull);
 *   - w.stop() halts the watch and ends the event stream with the CHANNEL
 *     close contract: buffered events still drain through next() in arrival
 *     order, then { done: true } -- so stop-then-iterate always terminates;
 *   - a for-await `break` calls return(), which stops the watch
 *     (unconditional cleanup, the pg.queryIter doctrine);
 *   - start()/stats()/close()/restart keep working.
 *
 * Timing discipline: nothing asserts a DURATION; every wait is a bound and a
 * missed event fails loudly. Every async loop is terminated by a stop() or a
 * return() under a watchdog, so a broken stream fails instead of hanging.
 */
import * as std from "std";
import { Watcher, Path } from "dyna:file";
import * as file from "dyna:file";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { ok(a === b, what + " (got " + a + ", want " + b + ")"); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const ROOT = `${std.getenv("TMPDIR") || "/tmp"}/dj_witer_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
const P = (x) => new Path(x);
file.makeDir(P(ROOT), { recursive: true });

/* the watchdog: nothing in this file may run forever */
const DEADLINE = setTimeout(() => {
    print("FAIL: the watcher iteration never terminated");
    print("  assertions so far: " + n + ", failures " + bad);
    throw new Error("watch iteration hung");
}, 40000);

/* ---- 1. for-await delivers typed events; stop() ends the loop ---- */
{
    const w = new Watcher(P(ROOT), { debounceMs: 20 });
    w.start();
    const got = [];
    const iter = (async () => {
        for await (const ev of w) got.push(ev);
    })();
    await sleep(80);                       /* the pull parks */
    file.writeFile(P(ROOT + "/a.txt"), "1");
    file.writeFile(P(ROOT + "/b.txt"), "1");
    await sleep(500);                      /* let both events arrive */
    w.stop();
    await Promise.race([iter, sleep(3000)]);
    ok(got.length >= 2, "for-await saw the events: " +
       got.map((e) => e.kind + ":" + e.path).join(", "));
    ok(got.some((e) => e.kind === "add" && e.path === "a.txt"),
       "an add event is typed { path, kind }");
    for (const ev of got) {
        const keys = Object.keys(ev).sort().join(",");
        eq(keys, "kind,path", "the event object is exactly { path, kind }");
        ok(["change", "add", "addDir", "unlink", "unlinkDir"].indexOf(ev.kind) >= 0,
           "kind is one of the five event kinds: " + ev.kind);
        ok(typeof ev.path === "string", "path is a string");
    }
    ok(!w.closed, "stop() halts but does not close");
    const s = w.stats();
    ok(typeof s.events === "number" && s.events >= 2, "stats still works after stop()");

    /* stop() is idempotent; iteration stays ended */
    w.stop();
    const after = await w.next();
    eq(after.done, true, "next() after stop() and a drained buffer is done");
    const again = await w.next();
    eq(again.done, true, "done is stable");
    w.close();
}

/* ---- 2. stop-then-iterate: buffered events drain, then done ---- */
{
    const w = new Watcher(P(ROOT), { debounceMs: 20 });
    w.start();
    const firstP = w.next();               /* parks -- do NOT await yet */
    await sleep(80);
    file.writeFile(P(ROOT + "/c.txt"), "1");
    const first = await firstP;            /* the parked pull gets it */
    ok(!first.done && first.value.path === "c.txt",
       "the parked pull got the first event");
    file.writeFile(P(ROOT + "/d.txt"), "1");
    await sleep(500);                      /* this one buffers (no puller) */
    w.stop();                              /* stop BEFORE iterating further */
    const seen = [];
    for (let i = 0; i < 10; i++) {
        const r = await w.next();
        if (r.done) break;
        seen.push(r.value.kind + ":" + r.value.path);
    }
    ok(seen.length <= 1, "only the already-emitted event drains: " + seen.join(", "));
    const tail = await w.next();
    eq(tail.done, true, "stop-then-iterate terminates (the probe itself)");
    w.close();
}

/* ---- 3. stop() while a next() is parked: it resolves done, no hang ---- */
{
    const w = new Watcher(P(ROOT), { debounceMs: 20 });
    w.start();
    const parked = w.next();
    await sleep(30);
    w.stop();
    const r = await Promise.race([parked, sleep(3000)]);
    ok(r && r.done === true, "a parked next() resolves done when stop() lands");
    w.close();
}

/* ---- 4. break runs return(): the watch stops with it ---- */
{
    const w = new Watcher(P(ROOT), { debounceMs: 20 });
    w.start();
    let seen = 0;
    const iter = (async () => {
        for await (const ev of w) {
            seen++;
            break;                         /* -> return() -> stop() */
        }
    })();
    await sleep(80);
    file.writeFile(P(ROOT + "/e.txt"), "1");
    await Promise.race([iter, sleep(3000)]);
    eq(seen, 1, "the loop body ran once before the break");
    /* return() stopped the watch: no further events arrive */
    await sleep(400);
    file.writeFile(P(ROOT + "/f.txt"), "1");
    await sleep(400);
    const r = await w.next();
    eq(r.done, true, "after a break the stream is over (return() stopped it)");
    ok(!w.closed, "return() stops, it does not close");
    w.close();
}

/* ---- 5. both surfaces see the same events ---- */
{
    const w = new Watcher(P(ROOT), { debounceMs: 20 });
    const cbSeen = [];
    w.start((e) => cbSeen.push(e.kind + ":" + e.path));
    const itSeen = [];
    const iter = (async () => { for await (const ev of w) itSeen.push(ev.kind + ":" + ev.path); })();
    await sleep(80);
    file.writeFile(P(ROOT + "/g.txt"), "1");
    await sleep(500);
    w.stop();
    await Promise.race([iter, sleep(3000)]);
    ok(cbSeen.some((s) => s === "add:g.txt"),
       "the callback surface saw the add: " + cbSeen.join(", "));
    ok(itSeen.some((s) => s === "add:g.txt"),
       "the iteration surface saw the same add: " + itSeen.join(", "));
    w.close();
}

/* ---- 6. concurrent next() calls resolve in arrival order ---- */
{
    const w = new Watcher(P(ROOT), { debounceMs: 20 });
    w.start();
    const p1 = w.next();
    const p2 = w.next();
    await sleep(80);
    file.writeFile(P(ROOT + "/h.txt"), "1");
    await sleep(500);
    w.stop();
    const [r1, r2] = await Promise.all([
        Promise.race([p1, sleep(3000)]),
        Promise.race([p2, sleep(3000)]),
    ]);
    ok(r1 && r2, "both concurrent pulls settled");
    if (r1 && r2) {
        ok(!r1.done, "the first pull got the event");
        if (!r2.done)
            ok(r1.value.path <= r2.value.path, "arrival order preserved");
        else
            eq(r2.done, true, "the second pull ended at stop() (one event)");
    }
    w.close();
}

/* ---- 7. lifecycle: restart after stop; close ends everything ---- */
{
    const w = new Watcher(P(ROOT), { debounceMs: 20 });
    w.start();
    w.stop();
    /* restart reopens the stream */
    w.start();
    const iter = (async () => {
        for await (const ev of w) { return ev; }
    })();
    await sleep(80);
    file.writeFile(P(ROOT + "/i.txt"), "1");
    const ev = await Promise.race([iter, sleep(3000)]);
    ok(ev && ev.kind === "add" && ev.path === "i.txt",
       "start() after stop() re-arms and the stream reopens");
    w.stop();
    w.close();
    ok(w.closed, "close() sets closed");
    const r = await w.next();
    eq(r.done, true, "next() on a closed watcher is done (no throw, no hang)");
    w.stop();                              /* no-op after close */
    const s = (() => { try { return w.stats(); } catch (e) { return "threw " + e.message; } })();
    ok(typeof s === "string" || typeof s.events === "number",
       "stats after close either works or throws a named error: " +
       (typeof s === "string" ? s : "worked"));
}

/* ---- 8. adversarial deferrals (probe-derived; scratch/e5_probe_w3.js) ---- */
{
    /* stop() from INSIDE the start() callback: deferred to the sweep end */
    {
        const w = new Watcher(P(ROOT), { debounceMs: 10 });
        let seen = 0;
        w.start(() => { seen++; w.stop(); });
        file.writeFile(P(ROOT + "/j.txt"), "1");
        await sleep(400);
        file.writeFile(P(ROOT + "/k.txt"), "1");
        await sleep(400);
        eq(seen, 1, "stop() inside the callback fired exactly once more");
        const r = await w.next();
        ok(r.done === true || typeof r.value.kind === "string",
           "next() after a deferred stop settles");
        w.close();
    }
    /* stop() from INSIDE the for-await body */
    {
        const w = new Watcher(P(ROOT), { debounceMs: 10 });
        w.start();
        const iter = (async () => {
            let c = 0;
            for await (const ev of w) { c++; w.stop(); }
            return c;
        })();
        await sleep(80);
        file.writeFile(P(ROOT + "/l.txt"), "1");
        const c = await Promise.race([iter, sleep(4000)]);
        eq(c, 1, "stop() inside the loop body ends the loop after the event");
        w.close();
    }
    /* close() from INSIDE the for-await body: dispose mid-iteration */
    {
        const w = new Watcher(P(ROOT), { debounceMs: 10 });
        w.start();
        const iter = (async () => {
            let c = 0;
            for await (const ev of w) { c++; w.close(); }
            return c;
        })();
        await sleep(80);
        file.writeFile(P(ROOT + "/m.txt"), "1");
        const c = await Promise.race([iter, sleep(4000)]);
        eq(c, 1, "close() inside the loop body ends the loop (no crash)");
    }
    /* start/pull/stop/close churn: a next() parked across a stop never
       delivers a stale event and never hangs */
    {
        let churnBad = 0;
        for (let i = 0; i < 20; i++) {
            const w = new Watcher(P(ROOT), { debounceMs: 5 });
            w.start();
            const p = w.next();
            w.stop();
            const r = await p;
            if (!r.done) churnBad++;
            w.close();
        }
        eq(churnBad, 0, "20x start/pull/stop/close churn is clean");
    }
    /* a bag is an object or absent: a non-object options argument refuses
       instead of silently defaulting */
    {
        let t = null;
        try { new Watcher(P(ROOT), "recursive=false"); } catch (e) { t = e; }
        ok(t instanceof TypeError,
           "a string options argument throws TypeError (got " +
           (t && t.constructor && t.constructor.name) + ")");
        t = null;
        try { new Watcher(P(ROOT), 5); } catch (e) { t = e; }
        ok(t instanceof TypeError, "a number options argument throws TypeError");
    }
}

/* ---- 9. pump-path churn: 1000+ events delivered to parked pulls ----
 * (the LSan regression: every queue node handed to a parked pull must be
 * freed -- the ASan gate runs this file leak-clean) */
{
    const dir = ROOT + "/churn";
    file.makeDir(P(dir), { recursive: true });
    const w = new Watcher(P(dir), { debounceMs: 5 });
    w.start();
    await sleep(30);
    let got = 0, hung = 0;
    for (let wave = 0; wave < 4; wave++) {
        const pulls = [];
        for (let i = 0; i < 250; i++) pulls.push(w.next());  /* park first */
        for (let i = 0; i < 260; i++) file.writeFile(P(dir + "/w" + wave + "_" + i), "1");
        const rs = await Promise.race([
            Promise.all(pulls),
            sleep(8000).then(() => null),
        ]);
        if (rs === null) { hung++; continue; }
        for (const r of rs)
            if (!r.done && r.value && typeof r.value.kind === "string") got++;
    }
    eq(hung, 0, "4 waves of 250 parked pulls all settled (no hang)");
    ok(got >= 1000, "1000-event churn: " + got + " events delivered to parked pulls");
    ok(w.stats().truncated === false, "the churn stayed inside the queue cap");
    w.stop();
    w.close();
}

/* ---- 10. GC and parked pulls (lifetime regression rows) ---- */
{
    /* a parked pull whose PROMISE survives keeps the watch delivering even
       after every watcher handle is dropped: the promise, not a native
       handle pin, is what holds the watcher */
    {
        const dir = ROOT + "/gc1";
        file.makeDir(P(dir), { recursive: true });
        file.writeFile(P(dir + "/seed"), "s");
        let w = new Watcher(P(dir), { debounceMs: 5 });
        w.start(() => {});
        await sleep(50);
        const kept = w.next();      /* parked; keep ONLY the promise */
        w = null;
        std.gc(); std.gc();
        file.writeFile(P(dir + "/e1"), "1");
        const r = await Promise.race([kept, sleep(5000).then(() => null)]);
        ok(r !== null, "a kept pull promise settles after the watcher handle is dropped");
        ok(r !== null && !r.done && /e1/.test(r.value.path),
           "and it settles with the event: " + JSON.stringify(r));
    }
    /* park, drop EVERYTHING (watcher and promise), force GC, repeat: the
       finalizer sees live waiters here and must free them without calling
       the resolvers -- observable as crash-freedom on this gate and as a
       leak-free run on the ASan gate */
    {
        const dir = ROOT + "/gc2";
        file.makeDir(P(dir), { recursive: true });
        file.writeFile(P(dir + "/seed"), "s");
        for (let i = 0; i < 50; i++) {
            const w = new Watcher(P(dir), { debounceMs: 5 });
            w.start(() => {});
            w.next();               /* parked and immediately dropped */
            if (i % 5 === 0) std.gc();
        }
        std.gc(); std.gc(); std.gc();
        ok(true, "50 parked-and-dropped pulls collected without resolver calls");
        file.writeFile(P(dir + "/after"), "1");
        await sleep(80);
        std.gc();
        ok(true, "traffic and GC after the churn are inert");
    }
}

clearTimeout(DEADLINE);
file.removeAll(P(ROOT));
print("test_watch_iter: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
