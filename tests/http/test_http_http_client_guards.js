// timeout: 60
// covers: dyna:http.HTTPClient
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_client_guards");
import { HTTPServer, HTTPClient } from "dyna:http";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
function throwsMatch(fn, re, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        if (re.test(String(e)))
            return;
        failed++;
        console.log("  FAIL " + msg + " (threw " + String(e).slice(0, 80) + ")");
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

const srv = new HTTPServer({ port: 0, routes: { "/ok": "fine" } });
srv.start();
const base = `http://127.0.0.1:${srv.port}/ok`;

throwsMatch(() => new HTTPClient().get(base, { "Content-Length ": "5" }),
    /token/i, "an OWS-obfuscated framing header name is refused");

await __kitS.section("a normal custom header still works", async () => {
    const c = new HTTPClient();
    const r = await c.get(base, { "X-Custom": "v" });
    ok(r.status === 200, "a normal custom header still works");
    c.close();
});
await __kitS.section("a failed request does not echo query credentials", async () => {
    const c = new HTTPClient();
    let msg = "";
    try {
        await c.get(`http://127.0.0.1:1/x?token=SECRET123`);
    } catch (e) {
        msg = String(e);
    }
    ok(msg.indexOf("SECRET123") === -1, "a failed request does not echo query credentials (got " + msg.slice(0, 90) + ")");
    c.close();
});
await __kitS.section("a failed request does not echo userinfo", async () => {
    const c = new HTTPClient();
    let msg = "";
    try {
        await c.get(`http://user:hunter2@127.0.0.1:1/x`);
    } catch (e) {
        msg = String(e);
    }
    ok(msg.indexOf("hunter2") === -1, "a failed request does not echo userinfo (got " + msg.slice(0, 90) + ")");
    ok(msg.indexOf("***@") !== -1, "the redacted form keeps the host readable");
    c.close();
});
await __kitS.section("the 250-char scheme boundary is rejected with a bounded message", async () => {
    const c = new HTTPClient();
    let msg = "";
    try { await c.get("a".repeat(250) + "://u@h/"); } catch (e) { msg = String(e); }
    ok(msg.length > 0 && msg.length < 700,
        "the 250-char scheme boundary is rejected with a bounded message (" + msg.length + " chars)");
    c.close();
});
await __kitS.section("a huge credentialed URL is rejected with a bounded message", async () => {
    const c = new HTTPClient();
    let msg = "";
    try { await c.get("a".repeat(4000) + "://u@" + "b".repeat(4000) + "/x"); }
    catch (e) { msg = String(e); }
    ok(msg.length > 0 && msg.length < 700,
        "a huge credentialed URL is rejected with a bounded message (" + msg.length + " chars)");
    ok(msg.indexOf("b".repeat(100)) === -1, "the hostile URL tail is not echoed into the message");
    c.close();
});
__kitS.end();
srv.close();
console.log("test_http_client_guards: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_http_client_guards: " + failed + " failures");
