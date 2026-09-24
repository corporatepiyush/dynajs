// flags: --std
/* test_http_app_workers.js --: the App ctor's `workers`/`backlog`
 * options.
 *
 * The plan's premise ("App passes workers/backlog through to HTTPServer")
 * does not match the code: App is the REACTOR server (HTTPServerAsync's
 * model), not the thread-pool one, and it never embeds HTTPServer. What the
 * option pair CAN honestly do here:
 *   - `backlog` really is forwarded to listen(2) (it used to be hardcoded
 *     1024), so listen success under an explicit backlog is the probe;
 *   - `workers` is validated against HTTPServer's 1..64 range and echoed
 *     back through the `workers` getter, so a config shared between the two
 *     servers does not silently drop the key -- and the CHANGELOG/API.md say
 *     plainly that App serves every request on its single reactor thread
 *     today, so workers sizes nothing (yet).
 * The silent-drop alternative was exactly what exists to kill.
 *
 * The ASYNC client, like every in-process App test: App handlers run on the
 * loop thread, so the blocking client would deadlock against its own server.
 * EVERY app is closed(): an App holds the shared reactor, and the loop
 * waits on it -- an unclosed app means a green assert count and a process
 * that never exits.
 */
import { App, HTTPClient } from "dyna:net";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const throws = (fn, m) => {
    try { fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
    catch (e) {
        pass++;
        return e;
    }
    return null;
};

/* ---- 1. defaults: the getters answer what the server will run with ---- */
{
    const app = new App({ port: 0 });
    eq(app.workers, 1, "workers defaults to 1 (unkeyed == HTTPServer's floor)");
    eq(app.backlog, 1024, "backlog defaults to the 1024 start() has always used");
    app.close();
}

/* ---- 2. accepted + echoed: explicit configuration is readable back ---- */
{
    const app = new App({ port: 0, workers: 4, backlog: 64 });
    eq(app.workers, 4, "workers: 4 is echoed");
    eq(app.backlog, 64, "backlog: 64 is echoed");
    app.close();
}

/* ---- 3. accepted-and-starts: workers>1 serves real traffic, and an
 * ---- explicit backlog still listens (the listen(2) probe) ---- */
{
    const app = new App({ port: 0, workers: 4, backlog: 16 });
    app.rpc("/ping", { p: (n) => "pong" + n });
    app.start();
    ok(app.port > 0, "App with workers=4, backlog=16 listens (port " + app.port + ")");
    const c = new HTTPClient();
    let served = -1;
    try {
        const r = await c.postAsync("http://127.0.0.1:" + app.port + "/ping",
                                    JSON.stringify({ jsonrpc: "2.0", method: "p", params: [7], id: 1 }),
                                    { "Content-Type": "application/json" });
        eq(r.status, 200, "workers=4 app serves an rpc");
        served = JSON.parse(r.body).result;
    } catch (e) { ok(false, "workers=4 app rpc round trip threw: " + e); }
    eq(served, "pong7", "and the handler really ran");
    eq(app.workers, 4, "workers unchanged after start");
    eq(app.backlog, 16, "backlog unchanged after start");
    app.close();
}

/* ---- 4. backlog boundary: 1 is legal (listen(fd, 1) succeeds) ---- */
{
    const app = new App({ port: 0, backlog: 1 });
    app.rpc("/x", { hi: () => "hi" });
    app.start();
    ok(app.port > 0, "backlog=1 listens");
    eq(app.backlog, 1, "backlog=1 echoed");
    app.close();
}

/* ---- 5. the validation matrix: non-positive / fractional / out-of-range /
 * ---- NaN all refuse, naming the option (loop-2: workers=0/-1) ---- */
const badWorkers = [0, -1, 2.5, 65, 1e9, NaN, Infinity];
for (const w of badWorkers) {
    const e = throws(() => new App({ port: 0, workers: w }),
                     "workers=" + w + " refuses");
    ok(e instanceof RangeError,
       "workers=" + w + " throws RangeError (got " + (e && e.constructor.name) + ")");
    ok(e && /workers/.test(e.message), "workers=" + w + " error names the option");
}
const badBacklog = [0, -1, 1.5, 65536, 1e9, NaN, Infinity];
for (const b of badBacklog) {
    const e = throws(() => new App({ port: 0, backlog: b }),
                     "backlog=" + b + " refuses");
    ok(e instanceof RangeError,
       "backlog=" + b + " throws RangeError (got " + (e && e.constructor.name) + ")");
    ok(e && /backlog/.test(e.message), "backlog=" + b + " error names the option");
}

/* ---- 6. holds: a typo'd key is still rejected, or not ---- */
{
    const e = throws(() => new App({ port: 0, workers: 2, wrkers: 2 }),
                     "unknown key beside workers refuses");
    ok(e instanceof TypeError, "typo'd key is a TypeError");
    ok(/wrkers/.test(e.message), "and names the misspelled key");
}

/* ---- 7. null counts as absent (module-wide bag convention) ---- */
{
    const app = new App({ port: 0, workers: null, backlog: null });
    eq(app.workers, 1, "workers: null falls back to the default");
    eq(app.backlog, 1024, "backlog: null falls back to the default");
    app.close();
}

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_http_app_workers: " + fail + " failures");
