// flags: --std
// timeout: 120
// covers: dyna:scrape.Fetcher
// slots: 2
// A response over the body cap is a RangeError on every path (blackbox finding scrapd-F1). The Fetcher's
// own HTTPClient refused a DECLARED Content-Length over maxBodyBytes with a plain Error ("response too
// large") while the d.ts promises the RangeError that getStream and the received-bytes check already threw.
// The cap error now leaves the HTTPClient as a RangeError, so HTTPClient.get, Fetcher.get and
// Fetcher.getStream agree. A python3 server on loopback declares a 20000-byte body; skipped without python3.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_fetcher_body_cap_type");
import { Fetcher } from "dyna:scrape";
import { spawnHelper } from "../lib/helper.js";
import { HTTPClient } from "dyna:http";
import { Exec, Which } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";

let n = 0, fails = 0, skip = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const caught = async (fn) => { try { await fn(); return null; } catch (e) { return e; } };

await __kitS.section("test_scrape_body_cap_type: python3 missing, skipped", async () => {
if (!Which("python3")) {
    skip++;
    print("test_scrape_body_cap_type: python3 missing, skipped");
} else {
    const T = makeTempDir("capty");
    const P = (x) => T + "/" + x;
    const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
    writeFile(new Path(P("srv.py")), [
        "import http.server, sys, os",
        "class H(http.server.BaseHTTPRequestHandler):",
        "    def do_GET(self):",
        "        body = b'x' * (20000 if self.path.startswith('/big') else 50)",
        "        self.send_response(200)",
        "        self.send_header('Content-Length', str(len(body)))",
        "        self.end_headers()",
        "        self.wfile.write(body)",
        "    def log_message(self, *a): pass",
        "s = http.server.HTTPServer(('127.0.0.1', 0), H)",
        "open(sys.argv[2], 'w').write(str(os.getpid()))",
        "open(sys.argv[1], 'w').write(str(s.server_address[1]))",
        "s.serve_forever()",
    ].join("\n"));
    spawnHelper(`python3 ${P("srv.py")} ${P("port")} ${P("pid")} >${P("srv.log")} 2>&1`);
    let PORT = 0;
    for (let i = 0; i < 80 && !PORT; i++) {
        sh("sleep 0.1");
        try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
    }
    check(PORT > 0, "the loopback server started");
    try {
    if (PORT) {
        const base = `http://127.0.0.1:${PORT}`;
        const c = new HTTPClient(1000);
        const e1 = await c.get(base + "/big").then(() => null, (e) => e);
        check(e1 instanceof RangeError && /too large/.test(e1.message), "HTTPClient over maxBody: RangeError (" + e1 + ")");
        check((await c.get(base + "/small")).status === 200, "CONTROL: a body under the cap is returned");
        c.close();
        const f = new Fetcher({ agent: "cap/1", robots: false, minDelayMs: 0, retries: 0, allowPrivateHosts: true, maxBodyBytes: 1000 });
        const e2 = await caught(() => f.get(base + "/big"));
        check(e2 instanceof RangeError, "Fetcher.get, declared Content-Length over maxBodyBytes: RangeError (" + e2 + ")");
        const e3 = await caught(() => f.getStream(base + "/big"));
        check(e3 instanceof RangeError, "Fetcher.getStream agrees: RangeError (" + e3 + ")");
        check((await f.get(base + "/small")).status === 200, "CONTROL: Fetcher.get under the cap");
        f.close();
    }
    } finally {
        try { sh(`kill ${readFile(new Path(P("pid")))} 2>/dev/null`); } catch (e) {}
    }
}
print("test_scrape_body_cap_type: " + n + " checks, " + fails + " failed, " + skip + " skipped");
if (fails) throw new Error("test_scrape_body_cap_type: " + fails + " failures");
});
__kitS.end();
