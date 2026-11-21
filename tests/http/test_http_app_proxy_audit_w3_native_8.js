// covers: dyna:http.App
// Split from tests/test_audit_w3_native_8.js: the App.proxy section (moved out of
// tests/net in B1e: one module per suite; TCPServer is the upstream fixture).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_proxy_audit_w3_native_8");
import * as http from "dyna:http";
import { TCPServer } from "dyna:net";
import { Path, makeTempDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- N1-20: App.proxy ----------------------------------------------------
// ORACLE: RFC 9112 3.2: a request to an origin server uses origin-form, a
// target that starts with "/". A route prefix ending in "/" used to leave the
// remainder "http://evil.example/x" as the upstream target, which is
// absolute-form and names another authority. And an upstream exchange exists
// only for its client: when the client connection goes away (here: the idle
// sweep), the upstream socket must be closed with it, not left open until the
// upstream chooses to speak.
// CONTROL: the upstream received exactly one request.

await __kitS.section("GET /api/http://evil.example/x HTTP/1.1r Host: ar r", async () => {
    let upConns = 0, upClosed = 0, upReq = "";
    const silent = new TCPServer({ port: 0 });
    silent.start({ connect: () => { upConns++; }, data: (c, b) => { upReq += new TextDecoder().decode(b); }, close: () => { upClosed++; } });
    const app = new http.App({ port: 0, idleTimeoutMs: 400 });
    app.proxy("/api/", { host: "127.0.0.1", port: silent.port });
    app.start();
    let clientClosed = false;
    const cl = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
        connect: (c) => c.write("GET /api/http://evil.example/x HTTP/1.1\r\nHost: a\r\n\r\n"),
        close: () => { clientClosed = true; },
    });
    const t0 = Date.now();
    while (!(clientClosed && upClosed) && Date.now() - t0 < 6000) await sleep(50);
    const line = upReq.split("\r\n")[0];
    ok(upConns === 1 && /^GET \S+ HTTP\/1\.1$/.test(line), "control: the upstream received one request (" + line + ")");
    ok(line.split(" ")[1].startsWith("/"), "[red] N1-20: the upstream target is origin-form (" + line + ")");
    ok(clientClosed, "control: the idle sweep closed the waiting client");
    ok(upClosed === 1, "[red] N1-20: the upstream connection is closed with its client (" + upClosed + ")");
    cl.close(); app.close(); silent.close();
});
__kitS.end();
removeAll(new Path(String(T)));
console.log("test_http_app_proxy_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_app_proxy_audit_w3_native_8: " + fail + " failures");
