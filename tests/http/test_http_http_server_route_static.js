// covers: dyna:http.HTTPServer
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_server_route_static");
import { HTTPServer, HTTPServerAsync } from "dyna:http";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(m, f) {
    try { f(); ok(false, m + ": did not throw"); }
    catch (e) { ok(String(e.message).indexOf("static routes only") >= 0, m + ": " + e.message); }
}

print("=== 1. a function route value is refused at construction ===");

throws("HTTPServer fn route", () => new HTTPServer({ port: 0, routes: { "/": () => {} } }));

print("=== 2. string and object routes serve their declared bytes ===");
await __kitS.section("plain body", async () => {
    const s = new HTTPServer({
        port: 0,
        routes: {
            "/": "plain body",
            "/json": { status: 201, contentType: "application/json", body: '{"a":1}' },
            "/empty": "",
        },
    });
    s.start();
    const base = "http://127.0.0.1:" + s.port;
    const r1 = await fetch(base + "/");
    eq(r1.status, 200, "string route status");
    eq(await r1.text(), "plain body", "string route body");
    const ct = r1.headers.get("content-type") || "";
    ok(ct.indexOf("text/plain") === 0, "default content-type (" + ct + ")");
    const r2 = await fetch(base + "/json");
    eq(r2.status, 201, "object route status");
    eq(r2.headers.get("content-type"), "application/json", "object route content-type");
    eq(await r2.text(), '{"a":1}', "object route body");
    const r3 = await fetch(base + "/empty");
    eq(r3.status, 200, "explicitly empty body still answers");
    eq(await r3.text(), "", "empty body served as empty");
    s.stop();
});
__kitS.end();

if (fails) {
    print("test_route_static: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_route_static failed");
}
print("test_route_static: " + n + " assertions, 0 failures");
