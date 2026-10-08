import { App, WsClient, TCPServer } from "dyna:net";

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
{
    let closed = false;
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        message: (w) => { closed = true; w.close(); },
    });
    await wait(400);
    ok(closed, "message handler ran");
    ws.dispose();
}

// 2. close() inside its own open handler.
{
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        open: (w) => { w.close(); },
    });
    await wait(400);
    ws.dispose();
    ok(true, "close() in open handler: no crash");
}

// 3. close() inside its own close handler.
{
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        open: (w) => { setTimeout(() => w.close(), 50); },
        close: (w) => { w.close(); },
    });
    await wait(600);
    ws.dispose();
    ok(true, "close() in close handler: no crash");
}

// 4. N1-16: WsClient.send(new Uint8Array) reaches the server as a binary frame.
{
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
}

// 5. N1-16 server side: WsConn.send(new Int8Array) is a binary frame for the client.
{
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
}

// 6. send() of a plain object still stringifies (documented ToString fallback).
{
    serverMsgs = [];
    const ws = new WsClient("ws://127.0.0.1:" + app.port + "/w", {
        open: (w) => { w.send({ a: 1 }); },
    });
    await wait(400);
    ok(serverMsgs.length === 1 && serverMsgs[0].bin === false,
        "non-view non-string still goes out as text");
    ws.dispose();
}

// 7. DataView goes out as binary.
{
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
}

app.close();
if (fails) {
    print("test_audit_http_1: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_1 failed");
}
print("test_audit_http_1: " + n + " assertions, 0 failures");
