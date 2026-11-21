// flags: --std
// timeout: 120
// covers: integration dyna:http.HTTPServer dyna:http.HTTPClient -- wire framing pinned on both ends of one exchange
// slots: 2
// peer-proof: exempt * -- the 1xx flood is bounded only by socket buffers and the recording proxy adds a second set, so the Python flood behind it completes and fails; flood.js is proven by mutant P3
import { HTTPServer, HTTPClient } from "dyna:http";
import { TCPServer } from "dyna:net";
import { Exec } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";
import { spawnHelper } from "../lib/helper.js";
import { suite } from "../lib/kit.js";

const t = suite("test_http_wire_framing");
let A = null;
const ok = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);

let srv = null;
t.before(() => {
    srv = new HTTPServer({ port: 0, routes: { "/ok": "fine" } });
    srv.start();
});
t.after(() => { if (srv) srv.close(); });

function rawExchange(bytes, ms = 1500) {
    return new Promise((resolve) => {
        let buf = "";
        let done = false;
        let c = null;
        const finish = () => {
            if (done)
                return;
            done = true;
            try { if (c) c.close(); } catch (e) {}
            resolve(buf);
        };
        c = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
            connect(conn, err) {
                if (err) {
                    finish();
                    return;
                }
                conn.write(bytes);
            },
            data(conn, b) {
                for (const x of new Uint8Array(b))
                    buf += String.fromCharCode(x);
            },
            close() { finish(); },
            drain() {},
        });
        setTimeout(finish, ms);
    });
}
const keepAlive = `GET /ok HTTP/1.1\r\nHost: x\r\n\r\n`;

test("HEAD gets headers with an accurate Content-Length and no body", async () => {
    const head = await rawExchange("HEAD /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    ok(/^HTTP\/1\.1 200/.test(head), "HEAD gets a 200 (got " + JSON.stringify(head.slice(0, 24)) + ")");
    ok(head.indexOf("\r\n\r\n") !== -1, "HEAD has a header terminator");
    const body = head.slice(head.indexOf("\r\n\r\n") + 4);
    ok(body === "", "HEAD carries no body bytes (got " + JSON.stringify(body) + ")");
    ok(/Content-Length: 4/i.test(head), "HEAD keeps an accurate Content-Length");
});

test("a plain HTTP/1.0 GET response is byte-exact", async () => {
    const resp = await rawExchange("GET /ok HTTP/1.0\r\n\r\n");
    ok(/^HTTP\/1\.1 200 OK\r\nDate: [A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} \d{2}:\d{2}:\d{2} GMT\r\nContent-Type: text\/plain\r\nContent-Length: 4\r\nConnection: close\r\n\r\nfine$/.test(resp),
        "a plain GET is unchanged (got " + JSON.stringify(resp) + ")");
});

test("pipelined keep-alive responses are framed back to back", async () => {
    const first = await rawExchange("GET /ok HTTP/1.1\r\nHost: x\r\n\r\nGET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    const count = (first.match(/HTTP\/1\.1 200/g) || []).length;
    ok(count === 2, "two pipelined keep-alive responses (got " + count + ")");
    ok(first.indexOf("\r\n\r\nfineHTTP/1.1") !== -1, "bodies are framed back to back without stray bytes");
});

test("an over-long request target is refused with 414", async () => {
    const resp = await rawExchange("GET /" + "a".repeat(3000) + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    ok(resp.indexOf("414") !== -1, "an over-long target is refused with 414 (got " + JSON.stringify(resp.slice(0, 24)) + ")");
});

test("a control byte in the query is refused with 400", async () => {
    const resp = await rawExchange("GET /ok?q=\u0001bad HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    ok(resp.indexOf("400") !== -1, "a control byte in the query is refused with 400 (got " + JSON.stringify(resp.slice(0, 24)) + ")");
});

test("an oversized Content-Length is refused", async () => {
    const resp = await rawExchange("GET /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n");
    ok(resp.indexOf("400") !== -1 || resp.indexOf("413") !== -1,
        "a wrapped/oversized Content-Length is refused (got " + JSON.stringify(resp.slice(0, 24)) + ")");
});

test("the client refuses an endless 1xx interim-response flood", async (a) => {
    {
        const T = makeTempDir("d2hwire");
        const P = (x) => T + "/" + x;
        const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
        const ROOT = String(Exec("/bin/pwd", []).stdout).trim();
        const KIT = JSON.stringify(ROOT + "/tests/lib/rawpeer.js");
        writeFile(new Path(P("flood.js")), `import { listen } from ${KIT};
import * as file from "dyna:file";
listen(async (c) => {
    const r = await c.readUntil("\\r\\n\\r\\n", { timeoutMs: 10000 });
    if (!r.found) return;
    let sent = 0;
    while (sent < 100000 && (await c.write("HTTP/1.1 100 Continue\\r\\n\\r\\n"))) sent++;
    file.writeFile(scriptArgs[2], String(sent));
    c.abort();
}, { portFile: scriptArgs[1] });
`);
        const flood = spawnHelper(`${ROOT}/dynajs --std ${P("flood.js")} ${P("port")} ${P("count")} >${P("srv.log")} 2>&1`);
        let fport = 0;
        for (let i = 0; i < 80 && !fport; i++) {
            sh("sleep 0.1");
            try { fport = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
        }
        if (fport) {
            const c = new HTTPClient();
            c.setTimeout(15000);
            const t0 = Date.now();
            let threw = false, why = "";
            try { await c.get(`http://127.0.0.1:${fport}/x`); } catch (e) { threw = true; why = String(e && e.message); }
            const dt = Date.now() - t0;
            // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
            ok(threw && /too many interim responses/.test(why) && dt < 10000,
                "an endless 1xx stream is refused by the interim cap (dt=" + dt + "ms, " + why + ")");
            let sent = 0;
            for (let i = 0; i < 40 && !sent; i++) {
                sh("sleep 0.1");
                try { sent = parseInt(readFile(new Path(P("count"))), 10) || 0; } catch (e) {}
            }
            // The server counts every interim response the kernel accepted, and loopback socket buffers absorb
            // ~1-2k of these 25-byte messages before the close is seen (more on Linux than macOS), so the bound is
            // a tenth of the 100000-message flood rather than the client's own cap.
            ok(sent > 0 && sent < 10000,
                "the client refuses well before a flood completes (server sent " + sent + ")");
            c.close();
        } else {
            ok(false, "the flood.js peer published its port");
        }
        flood.stop();
    }
}, { timeoutMs: 60000 });

await t.run();
