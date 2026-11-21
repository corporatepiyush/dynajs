// covers: dyna:http.WsClient
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_ws_client_audit_http_1");
import { App, WsClient } from "dyna:http";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const app = new App({ port: 0 });
let serverMsgs = [];
app.ws("/w", {
    open: (s) => { s.send("hello"); },
    message: (s, d, bin) => {
        serverMsgs.push({
            text: typeof d === "string" ? d : null,
            bin,
            len: typeof d === "string" ? d.length : (d.byteLength ?? d.length),
        });
    },
});
app.start();

// 1. N1-02: close() inside the client's own message handler must not crash.

await __kitS.section("message handler ran", async () => {
    let closed = false;
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        message: (w) => { closed = true; w.close(); },
    });
    await wait(400);
    ok(closed, "message handler ran");
    ws.dispose();
});

await __kitS.section("close() in open handler: no crash", async () => {
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        open: (w) => { w.close(); },
    });
    await wait(400);
    ws.dispose();
    ok(true, "close() in open handler: no crash");
});

await __kitS.section("close() in close handler: no crash", async () => {
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        open: (w) => { setTimeout(() => w.close(), 50); },
        close: (w) => { w.close(); },
    });
    await wait(600);
    ws.dispose();
    ok(true, "close() in close handler: no crash");
});

await __kitS.section("client opened for binary send", async () => {
    let opened = false;
    serverMsgs = [];
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        open: (w) => {
            opened = true;
            w.send(new Uint8Array([1, 2, 3]));
        },
    });
    await wait(400);
    ok(opened, "client opened for binary send");
    ok(serverMsgs.length === 1 && serverMsgs[0].bin === true && serverMsgs[0].len === 3,
        "Uint8Array arrives as binary of length 3 (got " + JSON.stringify(serverMsgs) + ")");
    ws.dispose();
});

await __kitS.section("non-view non-string still goes out as text", async () => {
    serverMsgs = [];
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        open: (w) => { w.send({ a: 1 }); },
    });
    await wait(400);
    ok(serverMsgs.length === 1 && serverMsgs[0].bin === false,
        "non-view non-string still goes out as text");
    ws.dispose();
});
__kitS.end();

app.close();
if (fails) {
    print("test_audit_http_1: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_1 failed");
}
print("test_audit_http_1: " + n + " assertions, 0 failures");
