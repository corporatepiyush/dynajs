// timeout: 240
// covers: dyna:scrape.Fetcher
// dyna:scrape's built-in transport is dyna:http's (src/modules/http/http-conn.c,
// slots: 2
// parsing in src/core/httpk). Each case below was RED before that move:
//  B1 Fetcher.getStream over https never flushed the TLS write BIO
//     (no dyn_tls_pull), so the ClientHello was never sent: a 15 s stall,
//     then "TLS handshake: connection closed".
//  B2 Fetcher `ca` (path or inline PEM) was stored and shown in stats() but
//     never reached either transport; both trusted only the platform store.
//  B3 getStream and a concurrent Crawl threw "more than N redirects" for a
//     3xx WITHOUT Location once the hop budget was spent (a 304 at
//     maxRedirects 0); get() returned it. A 3xx with no Location is not a
//     redirect (RFC 9110 15.4; fetch 4.4), so the response is returned.
//  B4 getStream parsed chunk sizes with strtoll: " 5", "+5", "0x5" were
//     accepted. RFC 9112 7.1: chunk-size = 1*HEXDIG.
//  B5 (2026-11-26, async Fetcher) HTTPClient's non-blocking transport built a default trust
//     context per pooled connection, so an adopted `ca` never applied; once it did, an idle
//     connection verified against one client's `ca` must not be reused by a client that trusts
//     only the platform store. Pooled connections are keyed by their trust (the `ca` text).
//     Mutant: the pool ignores trust (B5 red).
// Controls: without `ca` the self-signed peer is still refused (verification
// was not switched off to make B1/B2 pass), and a well-formed chunked body
// still streams.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_fetcher_transport");
import { Fetcher, Crawl } from "dyna:scrape";
import { HTTPClient } from "dyna:http";
import { spawnHelper } from "../lib/helper.js";
import * as crypto from "dyna:crypto";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const skipped = (w) => { if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; } skip++; print("  SKIP  " + w); };
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
const grab = async (fn) => { try { return { v: await fn() }; } catch (e) { return { e }; } };

async function drain(st) {
    const buf = new Uint8Array(7), parts = [];
    for (;;) {
        const n = await st.read(buf);
        if (!n) break;
        parts.push(String.fromCharCode(...buf.subarray(0, n)));
    }
    st.close();
    return parts.join("");
}

