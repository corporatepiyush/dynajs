// flags: --std
// covers: dyna:http.App
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_http_metrics_endpoint");
import { App, HTTPClient } from "dyna:http";
import * as std from "std";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const line = (s, pre) => s.split("\n").find((l) => l.startsWith(pre));

const rpc = (n) => JSON.stringify({ jsonrpc: "2.0", method: "echo", params: [n], id: n });

await __kitS.section("without metrics:true /metrics is 404", async () => {
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.rpc("/rpc", { echo: (n) => n });
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        eq((await c.get(base + "/metrics")).status, 404, "without metrics:true /metrics is 404");
        eq((await c.get(base + "/healthz")).status, 404, "and /healthz is 404");
    } finally { c.close(); app.close(); }
});

await __kitS.section("the user's /healthz route serves", async () => {
    const app = new App({ port: 0, idleTimeoutMs: 5000, metrics: true });
    app.rpc("/healthz", { echo: (n) => n });
    app.start();
    const c = new HTTPClient();
    try {
        const base = "http://127.0.0.1:" + app.port;
        const r = await c.post(base + "/healthz", rpc(3), { "Content-Type": "application/json" });
        eq(r.status, 200, "the user's /healthz route serves");
        ok(r.body.indexOf('"result":3') >= 0, "with the user's result, not the built-in (" + r.body + ")");
        eq((await c.get(base + "/metrics")).status, 200, "/metrics is unaffected by the squat");
    } finally { c.close(); app.close(); }
});
__kitS.end();

print("test_http_metrics_endpoint: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
