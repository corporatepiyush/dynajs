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
    let threw = false;
    try {
        TCPServer.connect({ path: "/nonexistent-dynajs-test.sock", tls: { servername: "x" } });
    } catch (e) {
        threw = true;
    }
    ok(threw, "connect({path, tls}) is not silently ignored (TLS over a unix socket is refused or attempted, never dropped)");
}
{
    let threw = false;
    try { new TCPServer({ port: 0, highWaterMark: 0 }); } catch (e) { threw = /highWaterMark/i.test(String(e)); }
    ok(threw, "server construction validates highWaterMark too");
}
console.log("test_net_tcp_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_net_tcp_bounds: " + failed + " failures");
