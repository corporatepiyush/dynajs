/* test_module_interop.js -- importing dyna:net and dyna:http in any order
 * yields the SAME classes (A1).
 *
 * Before the fix, a second registration of the HTTP classes on one runtime
 * failed -- JS_NewClassID reused its stored id -- so one of the two import
 * orders left every constructor undefined while the module import still
 * succeeded. The identity assertions below are the regression: they fail
 * unless the re-entry path re-exports the ctor from the existing class.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_module_interop.js
 */
import { App, HTTPClient, HTTPServer, HTTPServerAsync, WsClient, TCPServer }
    from "dyna:net";
import {
    App as HApp, HTTPClient as HHTTPClient, HTTPServer as HHTTPServer,
    HTTPServerAsync as HHTTPServerAsync, WsClient as HWsClient,
    fetch as HFetch, Request as HRequest,
} from "dyna:http";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + String(a) + ", want " + String(b) + ")"); }

print("=== 1. net-then-http static import: same ctor objects ===");
eq(HApp, App, "App identity");
eq(HHTTPClient, HTTPClient, "HTTPClient identity");
eq(HHTTPServer, HTTPServer, "HTTPServer identity");
eq(HHTTPServerAsync, HTTPServerAsync, "HTTPServerAsync identity");
eq(HWsClient, WsClient, "WsClient identity");
ok(typeof HFetch === "function", "fetch exported from dyna:http");
ok(typeof HRequest === "function", "Request exported from dyna:http");
ok(typeof TCPServer === "function", "net-only class still intact");

print("=== 2. http-then-net order via dynamic import ===");
{
    const httpFirst = await import("dyna:http");
    const netSecond = await import("dyna:net");
    eq(httpFirst.App, netSecond.App, "App identity, http imported first");
    eq(httpFirst.HTTPServer, netSecond.HTTPServer, "HTTPServer identity, http first");
    eq(httpFirst.HTTPClient, netSecond.HTTPClient, "HTTPClient identity, http first");
    eq(httpFirst.HTTPServerAsync, netSecond.HTTPServerAsync, "HTTPServerAsync identity, http first");
    eq(httpFirst.WsClient, netSecond.WsClient, "WsClient identity, http first");
}

print("=== 3. same class works after both imports (fresh construction) ===");
{
    const s = new HHTTPServer({ port: 0, routes: { "/": "ok" } });
    ok(s.port > 0, "http.HTTPServer constructs and binds");
    const s2 = new HTTPServer({ port: 0, routes: { "/": "ok" } });
    ok(s2.port > 0, "net.HTTPServer constructs and binds after double import");
    s.stop();
    s2.stop();
    const app = new HApp({ port: 0 });
    app.start();
    ok(app.port > 0, "http.App starts after double import");
    app.close();
}

if (fails) {
    print("test_module_interop: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_module_interop failed");
}
print("test_module_interop: " + n + " assertions, 0 failures");