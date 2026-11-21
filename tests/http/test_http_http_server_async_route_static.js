// covers: dyna:http.HTTPServerAsync
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_server_async_route_static");
import { HTTPServer, HTTPServerAsync } from "dyna:http";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(m, f) {
    try { f(); ok(false, m + ": did not throw"); }
    catch (e) { ok(String(e.message).indexOf("static routes only") >= 0, m + ": " + e.message); }
}

print("=== 1. a function route value is refused at construction ===");

throws("HTTPServerAsync fn route", () => new HTTPServerAsync({ port: 0, routes: { "/": () => {} } }));

print("=== 3. async server serves the same contract ===");
await __kitS.section("async static body", async () => {
    const s = new HTTPServerAsync({
        port: 0,
        routes: { "/": { contentType: "text/html", body: "<h1>hi</h1>" } },
    });
    s.start();
    const r = await fetch("http://127.0.0.1:" + s.port + "/");
    eq(await r.text(), "<h1>hi</h1>", "async static body");
    eq(r.headers.get("content-type"), "text/html", "async content-type");
    s.stop();
});
__kitS.end();

if (fails) {
    print("test_route_static: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_route_static failed");
}
print("test_route_static: " + n + " assertions, 0 failures");
