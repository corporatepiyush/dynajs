// flags: --std
// timeout: 60
import { TCPServer } from "dyna:net";

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
        console.log("  FAIL " + msg + " (threw " + String(e).slice(0, 70) + ")");
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

{
    const t = new TCPServer();
    ok(t !== undefined && t.closed === false, "new TCPServer() with no options constructs");
    t.close();
    ok(t.closed === true, "the bare server closes cleanly");
}
{
    const t = new TCPServer({});
    ok(t !== undefined, "new TCPServer({}) constructs");
    t.close();
}
{
    throwsMatch(() => TCPServer.connect({ path: "/nonexistent-dynajs-test.sock", highWaterMark: 0 }),
        /highWaterMark/i, "connect({path}) honours the bounds options (highWaterMark refused)");
}
{
    throwsMatch(() => TCPServer.connect({ path: "/nonexistent-dynajs-test.sock", maxConnections: -1 }),
        /maxConnections/i, "connect({path}) honours maxConnections");
}
{
    // The failure is a throw where the backend connects a unix socket inline
    // (kqueue/epoll) and a connect-handler error where it is queued (io_uring).
    let outcome = "pending";
    try {
        const h = TCPServer.connect({ path: "/nonexistent-dynajs-test.sock", tls: { servername: "x" } },
            { connect(conn, err) { outcome = err ? "reported" : "connected"; } });
        for (let i = 0; i < 100 && outcome === "pending"; i++) await new Promise((r) => setTimeout(r, 20));
        h.close();
    } catch (e) {
        outcome = "threw";
    }
    ok(outcome === "threw" || outcome === "reported",
        "connect({path, tls}) is not silently ignored (TLS over a unix socket is refused or attempted, never dropped): " + outcome);
}
{
    let threw = false;
    try { new TCPServer({ port: 0, highWaterMark: 0 }); } catch (e) { threw = /highWaterMark/i.test(String(e)); }
    ok(threw, "server construction validates highWaterMark too");
}
console.log("test_net_tcp_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_net_tcp_bounds: " + failed + " failures");
