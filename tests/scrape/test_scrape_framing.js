// flags: --std
// timeout: 120
import { Fetcher } from "dyna:scrape";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b,
    m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
function skipped(m) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + m); return; }
    skip++; print("  SKIP  " + m);
}
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

{
    const g = new Fetcher({ agent: "redact/1.0", robots: false,
        allowPrivateHosts: true,
        proxy: "http://alice:s3cr3t@127.0.0.1:8080",
        ca: "-----BEGIN CERTIFICATE-----\nAAA\n-----END CERTIFICATE-----\n" });
    const s = g.stats();
    eq(s.proxy, "http://***@127.0.0.1:8080", "stats redacts proxy userinfo");
    eq(s.ca, "<pem-redacted>", "stats redacts PEM material");
    g.close();
    const h = new Fetcher({ agent: "redact/1.0", robots: false,
        allowPrivateHosts: true,
        proxy: "http://127.0.0.1:8080", ca: "/etc/ssl/certs/ca.pem" });
    const t = h.stats();
    eq(t.proxy, "http://127.0.0.1:8080", "CONTROL: a userinfo-free proxy is echoed");
    eq(t.ca, "/etc/ssl/certs/ca.pem", "CONTROL: a CA path is echoed");
    h.close();
}

{
    const grab = (fn) => { try { fn(); return null; } catch (e) { return e; } };
    const e1 = grab(() => new Fetcher({ agent: "x", retries: 4294967296 }));
    ok(e1 instanceof RangeError && /retries/.test(e1.message),
        "an out-of-int32 retries is a RangeError, not a wrapped truncation");
    const e2 = grab(() => new Fetcher({ agent: "x", maxRedirects: -2147483649 }));
    ok(e2 instanceof RangeError && /maxRedirects/.test(e2.message),
        "an out-of-int32 maxRedirects is a RangeError");
}

if (!Which("python3")) {
    skipped("python3 missing -- the raw-peer framing probes cannot run");
} else {
    const T = makeTempDir("framing");
    const P = (n) => T + "/" + n;
    writeFile(new Path(P("raw.py")), [
        "import socket, sys, threading, os as _os",
        "threading.Timer(180, lambda: _os._exit(0)).start()",
        "RESP = {",
        " '/malcl': b'HTTP/1.1 200 OK\\r\\nContent-Length: 12abc\\r\\nConnection: close\\r\\n\\r\\n',",
        " '/dupcl': b'HTTP/1.1 200 OK\\r\\nContent-Length: 4\\r\\nContent-Length: 5\\r\\nConnection: close\\r\\n\\r\\n',",
        " '/clte': b'HTTP/1.1 200 OK\\r\\nContent-Length: 4\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n0\\r\\n\\r\\n',",
        " '/emptycl': b'HTTP/1.1 200 OK\\r\\nContent-Length:\\r\\nConnection: close\\r\\n\\r\\n',",
        " '/ok': b'HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nhi',",
        " '/dupok': b'HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nhi',",
        " '/teok': b'HTTP/1.1 200 OK\\r\\nTransfer-Encoding: gzip\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nhi',",
        " '/foldcl': b'HTTP/1.1 200 OK\\r\\nContent-Length: 4\\r\\n 5\\r\\nConnection: close\\r\\n\\r\\nhi',",
        " '/nulname': b'HTTP/1.1 200 OK\\r\\nContent-Length\\x00x: 4\\r\\nConnection: close\\r\\n\\r\\nhi',",
        " '/spcolon': b'HTTP/1.1 200 OK\\r\\nContent-Length : 4\\r\\nConnection: close\\r\\n\\r\\nhi',",
        "}",
        "def handle(c):",
        "    try:",
        "        data = b''",
        "        while b'\\r\\n\\r\\n' not in data:",
        "            b = c.recv(4096)",
        "            if not b: return",
        "            data += b",
        "        path = data.split(b' ')[1].decode()",
        "        c.sendall(RESP.get(path, RESP['/ok']))",
        "    except Exception: pass",
        "    finally:",
        "        try: c.close()",
        "        except Exception: pass",
        "s = socket.socket()",
        "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)",
        "s.bind(('127.0.0.1', 0))",
        "s.listen(16)",
        "open(sys.argv[1], 'w').write(str(s.getsockname()[1]))",
        "while True:",
        "    c, _ = s.accept()",
        "    threading.Thread(target=handle, args=(c,), daemon=True).start()",
    ].join("\n"));
    sh(`python3 ${P("raw.py")} ${P("port")} >${P("raw.log")} 2>&1 &`);
    let PORT = 0;
    for (let i = 0; i < 60 && !PORT; i++) {
        sh("sleep 0.1");
        try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) { }
    }
    if (!PORT) {
        skipped("the raw peer did not start");
    } else {
        const BASE = "http://127.0.0.1:" + PORT;
        const f = new Fetcher({ agent: "framing-test/1.0", robots: false,
            allowPrivateHosts: true, minDelayMs: 0, retries: 0 });

        const refuses = (name, route, rx) => {
            try {
                const r = f.getStream(BASE + route);
                ok(false, name + " (resolved status " + r.status + ")");
            } catch (e) {
                ok(e instanceof RangeError && rx.test(e.message), name + " [" + e + "]");
            }
        };

        refuses("CL with trailing garbage", "/malcl", /malformed Content-Length/);
        refuses("conflicting duplicate CL", "/dupcl",
            /conflicting duplicate Content-Length/);
        refuses("CL + Transfer-Encoding: chunked", "/clte",
            /Content-Length with Transfer-Encoding/);
        refuses("empty CL", "/emptycl", /empty Content-Length/);
        // R3-4: the scrape parser matches dyna-http.c's header-line hardening.
        refuses("obsolete line folding after CL", "/foldcl",
            /malformed header line/);
        refuses("NUL inside a header name", "/nulname",
            /malformed header line/);
        refuses("whitespace before the name colon", "/spcolon",
            /malformed header line/);

        {
            const r = f.get(BASE + "/ok");
            ok(r.status === 200 && r.body === "hi",
                "CONTROL: a well-framed exchange still resolves");
        }
        {
            const r = f.get(BASE + "/dupok");
            ok(r.status === 200 && r.body === "hi",
                "CONTROL: identical duplicate CL is accepted");
        }
        {
            const r = f.get(BASE + "/teok");
            ok(r.status === 200, "CONTROL: a non-chunked TE beside CL is accepted");
        }
        f.close();
        sh(`pkill -f '${P("raw.py")}' 2>/dev/null`);
    }
    removeAll(new Path(T));
}

print("test_scrape_framing: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail) throw new Error("test_scrape_framing: " + fail + " failures");
