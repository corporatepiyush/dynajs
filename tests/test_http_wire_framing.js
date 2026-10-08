// flags: --std
// timeout: 120
import { HTTPServer, HTTPClient } from "dyna:http";
import { TCPServer } from "dyna:net";
import { Exec, Which } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}

const srv = new HTTPServer({ port: 0, routes: { "/ok": "fine" } });
srv.start();

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

async function main() {
    {
        const head = await rawExchange("HEAD /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        ok(/^HTTP\/1\.1 200/.test(head), "HEAD gets a 200 (got " + JSON.stringify(head.slice(0, 24)) + ")");
        ok(head.indexOf("\r\n\r\n") !== -1, "HEAD has a header terminator");
        const body = head.slice(head.indexOf("\r\n\r\n") + 4);
        ok(body === "", "HEAD carries no body bytes (got " + JSON.stringify(body) + ")");
        ok(/Content-Length: 4/i.test(head), "HEAD keeps an accurate Content-Length");
    }
    {
        const resp = await rawExchange("GET /ok HTTP/1.0\r\n\r\n");
        ok(resp === "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4\r\nConnection: close\r\n\r\nfine",
            "a plain GET is unchanged (got " + JSON.stringify(resp) + ")");
    }
    {
        const first = await rawExchange("GET /ok HTTP/1.1\r\nHost: x\r\n\r\nGET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        const count = (first.match(/HTTP\/1\.1 200/g) || []).length;
        ok(count === 2, "two pipelined keep-alive responses (got " + count + ")");
        ok(first.indexOf("\r\n\r\nfineHTTP/1.1") !== -1, "bodies are framed back to back without stray bytes");
    }
    {
        const resp = await rawExchange("GET /" + "a".repeat(3000) + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        ok(resp.indexOf("414") !== -1, "an over-long target is refused with 414 (got " + JSON.stringify(resp.slice(0, 24)) + ")");
    }
    {
        const resp = await rawExchange("GET /ok?q=\u0001bad HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        ok(resp.indexOf("400") !== -1, "a control byte in the query is refused with 400 (got " + JSON.stringify(resp.slice(0, 24)) + ")");
    }
    {
        const resp = await rawExchange("GET /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n");
        ok(resp.indexOf("400") !== -1 || resp.indexOf("413") !== -1,
            "a wrapped/oversized Content-Length is refused (got " + JSON.stringify(resp.slice(0, 24)) + ")");
    }
    if (Which("python3")) {
        const T = makeTempDir("d2hwire");
        const P = (x) => T + "/" + x;
        const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
        writeFile(new Path(P("flood.py")), [
            "import socket, sys, threading",
            "srv = socket.socket()",
            "srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)",
            "srv.bind(('127.0.0.1', 0))",
            "srv.listen(8)",
            "open(sys.argv[1], 'w').write(str(srv.getsockname()[1]))",
            "def handle(c):",
            "    c.settimeout(10)",
            "    try:",
            "        d = b''",
            "        while b'\\r\\n\\r\\n' not in d:",
            "            b = c.recv(4096)",
            "            if not b: return",
            "            d += b",
            "        sent = 0",
            "        for _ in range(100000):",
            "            c.sendall(b'HTTP/1.1 100 Continue\\r\\n\\r\\n')",
            "            sent += 1",
            "    except Exception: pass",
            "    open(sys.argv[2], 'w').write(str(sent))",
            "    c.close()",
            "while True:",
            "    c, _ = srv.accept()",
            "    threading.Thread(target=handle, args=(c,), daemon=True).start()",
        ].join("\n"));
        sh(`python3 ${P("flood.py")} ${P("port")} ${P("count")} >${P("srv.log")} 2>&1 &`);
        let fport = 0;
        for (let i = 0; i < 80 && !fport; i++) {
            sh("sleep 0.1");
            try { fport = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
        }
        if (fport) {
            const c = new HTTPClient();
            c.setTimeout(15000);
            const t0 = Date.now();
            let threw = false;
            try { c.get(`http://127.0.0.1:${fport}/x`); } catch (e) { threw = true; }
            const dt = Date.now() - t0;
            ok(threw && dt < 5000,
                "an endless 1xx stream is refused by the interim cap (dt=" + dt + "ms)");
            let sent = 0;
            for (let i = 0; i < 40 && !sent; i++) {
                sh("sleep 0.1");
                try { sent = parseInt(readFile(new Path(P("count"))), 10) || 0; } catch (e) {}
            }
            ok(sent > 0 && sent <= 1024,
                "the client refuses well before a flood completes (server sent " + sent + ")");
            c.close();
        } else {
            ok(false, "python3 1xx flood server did not start");
        }
    } else {
        console.log("  SKIP  python3 missing: the 1xx flood server is not run");
    }
    srv.close();
    console.log("test_http_wire_framing: " + (n - failed) + " passed, " + failed + " failed");
    if (failed)
        throw new Error("test_http_wire_framing: " + failed + " failures");
}
main().catch((e) => {
    throw new Error("test_http_wire_framing: harness " + e);
});
