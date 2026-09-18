/* test_route_static.js -- HTTPServer/HTTPServerAsync route contract (D1).
 *
 * dyn_route_copy treated a FUNCTION value as "{status:200, no body}" because
 * a function IS an object -- so an App-shaped handler aimed at the wrong
 * server compiled to an empty-body 200 forever and nothing errored. The
 * refusal below pins the fix; the rest pins what static routes DO serve.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_route_static.js
 */
import { HTTPServer, HTTPServerAsync } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(m, f) {
    try { f(); ok(false, m + ": did not throw"); }
    catch (e) { ok(String(e.message).indexOf("static routes only") >= 0, m + ": " + e.message); }
}

print("=== 1. a function route value is refused at construction ===");
throws("HTTPServer fn route", () => new HTTPServer({ port: 0, routes: { "/": () => {} } }));
throws("HTTPServerAsync fn route", () => new HTTPServerAsync({ port: 0, routes: { "/": () => {} } }));

print("=== 2. string and object routes serve their declared bytes ===");
{
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
}

print("=== 3. async server serves the same contract ===");
{
    const s = new HTTPServerAsync({
        port: 0,
        routes: { "/": { contentType: "text/html", body: "<h1>hi</h1>" } },
    });
    s.start();
    const r = await fetch("http://127.0.0.1:" + s.port + "/");
    eq(await r.text(), "<h1>hi</h1>", "async static body");
    eq(r.headers.get("content-type"), "text/html", "async content-type");
    s.stop();
}

if (fails) {
    print("test_route_static: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_route_static failed");
}
print("test_route_static: " + n + " assertions, 0 failures");
