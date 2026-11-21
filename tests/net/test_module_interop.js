// covers: integration dyna:http.App dyna:http.HTTPClient dyna:http.HTTPServer dyna:http.HTTPServerAsync dyna:net.TCPServer dyna:http.WsClient -- the server and client types sharing one loop is the subject
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_module_interop");
import { TCPServer } from "dyna:net";
import {
    App as HApp, HTTPClient as HHTTPClient, HTTPServer as HHTTPServer,
    HTTPServerAsync as HHTTPServerAsync, WsClient as HWsClient,
    fetch as HFetch, Request as HRequest,
    App,
    HTTPClient,
    HTTPServer,
    HTTPServerAsync,
    WsClient
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
await __kitS.section("App identity, http imported first", async () => {
    const httpFirst = await import("dyna:http");
    const netSecond = await import("dyna:net");
    eq(httpFirst.App, App, "App identity, http imported dynamically");
    eq(httpFirst.HTTPServer, HTTPServer, "HTTPServer identity, dynamic import");
    for (const k of ["App", "HTTPServer", "HTTPClient", "HTTPServerAsync", "WsClient"]) ok(!(k in netSecond), "dyna:net no longer re-exports " + k);
    eq(netSecond.TCPServer, TCPServer, "TCPServer identity, net second");
});

print("=== 3. same class works after both imports (fresh construction) ===");
__kitS.section("http.HTTPServer constructs and binds", () => {
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
});
__kitS.end();

if (fails) {
    print("test_module_interop: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_module_interop failed");
}
print("test_module_interop: " + n + " assertions, 0 failures");
