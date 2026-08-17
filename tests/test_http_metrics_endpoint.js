/* test_http_metrics_endpoint.js -- GET /metrics and /healthz on App.
 *
 * The review's finding was precise: a Metrics MODULE exists, and the HTTP
 * server carried ZERO references to it -- nothing was instrumented. This
 * pins the wiring end to end: a response bumps the C-side registry, the
 * /metrics route scrapes THAT SAME registry (not a second one), and the
 * JS Metrics API sees the same series. Counts are asserted EXACTLY, not
 * "at least", because the scrape of /metrics runs before its own bump --
 * if the order ever changed, the count would too.
 *
 * /metrics and /healthz are opt-IN (metrics: true): exposing internals is
 * a stance. A user route of the same path wins over the built-in
 * (registration beats convention).
 *
 * The client is the ASYNC one on purpose: App handlers run on the loop
 * thread, so the blocking client would deadlock against an in-process App
 * and every assertion would time out.
 */
import { App, HTTPClient, Metrics } from "dyna:net";
import * as std from "std";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const line = (s, pre) => s.split("\n").find((l) => l.startsWith(pre));

const rpc = (n) => JSON.stringify({ jsonrpc: "2.0", method: "echo", params: [n], id: n });

/* ---- the wiring: a response bumps the registry, /metrics scrapes it ---- */
{
    Metrics.reset();
    const app = new App({ port: 0, idleTimeoutMs: 5000, metrics: true });
    app.rpc("/rpc", { echo: ([n]) => n });
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        for (let i = 0; i < 5; i++) {
            const r = await c.postAsync(base + "/rpc", rpc(i), { "Content-Type": "application/json" });
            eq(r.status, 200, "request #" + i + " served");
        }
        const m = await c.getAsync(base + "/metrics");
        eq(m.status, 200, "/metrics answers 200");
        ok((m.headers["Content-Type"] || "").indexOf("text/plain") >= 0,
           "with the Prometheus content type (" + m.headers["Content-Type"] + ")");
        ok(m.body.indexOf("# TYPE dyn_http_responses_total counter") >= 0,
           "the counter series is typed");
        eq(line(m.body, "dyn_http_responses_total "), "dyn_http_responses_total 5",
           "the counter equals the EXACT number of responses before the scrape");
        ok(m.body.indexOf("dyn_http_response_bytes_total") >= 0,
           "response bytes are counted");
        ok(/^dyn_http_request_duration_seconds_count 5$/m.test(m.body),
           "the duration histogram counted the same five requests");
        ok(/^dyn_http_request_duration_seconds_bucket\{le="\+Inf"\} 5$/m.test(m.body),
           "and its +Inf bucket holds all of them");

        /* the JS Metrics API reads the SAME registry -- the live one, which
           already includes the /metrics response's own bump */
        const js = Metrics.scrape();
        eq(line(js, "dyn_http_responses_total "), "dyn_http_responses_total 6",
           "Metrics.scrape() sees the same counter (incl. the scrape response itself)");

        /* one more request, then the next scrape text shows seven */
        await c.postAsync(base + "/rpc", rpc(9), { "Content-Type": "application/json" });
        eq(line((await c.getAsync(base + "/metrics")).body, "dyn_http_responses_total "),
           "dyn_http_responses_total 7", "the counter advances between scrapes");

        /* ---- /healthz ---- */
        const h = await c.getAsync(base + "/healthz");
        eq(h.status, 200, "/healthz answers 200");
        eq(h.body, '{"ok":true}', "with the liveness document");
    } finally { c.close(); app.close(); }
    Metrics.reset();
}

/* ---- opt-OUT by default: no metrics:true, no endpoint ---- */
{
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.rpc("/rpc", { echo: ([n]) => n });
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        eq((await c.getAsync(base + "/metrics")).status, 404, "without metrics:true /metrics is 404");
        eq((await c.getAsync(base + "/healthz")).status, 404, "and /healthz is 404");
    } finally { c.close(); app.close(); }
}

/* ---- registration beats convention: a user route owns the path ---- */
{
    const app = new App({ port: 0, idleTimeoutMs: 5000, metrics: true });
    app.rpc("/healthz", { echo: ([n]) => n });   /* squats on the built-in */
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        const r = await c.postAsync(base + "/healthz", rpc(3), { "Content-Type": "application/json" });
        eq(r.status, 200, "the user's /healthz route serves");
        ok(r.body.indexOf('"result":3') >= 0, "with the user's result, not the built-in (" + r.body + ")");
        eq((await c.getAsync(base + "/metrics")).status, 200, "/metrics is unaffected by the squat");
    } finally { c.close(); app.close(); }
}

/* ---- metrics instrumentation is ALWAYS on, even with the endpoint off
        (the lock-free bump costs the request path nothing to run) ---- */
{
    Metrics.reset();
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.rpc("/rpc", { echo: ([n]) => n });
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        await c.postAsync(base + "/rpc", rpc(1), { "Content-Type": "application/json" });
        const js = Metrics.scrape();
        ok(line(js, "dyn_http_responses_total ") !== undefined,
           "the registry is populated even though no endpoint exposes it");
    } finally { c.close(); app.close(); }
    Metrics.reset();
}

print("test_http_metrics_endpoint: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
