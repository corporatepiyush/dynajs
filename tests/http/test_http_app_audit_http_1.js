// covers: dyna:http.App
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_audit_http_1");
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

await __kitS.section("Int8Array from WsConn.send arrives as binary of length 3", async () => {
    const app2 = new App({ port: 0 });
    app2.ws("/bin", {
        open: (s) => { s.send(new Int8Array([9, 8, 7])); },
    });
    app2.start();
    let got = null, gotBin = false;
    const ws = new WsClient("ws://127.0.0.1:" + app2.port + "/bin", {
        message: (w, d, bin) => { got = d; gotBin = bin; w.close(); },
    });
    await wait(400);
    ok(got && typeof got !== "string" && got.byteLength === 3 && new Uint8Array(got)[0] === 9,
        "Int8Array from WsConn.send arrives as binary of length 3 (got " + (got && typeof got) + ")");
    ok(gotBin, "isBinary flag true for typed-array send");
    ws.dispose();
    app2.close();
});

await __kitS.section("DataView slice arrives as binary of length 2", async () => {
    const app3 = new App({ port: 0 });
    app3.ws("/dv", {
        open: (s) => {
            const b = new ArrayBuffer(4);
            new DataView(b).setUint16(0, 0x1234);
            s.send(new DataView(b, 0, 2));
        },
    });
    app3.start();
    let got = null;
    const ws = new WsClient("ws://127.0.0.1:" + app3.port + "/dv", {
        message: (w, d, bin) => { got = d; w.close(); },
    });
    await wait(400);
    ok(got && typeof got !== "string" && got.byteLength === 2 && new Uint8Array(got)[0] === 0x12,
        "DataView slice arrives as binary of length 2");
    ws.dispose();
    app3.close();
});
__kitS.end();

app.close();
if (fails) {
    print("test_audit_http_1: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_1 failed");
}
print("test_audit_http_1: " + n + " assertions, 0 failures");
