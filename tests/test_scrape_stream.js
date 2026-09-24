/* test_scrape_stream.js -- + CC-2-scrape: Fetcher.getStream(url).
 *
 * The policy contract: getStream applies the SAME policy as buffered get()
 * -- robots gate, per-host delay floor, retries with Retry-After and
 * jittered backoff, redirect chase with the private-host and https-downgrade
 * gates, maxBodyBytes, the SSRF name gate -- and returns the body as a
 * dyna:stream-compatible ByteSource instead of a string. So every case here
 * is a POLICY case measured against a live mock:
 *
 *   - the wire shapes (content-length, chunked, EOF-framed, slow drips)
 *     stream through read(buf) -> Promise<number> (0 = end);
 *   - the object duck-types INTO dyna:stream (lines(), pipe()) without
 *     dyna:scrape importing dyna:stream;
 *   - the bounds refuse or reject: a declared Content-Length over
 *     maxBodyBytes refuses at open, received bytes crossing the cap reject
 *     the read (sticky), a connection ending before the declared length is
 *     a named truncation, never a silent 0;
 *   - buffered get() on the same fetcher is untouched (the default stays
 *     byte-identical);
 *   - an injected mock client changes nothing: the stream path owns its
 *     transport (the mock's buffered request contract cannot stream).
 *
 * Needs python3. A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { Fetcher } from "dyna:scrape";
import * as stream from "dyna:stream";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

if (!Which("python3")) {
    skipped("python3 missing -- the stream shapes cannot be served");
    print("test_scrape_stream: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_scrape_stream: " + fail + " failures");
} else {

const T = makeTempDir("d2stream");
const P = (n) => T + "/" + n;
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
writeFile(new Path(P("mock.py")), [
  "import http.server, sys, time",
  "import threading as _th, os as _os",
  "_th.Timer(120, lambda: _os._exit(0)).start()",
  "class H(http.server.BaseHTTPRequestHandler):",
  "    protocol_version='HTTP/1.1'",
  "    def log_message(self,*a): pass",
  "    def do_GET(self):",
  "        p=self.path",
  "        if p=='/robots.txt':",
  "            b=b'User-agent: *\\nAllow: /\\n'",
  "            self.send_response(200); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)",
  "        elif p=='/cl':",
  "            b=bytes(range(256))*64",
  "            self.send_response(200); self.send_header('Content-Type','application/octet-stream'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)",
  "        elif p=='/bigchunked':",
  "            self.send_response(200); self.send_header('Content-Type','text/plain'); self.send_header('Transfer-Encoding','chunked'); self.end_headers()",
  "            for i in range(2000):",
  "                self.wfile.write(b'%x\\r\\n%s\\r\\n' % (10, b'0123456789')); self.wfile.flush()",
  "            self.wfile.write(b'0\\r\\n\\r\\n')",
  "        elif p=='/chunked':",
  "            self.send_response(200); self.send_header('Content-Type','text/plain'); self.send_header('Transfer-Encoding','chunked'); self.end_headers()",
  "            for i in range(5):",
  "                self.wfile.write(b'%x\\r\\n%s\\r\\n' % (10, b'0123456789')); self.wfile.flush()",
  "            self.wfile.write(b'0\\r\\n\\r\\n')",
  "        elif p=='/truncated':",
  "            self.send_response(200); self.send_header('Content-Length','1000'); self.end_headers(); self.wfile.write(b'x'*400); self.wfile.flush(); self.close_connection=True",
  "        elif p=='/toolong':",
  "            self.send_response(200); self.send_header('Content-Length','99999999'); self.end_headers()",
  "        elif p=='/redirect':",
  "            self.send_response(302); self.send_header('Location','/cl'); self.send_header('Content-Length','0'); self.end_headers()",
  "        elif p=='/nolength':",
  "            self.send_response(200); self.send_header('Connection','close'); self.end_headers(); self.wfile.write(b'no framing at all'); self.close_connection=True",
  "        elif p=='/slow':",
  "            self.send_response(200); self.send_header('Content-Length','30'); self.end_headers()",
  "            for i in range(30):",
  "                self.wfile.write(b'%d' % (i%10)); self.wfile.flush(); time.sleep(0.02)",
  "        else:",
  "            self.send_response(404); self.send_header('Content-Length','0'); self.end_headers()",
  "srv=http.server.ThreadingHTTPServer(('127.0.0.1',0), H)",
  "open(sys.argv[1],'w').write(str(srv.server_address[1]))",
  "srv.serve_forever()",
].join("\n"));
sh(`python3 ${P("mock.py")} ${P("port")} >${P("mock.log")} 2>&1 &`);
let PORT = 0;
for (let i = 0; i < 60 && !PORT; i++) { sh("sleep 0.1"); try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {} }
const BASE = "http://127.0.0.1:" + PORT;
const mkF = (o) => new Fetcher(Object.assign({ agent: "streamprobe/1.0", minDelayMs: 0, robots: true, allowPrivateHosts: true }, o));

/* 1: content-length body, read to end */
{
  const f = mkF();
  const r = f.getStream(BASE + "/cl");
  ok(r.status === 200 && r.ok === true, "status/ok on the stream object");
  ok(r.contentType === "application/octet-stream", "contentType parsed");
  ok(typeof r.read === "function" && typeof r.close === "function", "duck-shape: read/close present");
  const buf = new Uint8Array(4096);
  let total = 0, first = -1, sum = 0;
  for (;;) {
    const n = await r.read(buf);
    if (n === 0) break;
    if (first < 0) { first = buf[0]; for (let i = 0; i < n; i++) sum += buf[i]; }
    total += n;
  }
  ok(total === 16384, "content-length body fully streamed (" + total + ")");
  ok(first === 0 && sum === 16 * 32640, "bytes are the promised bytes (sum " + sum + ")");
  const n2 = await r.read(buf);
  ok(n2 === 0, "read after EOF stays 0");
  r.close();
  ok(r.closed === true, "close() marks closed");
  f.close();
}
/* 2: chunked body via dyna:stream lines() -- the duck-typing integration */
{
  const f = mkF();
  const r = f.getStream(BASE + "/chunked");
  const lines = [];
  for await (const l of stream.lines(r, { encoding: "utf-8" })) lines.push(l);
  ok(lines.length === 1 && lines[0].length === 50, "dyna:stream lines() consumed the chunked body (" + lines.length + " line, " + (lines[0] || "").length + " chars)");
  f.close();
}
/* 3: pipe -> a counting sink */
{
  const f = mkF();
  const r = f.getStream(BASE + "/chunked");
  let total = 0;
  const sink = { async write(b) { total += b.length; return b.length; }, async flush() {}, close() {} };
  const n = await stream.pipe(r, sink);
  ok(n === 50, "dyna:stream pipe() counted " + n + " bytes (want 50)");
  f.close();
}
/* 4: truncation detected (Content-Length 1000, 400 sent) */
{
  const f = mkF();
  const r = f.getStream(BASE + "/truncated");
  const buf = new Uint8Array(300);
  let got = 0, rejected = false, msg = "";
  for (;;) {
    try { const n = await r.read(buf); if (n === 0) break; got += n; }
    catch (e) { rejected = true; msg = String(e); break; }
  }
  ok(got === 300, "truncated body delivers its successful reads first (" + got + "); the failing read owns its own partial bytes");
  ok(rejected && /truncat/i.test(msg), "then the read REJECTS naming the truncation [" + msg.slice(0, 90) + "]");
  f.close();
}
/* 5: declared Content-Length over maxBodyBytes refuses at open */
{
  const f = mkF({ maxBodyBytes: 5000 });
  let threw = false;
  try { f.getStream(BASE + "/toolong"); } catch (e) { threw = /maxBodyBytes/.test(String(e)); }
  ok(threw, "declared CL over the cap refuses at open");
  f.close();
}
/* 6: maxBody enforced ON the stream (chunked, cap crossed mid-body) */
{
  const f = mkF({ maxBodyBytes: 1000 });
  const r = f.getStream(BASE + "/bigchunked");
  const buf = new Uint8Array(4096);
  let got = 0, rejected = false, msg = "";
  for (;;) {
    try { const n = await r.read(buf); if (n === 0) break; got += n; }
    catch (e) { rejected = true; msg = String(e); break; }
  }
  ok(rejected && /maxBodyBytes/.test(msg), "cap enforced on received bytes after " + got + " [" + msg.slice(0, 60) + "]");
  ok(got <= 4096 + 1000, "the cap actually bounded the delivered bytes (" + got + ")");
  let rejected2 = false;
  try { await r.read(buf); } catch (e) { rejected2 = true; }
  ok(rejected2, "read after failure rejects (sticky)");
  f.close();
}
/* 7: redirect chased */
{
  const f = mkF();
  const r = f.getStream(BASE + "/redirect");
  ok(r.status === 200 && r.url === BASE + "/cl", "redirect chased to the final url (" + r.url + ")");
  const buf = new Uint8Array(512);
  const n = await r.read(buf);
  ok(n === 512 && buf[0] === 0, "the redirected body streams");
  r.close();
  f.close();
}
/* 8: read-to-EOF framing (no length, no chunked) */
{
  const f = mkF();
  const r = f.getStream(BASE + "/nolength");
  const all = [];
  const buf = new Uint8Array(7);
  for (;;) { const n = await r.read(buf); if (n === 0) break; all.push(...buf.slice(0, n)); }
  ok(all.length === 17 && String.fromCharCode(...all) === "no framing at all", "EOF-framed body reads through (" + all.length + " bytes)");
  f.close();
}
/* 9: a slowly dripping body is read incrementally, not buffered whole */
{
  const f = mkF();
  const r = f.getStream(BASE + "/slow");
  const buf = new Uint8Array(4);
  let reads = 0, total = 0;
  for (;;) { const n = await r.read(buf); if (n === 0) break; reads++; total += n; }
  ok(total === 30, "slow body fully read (" + total + ")");
  ok(reads >= 6, "arrived in " + reads + " reads (incremental, not one buffer)");
  r.close();
  f.close();
}
/* 10: buffered get() unchanged on the same fetcher shape */
{
  const f = mkF();
  const g = f.get(BASE + "/cl");
  ok(g.status === 200 && g.bodyBytes.byteLength === 16384, "buffered get() byte-identical on the same fetcher");
  f.close();
}
/* 11: a mock client (injected) does not break getStream -- it has its own transport */
{
  const mock = { request(method, url) {
    if (url.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
    return { status: 200, headers: { "Content-Type": "text/mock" }, body: "ignored" };
  } };
  const f = mkF({ client: mock, robots: false });
  const r = f.getStream(BASE + "/chunked");
  ok(r.status === 200, "getStream uses its own transport even with an injected mock client");
  const buf = new Uint8Array(100);
  let total = 0;
  for (;;) { const n = await r.read(buf); if (n === 0) break; total += n; }
  ok(total === 50, "and the bytes come from the wire, not the mock (" + total + ")");
  f.close();
}
    print("test_scrape_stream: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_scrape_stream: " + fail + " failures");
}
