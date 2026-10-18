// flags: --std
// Split from tests/test_audit_w3_native_8.js: the net sections.
import * as http from "dyna:http";
import { Redis, TCPServer } from "dyna:net";
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
{
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
}
// ---- N3-11 / N3-07: Redis -----------------------------------------------
// ORACLE (options): an option is sent to the server as a C string; a NUL
// would silently shorten a password or a host.
// ORACLE (subscriptions): on RESP2 a subscribed connection receives
// ["message", channel, payload] arrays that are not replies. Whether the
// connection is subscribed is the SERVER's state; the confirmation's third
// element is its count. UNSUBSCRIBE of a channel that was never subscribed
// leaves the count unchanged, so a later message must still be routed to the
// push handler and must never resolve a command.
// ORACLE (pipeline): a subscribe verb has one confirmation per channel and on
// RESP3 they arrive as pushes, so a pipeline cannot match them positionally.
// CONTROL: PING resolves with PONG; the mock counts what reaches the wire.
{
    ok(kind(() => new Redis({ host: "127.0.0.1", port: 1, password: "a\0b" })) === "TypeError", "[red] N3-11: a Redis option containing NUL is refused");

    const wire = [];
    const srv = new TCPServer({ port: 0 });
    srv.start({
        data: (conn, bytes) => {
            const text = new TextDecoder().decode(bytes);
            const cmds = text.split(/(?=\*\d+\r\n)/).filter((c) => c.length);
            for (const c of cmds) {
                const parts = c.split("\r\n").filter((l, i, a) => i > 0 && a[i - 1].startsWith("$"));
                const verb = (parts[0] || "").toUpperCase();
                wire.push(verb);
                if (verb === "HELLO") conn.write("-ERR unknown command 'HELLO'\r\n");
                else if (verb === "SUBSCRIBE") conn.write("*3\r\n$9\r\nsubscribe\r\n$1\r\na\r\n:1\r\n");
                else if (verb === "UNSUBSCRIBE") conn.write("*3\r\n$11\r\nunsubscribe\r\n$1\r\nb\r\n:1\r\n");
                else if (verb === "PING") conn.write("*3\r\n$7\r\nmessage\r\n$1\r\na\r\n$1\r\nx\r\n+PONG\r\n");
                else conn.write("+OK\r\n");
            }
        },
    });
    const r = new Redis({ host: "127.0.0.1", port: srv.port, commandTimeoutMs: 3000 });
    const pushes = [];
    r.on("push", (m) => pushes.push(m));
    r.on("error", () => {});
    let ping = "unset";
    try {
        await r.command("SUBSCRIBE", "a");
        await r.command("UNSUBSCRIBE", "b");
        ping = await r.command("PING");
    } catch (e) { ping = "threw " + e; }
    ok(ping === "PONG", "[red] N3-07: a message after a foreign UNSUBSCRIBE does not resolve the next command (" + JSON.stringify(ping) + ")");
    ok(pushes.length === 1 && String(pushes[0][0]) === "message", "[red] N3-07: it is delivered to the push handler (" + pushes.length + ")");
    const before = wire.length;
    let pk = "none";
    try { await r.pipeline([["SUBSCRIBE", "z"], ["PING"]]); } catch (e) { pk = e.constructor.name; }
    ok(pk === "TypeError" && wire.length === before, "[red] N3-07: a subscribe verb inside pipeline() is refused before anything is sent (" + pk + ")");
    r.close();
    srv.close();
}
removeAll(new Path(String(T)));
console.log("test_net_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_net_audit_w3_native_8: " + fail + " failures");
