import { App, WsClient } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

// N1-03: the idle sweep closes an idle WS conn whose close handler closes
// OTHER conns. Pre-fix the sweep walked a freed list link (ASan UAF).
{
    const socks = [];
    let closes = 0;
    const app = new App({ port: 0, idleTimeoutMs: 200 });
    app.ws("/w", {
        open: (s) => { socks.push(s); },
        close: (s) => {
            closes++;
            for (const o of socks) if (o !== s) { try { o.close(); } catch (e) {} }
        },
    });
    app.start();
    const cs = [];
    for (let i = 0; i < 6; i++)
        cs.push(new WsClient("ws://127.0.0.1:" + app.port + "/w", {}));
    await wait(2500);
    ok(closes >= 1, "at least one close handler ran during the sweep (got " + closes + ")");
    for (const c of cs) c.dispose();
    app.close();
}

// variant: the close handler of the swept conn calls app.close() from its own
// timer callback -- teardown must be deferred, not run under the sweep.
{
    const app = new App({ port: 0, idleTimeoutMs: 150 });
    const socks = [];
    let opened = 0;
    app.ws("/w", {
        open: (s) => { opened++; socks.push(s); },
        close: (s) => { setTimeout(() => { try { app.close(); } catch (e) {} }, 20); },
    });
    app.start();
    const cs = [];
    for (let i = 0; i < 4; i++)
        cs.push(new WsClient("ws://127.0.0.1:" + app.port + "/w", {}));
    await wait(2200);
    ok(opened === 4, "all four connections were established before the sweep (" + opened + ")");
    for (const c of cs) c.dispose();
    ok(true, "app.close() inside a sweep-triggered close handler: no crash");
    try { app.dispose(); } catch (e) {}
}

if (fails) {
    print("test_audit_http_3: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_3 failed");
}
print("test_audit_http_3: " + n + " assertions, 0 failures");