async function main() {
    if (!Which("python3")) { skipped("python3 missing -- no peer to serve the cases"); return; }
    const T = makeTempDir("scrtx");
    const P = (n) => T + "/" + n;
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(P("k.pem")), k.privateKey);
    const cert = crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 2 });
    writeFile(new Path(P("c.pem")), cert);
    writeFile(new Path(P("srv.py")), [
        "import http.server, ssl, sys, threading, os, socketserver",
        "threading.Timer(150, lambda: os._exit(0)).start()",
        "class H(http.server.BaseHTTPRequestHandler):",
        "    protocol_version='HTTP/1.1'",
        "    def log_message(self,*a): pass",
        "    def raw(self, b):",
        "        self.wfile.write(b); self.wfile.flush(); self.close_connection=True",
        "    def do_GET(self):",
        "        p=self.path",
        "        if p=='/hello':",
        "            b=b'hello-tls'",
        "            self.send_response(200); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)",
        "        elif p=='/304':",
        "            self.raw(b'HTTP/1.1 304 Not Modified\\r\\nConnection: close\\r\\n\\r\\n')",
        "        elif p=='/302noloc':",
        "            self.raw(b'HTTP/1.1 302 Found\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nno')",
        "        elif p.startswith('/chunk/'):",
        "            sz={'hex0x':b'0x5','space':b' 5','plus':b'+5','junk':b'5 x','good':b'5','goodext':b'5 ;a=b'}[p[7:]]",
        "            self.raw(b'HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n'+sz+b'\\r\\nhello\\r\\n0\\r\\n\\r\\n')",
        "        else:",
        "            self.send_response(404); self.send_header('Content-Length','0'); self.end_headers()",
        "class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads=True",
        "plain=S(('127.0.0.1',0),H)",
        "tls=S(('127.0.0.1',0),H)",
        "ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[1], sys.argv[2])",
        "tls.socket=ctx.wrap_socket(tls.socket, server_side=True)",
        "threading.Thread(target=plain.serve_forever, daemon=True).start()",
        "threading.Thread(target=tls.serve_forever, daemon=True).start()",
        "open(sys.argv[3]+'.tmp','w').write('%d %d' % (plain.server_address[1], tls.server_address[1]))",
        "os.rename(sys.argv[3]+'.tmp', sys.argv[3])",
        "threading.Event().wait()",
    ].join("\n") + "\n");
    spawnHelper("env -u SSL_CERT_FILE -u SSL_CERT_DIR python3 " + P("srv.py") + " " + P("c.pem") + " " + P("k.pem") + " " + P("ports") + " >/dev/null 2>&1");
    let ports = null;
    for (let i = 0; i < 100 && !ports; i++) {
        const r = await grab(() => readFile(new Path(P("ports"))));
        if (r.v) ports = String(r.v).trim().split(" ").map(Number);
        else await new Promise((res) => setTimeout(res, 50));
    }
    if (!ports) { skipped("python TLS peer did not start"); removeAll(new Path(T)); return; }
    const H = "http://127.0.0.1:" + ports[0], S = "https://localhost:" + ports[1];
    const base = { agent: "transport-test/1.0", robots: false, allowPrivateHosts: true, retries: 0, minDelayMs: 0 };
    const pemText = String(readFile(new Path(P("c.pem"))));
    try {
        {
            const f = new Fetcher(Object.assign({ ca: P("c.pem") }, base));
            const t0 = Date.now();
            const r = await grab(() => f.getStream(S + "/hello"));
            ok(r.v && r.v.status === 200, "B1: getStream over https completes the handshake", r.e ? r.e.message + " after " + (Date.now() - t0) + "ms" : "");
            if (r.v) ok((await drain(r.v)) === "hello-tls", "B1: getStream over https streams the body");
            const g = await grab(() => f.get(S + "/hello"));
            ok(g.v && g.v.status === 200 && g.v.body === "hello-tls", "B2: get() trusts the `ca` path", g.e && g.e.message);
            f.close();
        }
        {
            const f = new Fetcher(Object.assign({ ca: pemText }, base));
            const r = await grab(() => f.getStream(S + "/hello"));
            ok(r.v && r.v.status === 200, "B2: getStream trusts an inline-PEM `ca`", r.e && r.e.message);
            if (r.v) r.v.close();
            const g = await grab(() => f.get(S + "/hello"));
            ok(g.v && g.v.status === 200, "B2: get() trusts an inline-PEM `ca`", g.e && g.e.message);
            f.close();
        }
        {
            const f = new Fetcher(Object.assign({ ca: pemText }, base));
            const g = await grab(() => f.get(S + "/hello"));
            ok(g.v && g.v.status === 200, "B5: the `ca` Fetcher leaves a verified connection idle in the pool", g.e && g.e.message);
            const plain = new HTTPClient();
            const p = await grab(() => plain.get(S + "/hello"));
            ok(p.e && /certificate|verify|TLS|handshake/i.test(p.e.message), "B5: a platform-trust client does not reuse it", p.v ? "got " + p.v.status : p.e.message);
            plain.close();
            f.close();
        }
        {
            const f = new Fetcher(base);
            const r = await grab(() => f.getStream(S + "/hello"));
            ok(r.e && /certificate|verify|TLS|handshake/i.test(r.e.message), "control: without `ca` getStream refuses the self-signed peer", r.v ? "got " + r.v.status : r.e.message);
            const g = await grab(() => f.get(S + "/hello"));
            ok(g.e && /certificate|verify|TLS|handshake/i.test(g.e.message), "control: without `ca` get() refuses the self-signed peer", g.v ? "got " + g.v.status : g.e.message);
            f.close();
        }
        {
            const f = new Fetcher(Object.assign({ maxRedirects: 0 }, base));
            const g = await grab(() => f.get(H + "/304"));
            ok(g.v && g.v.status === 304, "control: get() returns a 304 at maxRedirects 0", g.e && g.e.message);
            const r = await grab(() => f.getStream(H + "/304"));
            ok(r.v && r.v.status === 304, "B3: getStream returns a 304 at maxRedirects 0", r.e && r.e.message);
            if (r.v) r.v.close();
            const r2 = await grab(() => f.getStream(H + "/302noloc"));
            ok(r2.v && r2.v.status === 302, "B3: a 302 without Location is returned, not chased", r2.e && r2.e.message);
            if (r2.v) r2.v.close();
            const c = new Crawl(f, { maxPages: 1, concurrency: 2 }).start(H + "/304");
            let page = null, err = null;
            try { for await (const p of c) { page = p; break; } } catch (e) { err = e; }
            ok(page && page.status === 304, "B3: a concurrent Crawl emits the 304 page", err ? err.message : JSON.stringify(page && page.status));
            c.close();
            f.close();
        }
        {
            const f = new Fetcher(base);
            for (const bad of ["hex0x", "space", "plus", "junk"]) {
                const r = await grab(() => f.getStream(H + "/chunk/" + bad));
                let got = null, err = r.e || null;
                if (r.v) { try { got = await drain(r.v); } catch (e) { err = e; } }
                ok(err && /chunk/i.test(err.message), "B4: chunk-size \"" + bad + "\" is refused", got !== null ? "streamed " + JSON.stringify(got) : err && err.message);
            }
            for (const good of ["good", "goodext"]) {
                const r = await grab(() => f.getStream(H + "/chunk/" + good));
                let got = null;
                if (r.v) { try { got = await drain(r.v); } catch (e) { got = "ERR " + e.message; } }
                ok(got === "hello", "control: chunk-size \"" + good + "\" streams", got);
            }
            f.close();
        }
    } finally {
        sh("pkill -f " + P("srv.py") + " >/dev/null 2>&1");
        removeAll(new Path(T));
    }
}

await __kitS.section("main", async () => { await main(); });
__kitS.end();
print("test_scrape_fetcher_transport: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail) throw new Error("test_scrape_fetcher_transport: " + fail + " failures");
