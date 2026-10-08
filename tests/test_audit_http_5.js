import "./httpc.js";
import { App, TCPServer, HTTPClient } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const bytes = (s) => { const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff; return a; };
const str = (b) => { let s = ""; for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]); return s; };

// N1-01: App.proxy -- a client that disconnects before the upstream replies
// used to UAF the client conn in dyn_proxy_relay_response. The assertion is
// "no crash under this engine" (the crash is a native UAF, proven by
// .agent-work/audit-plan/N1/probes/p01_proxy_uaf.js under ASan).
{
    const back = new TCPServer({ port: 0 });
    back.start({
        data: (c, b) => {
            setTimeout(() => {
                try {
                    c.write(bytes("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
                } catch (e) {}
            }, 300);
        },
    });
    const app = new App({ port: 0 });
    app.proxy("/p", { host: "127.0.0.1", port: back.port });
    app.start();

    // a) client leaves before the upstream reply arrives.
    const cl = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
        connect: (c, e) => {
            if (!e) {
                c.write(bytes("GET /p HTTP/1.1\r\nHost: x\r\n\r\n"));
                setTimeout(() => c.close(), 80);
            }
        },
    });
    await wait(1200);
    cl.close();
    ok(true, "client disconnect before upstream reply: no crash");

    // b) a well-behaved client still gets the proxied response afterwards.
    const c = new HTTPClient();
    const r = await c.getAsync("http://127.0.0.1:" + app.port + "/p");
    ok(r.status === 200 && r.body === "ok", "proxy relay still works (got " + r.status + ")");
    c.close();

    // c) several disconnecting clients back to back.
    for (let i = 0; i < 5; i++) {
        const cx = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
            connect: (cc, e) => {
                if (!e) {
                    cc.write(bytes("GET /p HTTP/1.1\r\nHost: x\r\n\r\n"));
                    setTimeout(() => cc.close(), 20);
                }
            },
        });
        await wait(120);
        cx.close();
    }
    await wait(600);
    ok(true, "five more aborting proxy clients: no crash");

    app.close();
    back.close();
}

if (fails) {
    print("test_audit_http_5: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_5 failed");
}
print("test_audit_http_5: " + n + " assertions, 0 failures");
