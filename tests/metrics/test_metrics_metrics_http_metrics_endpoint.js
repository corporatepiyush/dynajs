// flags: --std
// covers: integration dyna:metrics.Metrics dyna:http.App -- the App metrics endpoint serves the dyna:metrics registry over HTTP
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_metrics_metrics_http_metrics_endpoint");
import { App, HTTPClient } from "dyna:http";
import { Metrics } from "dyna:metrics";
import * as std from "std";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const line = (s, pre) => s.split("\n").find((l) => l.startsWith(pre));

const rpc = (n) => JSON.stringify({ jsonrpc: "2.0", method: "echo", params: [n], id: n });

await __kitS.section("request #", async () => {
    Metrics.reset();
    const app = new App({ port: 0, idleTimeoutMs: 5000, metrics: true });
    app.rpc("/rpc", { echo: (n) => n });
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        for (let i = 0; i < 5; i++) {
            const r = await c.post(base + "/rpc", rpc(i), { "Content-Type": "application/json" });
            eq(r.status, 200, "request #" + i + " served");
        }
        const m = await c.get(base + "/metrics");
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

        const js = Metrics.scrape();
        eq(line(js, "dyn_http_responses_total "), "dyn_http_responses_total 6",
           "Metrics.scrape() sees the same counter (incl. the scrape response itself)");

        await c.post(base + "/rpc", rpc(9), { "Content-Type": "application/json" });
        eq(line((await c.get(base + "/metrics")).body, "dyn_http_responses_total "),
           "dyn_http_responses_total 7", "the counter advances between scrapes");

        const h = await c.get(base + "/healthz");
        eq(h.status, 200, "/healthz answers 200");
        eq(h.body, '{"ok":true}', "with the liveness document");
    } finally { c.close(); app.close(); }
    Metrics.reset();
});

await __kitS.section("dyn_http_responses_total", async () => {
    Metrics.reset();
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.rpc("/rpc", { echo: (n) => n });
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        await c.post(base + "/rpc", rpc(1), { "Content-Type": "application/json" });
        const js = Metrics.scrape();
        ok(line(js, "dyn_http_responses_total ") !== undefined,
           "the registry is populated even though no endpoint exposes it");
    } finally { c.close(); app.close(); }
    Metrics.reset();
});
__kitS.end();

print("test_http_metrics_endpoint: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
