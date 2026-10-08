// flags: --std
// timeout: 120
import { Fetcher } from "dyna:scrape";
import { Exec, Which } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; console.log("  FAIL " + w + (d ? "  [" + d + "]" : "")); } };

if (!Which("python3")) {
    skip++;
    console.log("test_fetcher_crlf: python3 missing, wire capture skipped");
} else {
    const T = makeTempDir("d2crlf");
    const P = (n) => T + "/" + n;
    const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

    writeFile(new Path(P("rawsrv.py")), [
        "import socket, sys, threading",
        "srv = socket.socket()",
        "srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)",
        "srv.bind(('127.0.0.1', 0))",
        "srv.listen(16)",
        "open(sys.argv[1], 'w').write(str(srv.getsockname()[1]))",
        "log = open(sys.argv[2], 'ab', buffering=0)",
        "def handle(c):",
        "    c.settimeout(3)",
        "    data = b''",
        "    try:",
        "        while b'\\r\\n\\r\\n' not in data:",
        "            b = c.recv(4096)",
        "            if not b: break",
        "            data += b",
        "    except Exception: pass",
        "    log.write(data + b'\\n====\\n')",
        "    try: c.sendall(b'HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nok')",
        "    except Exception: pass",
        "    c.close()",
        "while True:",
        "    c, _ = srv.accept()",
        "    threading.Thread(target=handle, args=(c,), daemon=True).start()",
    ].join("\n"));
    sh(`python3 ${P("rawsrv.py")} ${P("port")} ${P("reqlog")} >${P("srv.log")} 2>&1 &`);
    let PORT = 0;
    for (let i = 0; i < 80 && !PORT; i++) {
        sh("sleep 0.1");
        try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
    }
    const mkF = (o) => new Fetcher(Object.assign({
        agent: "crlfprobe/1.0 (+https://example.test/bot)",
        minDelayMs: 0, retries: 0, robots: false, allowPrivateHosts: true,
    }, o));
    const log = () => { try { return readFile(new Path(P("reqlog"))); } catch (e) { return ""; } };
    const waitFor = (pred) => {
        for (let i = 0; i < 60; i++) {
            if (pred(log())) return true;
            sh("sleep 0.1");
        }
        return pred(log());
    };
    const recs = () => log().split("\n====\n").filter((s) => s.length);

    if (PORT) {
        {
            const f = mkF();
            const s = f.getStream(`http://127.0.0.1:${PORT}/plain`);
            s.close();
            ok(s.status === 200, "plain getStream succeeds");
            ok(waitFor((l) => l.indexOf("/plain") !== -1), "the request reached the raw capture server");
            const w = recs()[0] || "";
            ok(/^GET \/plain HTTP\/1\.1\r\n/.test(w), "request line intact", JSON.stringify(w.slice(0, 40)));
            f.close();
        }
        {
            const f = mkF();
            let threw = false;
            try {
                const s = f.getStream(`http://127.0.0.1:${PORT}/x\r\nX-Injected: 1\r\n\r\nGET /`);
                s.close();
            } catch (e) {
                threw = true;
            }
            const raw = log();
            ok(threw || (raw.indexOf("\r\nX-Injected") === -1 && raw.indexOf("\nX-Injected") === -1),
                "CR/LF in the URL path is refused or percent-encoded (no injected line)",
                JSON.stringify(raw.slice(0, 160)));
            f.close();
        }
        {
            const f = mkF();
            let threw = false;
            try {
                const s = f.getStream(`http://127.0.0.1:${PORT}/a b`);
                s.close();
            } catch (e) {
                threw = true;
            }
            const raw = log();
            ok(threw || raw.indexOf("GET /a b ") === -1,
                "a space in the request target is refused or percent-encoded");
            f.close();
        }
        {
            const f = mkF({ headers: { "x\r\nInjected": "1" } });
            let threw = false;
            try {
                const s = f.getStream(`http://127.0.0.1:${PORT}/hdrname`);
                s.close();
            } catch (e) {
                threw = true;
            }
            const raw = log();
            ok(threw && raw.indexOf("hdrname") === -1,
                "a CR/LF header NAME is refused before any bytes are sent");
            f.close();
        }
        {
            const f = mkF({ headers: { "x-bad": "a\r\nInjected: 1" } });
            let threw = false;
            try {
                const s = f.getStream(`http://127.0.0.1:${PORT}/hdrval`);
                s.close();
            } catch (e) {
                threw = true;
            }
            const raw = log();
            ok((threw && raw.indexOf("hdrval") === -1)
                || raw.indexOf("Injected: 1") === -1,
                "a CR/LF header VALUE cannot smuggle a header");
            f.close();
        }
    } else {
        ok(false, "python3 raw capture server did not start");
    }
}
console.log("test_fetcher_crlf: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail)
    throw new Error("test_fetcher_crlf: " + fail + " failures");
