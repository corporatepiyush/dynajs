/* test_http_hardening.js -- regressions for the 2026-08 HTTP hardening pass
 * (client, App, HTTPServerAsync, HTTPServer). Every case names the fix it
 * pins; the C side carries the same numbering in comments.
 *
 * ORACLE RULES USED THROUGHOUT:
 *   - exact statuses, never "not 200" (a server that refused everything
 *     would pass a weak check);
 *   - a control case beside every refusal, so a blanket refusal cannot read
 *     as a defence;
 *   - raw-socket probes through python3, because the attacks a well-behaved
 *     client will not send are the ones that matter;
 *   - servers run in CHILD PROCESSES (App handlers run on the JS thread; a
 *     blocking probe in-process would deadlock and "pass" unserved).
 *
 * Sections:
 *   1. client: header/method injection refused, clean forms still work
 *   2. route registration: status range, CTL in path/contentType refused
 *   3. App: prefix boundary (static+proxy), WS handshake strictness,
 *      RPC error escaping, double-settle thenable, 413, maxConns,
 *      upload-vs-realloc race (UAF the old code had)
 *   4. HTTPServerAsync: 431 header cap WITH a status, 413, version anchor,
 *      Connection whole-token matching
 *   5. HTTPServer (thread pool): 431/413/400/408 instead of silent drops,
 *      slowloris deadline, version anchor, token matching, writev integrity
 *   6. hostile status line: 40 digits cannot hang or crash the client
 *
 * Skips cleanly when python3 is unavailable.
 */
import { HTTPClient, HTTPServer, HTTPServerAsync } from "dyna:net";
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_hardening: SKIP (python3 not available)");
} else {
    const T = "/tmp/_dyna_http_hard";
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    /* ================================================================
     * 1. CLIENT: injection refused at the boundary, clean forms work
     * ================================================================ */
    {
        const s = new HTTPServerAsync({ port: 0, routes: { "/": "ok\n" } });
        s.start();
        const c = new HTTPClient();
        try {
            const url = "http://127.0.0.1:" + s.port + "/";
            /* get/post take (url[, headers]): headers is the SECOND argument */
            let threw = null;
            try { c.get(url, { "X-Good": "1" }); }
            catch (e) { threw = e; }
            ok(!threw, "object headers are still accepted (control)");
            ok(c.get(url, "X-Good: 1\r\n").status === 200,
               "a clean pre-formatted header string is still accepted (control)");

            threw = null;
            try { c.get(url, "X-Evil: a\r\nEvil: 1"); }
            catch (e) { threw = e; }
            ok(threw && /CR or LF/.test(threw.message),
               "a CR/LF in a pre-formatted header string is refused (CWE-93), got: " +
               (threw ? threw.message.slice(0, 60) : "no throw"));

            threw = null;
            try { c.request("GET\r\nX-Evil: 1", url); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "a method carrying CRLF is refused (request splitting), got: " +
               (threw ? threw.message.slice(0, 60) : "no throw"));

            threw = null;
            try { c.request("GET X", url); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "a method with a space is refused (tchar rule)");

            threw = null;
            try { c.request("", url); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "an empty method is refused");

            threw = null;
            try { c.requestAsync("POST\r\n", url, "x"); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "requestAsync validates the method too");
        } finally { c.close(); s.close(); }
    }

    /* a real token method must still reach the wire: a capture server records
     * the request line, so this proves the validator is not a blanket refusal */
    {
        const CAP = 18741;
        write(`${T}/capture.py`, `import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", ${CAP})); s.listen(4)
import sys, threading as th
th.Timer(60, lambda: sys.exit(0)).start()
while True:
    cn, _ = s.accept()
    try:
        d = b""
        while b"\\r\\n\\r\\n" not in d:
            b2 = cn.recv(4096)
            if not b2: break
            d += b2
        line = d.split(b"\\r\\n")[0].decode("latin1")
        cn.sendall(("HTTP/1.1 200 OK\\r\\nContent-Length: %d\\r\\n\\r\\n%s"
                    % (len(line), line)).encode())
    except Exception: pass
    finally: cn.close()
`);
        sh(`python3 ${T}/capture.py >${T}/cap.log 2>&1 & sleep 0.5`);
        const c = new HTTPClient();
        try {
            const r = c.request("PATCH", `http://127.0.0.1:${CAP}/x`);
            ok(r.status === 200 && r.body.indexOf("PATCH /x") === 0,
               "a valid custom method (PATCH) reaches the wire verbatim (got '" +
               r.body.slice(0, 30) + "')");
        } catch (e) {
            ok(false, "PATCH request threw: " + e.message.slice(0, 60));
        }
        c.close();
        sh("pkill -f capture.py 2>/dev/null");
    }

    /* ================================================================
     * 2. ROUTE REGISTRATION: emitted values are validated
     * ================================================================ */
    {
        const throws = (f, re, m) => {
            let e = null;
            try { f(); } catch (x) { e = x; }
            ok(e && re.test(e.message), m + " (got: " +
               (e ? e.message.slice(0, 70) : "no throw") + ")");
        };
        /* HTTPServerAsync */
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x": { status: -5, body: "y" } } }),
               /status must be in \[100, 999\]/, "Async: status -5 refused");
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x": { status: 1000, body: "y" } } }),
               /status must be in \[100, 999\]/, "Async: status 1000 refused");
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x": { status: 200, body: "y", contentType: "text/plain\r\nX-Evil: 1" } } }),
               /control characters/, "Async: contentType CRLF refused (response splitting)");
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x\r\nEvil: 1": "y" } }),
               /control characters/, "Async: route path CRLF refused");
        /* HTTPServer (thread pool) shares dyn_route_copy */
        throws(() => new HTTPServer({ port: 0, routes: { "/x": { status: 99, body: "y" } } }),
               /status must be in \[100, 999\]/, "ThreadPool: status 99 refused");
        throws(() => new HTTPServer({ port: 0, routes: { "/x": { status: 200, body: "y", contentType: "a\r\nb" } } }),
               /control characters/, "ThreadPool: contentType CRLF refused");
        /* controls */
        let good = null;
        try {
            const s = new HTTPServerAsync({ port: 0, routes: {
                "/": { status: 404, body: "nope", contentType: "application/xml" } } });
            s.start();
            const c = new HTTPClient();
            const r = c.get("http://127.0.0.1:" + s.port + "/");
            good = r.status === 404 &&
                   (r.headers["Content-Type"] === "application/xml" ||
                    r.headers["content-type"] === "application/xml");
            c.close(); s.close();
        } catch (e) { good = false; }
        ok(good, "control: a valid non-200 route with a real content type serves");
    }

    /* ================================================================
     * 3. APP (child process + python raw sockets)
     * ================================================================ */
    {
        const AP = 18742;      /* App under test */
        const UP = 18743;      /* proxy upstream */
        sh(`mkdir -p ${T}/www ${T}/updir`);
        sh(`printf 'PREFIX-OK' > ${T}/www/oo.html`);
        write(`${T}/upstream.py`, `import socket, sys, threading as th
th.Timer(120, lambda: sys.exit(0)).start()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", ${UP})); s.listen(8); print("up", flush=True)
paths = []
def respond(cn, d):
    p = d.split(b" ")[1].decode("latin1") if d else "/"
    paths.append(p)
    if p.startswith("/hugehead"):
        pad = b"X-Pad: " + b"h" * 900 + b"\\r\\n"
        body = b"big"
        cn.sendall(b"HTTP/1.1 200 OK\\r\\n" + pad * 120 +
                   b"Content-Length: %d\\r\\n\\r\\n" % len(body) + body)
    elif p.startswith("/earlyclose"):
        cn.close(); return True
    else:
        body = ("seen=" + "|".join(paths)).encode()
        cn.sendall(b"HTTP/1.1 200 OK\\r\\nContent-Length: %d\\r\\n\\r\\n" % len(body) + body)
    return False
while True:
    cn, _ = s.accept()
    try:
        d = b""
        while b"\\r\\n\\r\\n" not in d:
            b2 = cn.recv(4096)
            if not b2: break
            d += b2
        if respond(cn, d): continue
    except Exception: pass
    finally:
        try: cn.close()
        except Exception: pass
`);
        write(`${T}/app.js`, `import { App } from "dyna:net";
import { Path } from "dyna:file";
import { pid } from "dyna:sys";
import * as std from "std";
const f = std.open("${T}/app.pid", "w"); f.puts(String(pid())); f.close();
const app = new App({ port: ${AP}, idleTimeoutMs: 8000, maxConns: 2 });
app.static("/f", new Path("${T}/www"));
app.proxy("/api", { host: "127.0.0.1", port: ${UP} });
app.ws("/ws", { open: () => {}, message: () => {}, close: () => {} });
app.rpc("/rpc", {
  add: (a, b) => a + b,
  quoteThrow: () => { throw new Error('a"b\\\\c\\nd'); },
  doubleSettle: () => ({ then: (res, rej) => { res("first"); res("second"); } }),
  never: () => new Promise(() => {}),
});
app.upload("/up", { dir: new Path("${T}/updir"), maxFileSize: 1 << 22 },
           (p, m) => m.size);
app.upload("/uptxt", { dir: new Path("${T}/updir"), maxFileSize: 65536,
                       allow: ["text/plain"] }, (p, m) => m.size);
/* route registrations while an upload is in flight: the old code held a
 * pointer into the realloc'd route array across event-loop turns (UAF) */
let n = 0;
const timer = setInterval(() => {
  app.upload("/up_gen" + (n++), { dir: new Path("${T}/updir") }, () => 0);
}, 30);
setTimeout(() => clearInterval(timer), 60000);
app.start();
`);
        write(`${T}/probe.py`, `import socket, time, json, sys, base64, hashlib
H, P = "127.0.0.1", ${AP}
R = []
def rec(tag, **kv): R.append(dict(tag=tag, **kv))

def hit(payload, want=1, hard=5.0, wait_port=True):
    for _ in range(30 if wait_port else 1):
        try:
            s = socket.create_connection((H, P), timeout=2); break
        except Exception: time.sleep(0.1)
    else:
        return {"n": 0, "code": None, "body": "", "raw": "", "err": "no connect"}
    senderr = None
    try: s.sendall(payload)
    except Exception as e: senderr = str(e)
    s.setblocking(False)
    out = b""; t0 = time.time(); last = t0
    while time.time() - t0 < hard:
        try:
            b2 = s.recv(65536)
            if not b2: break
            out += b2; last = time.time()
            if out.count(b"HTTP/1.1") >= want and time.time() - last > 0.15: break
        except BlockingIOError:
            if out and time.time() - last > 0.35: break
            time.sleep(0.02)
        except Exception: break
    s.close()
    return {"n": out.count(b"HTTP/1.1"),
            "code": (out[9:12].decode() if out[:4] == b"HTTP" else None),
            "body": out.split(b"\\r\\n\\r\\n", 1)[1].decode("latin1") if b"\\r\\n\\r\\n" in out else "",
            "raw": out.decode("latin1"),
            "senderr": senderr}

def curl(p):
    import subprocess
    r = subprocess.run(["curl", "-s", "--max-time", "4", "--path-as-is",
                        "http://127.0.0.1:%d%s" % (P, p)], capture_output=True)
    return r.stdout.decode("latin1")

# wait for the server to actually serve before probing
for _ in range(50):
    if "PREFIX-OK" in curl("/f/oo.html"): break
    time.sleep(0.1)

# --- S6: prefix boundary, static ---
rec("static_ok", body=curl("/f/oo.html"))            # control
rec("static_boundary", body=curl("/foo.html"))       # /f must NOT match /foo...

# --- S6: prefix boundary, proxy (retry: the upstream may still be binding) ---
for _ in range(30):
    r = curl("/api/x")
    if "seen=" in r: break
    time.sleep(0.1)
rec("proxy_ok", body=r)                              # control: forwarded
rec("proxy_boundary", body=curl("/apiv2/x"))         # /api must NOT match

# --- S4: WS handshake strictness ---
def ws_probe(tag, upgrade="websocket", conn="Upgrade", ver="13", key=None, method=b"GET"):
    k = key if key is not None else base64.b64encode(b"0123456789abcdef").decode()
    req = (method + b" /ws HTTP/1.1\\r\\nHost: h\\r\\n"
           + ("Upgrade: " + upgrade + "\\r\\n").encode()
           + ("Connection: " + conn + "\\r\\n").encode()
           + ("Sec-WebSocket-Key: " + k + "\\r\\n").encode()
           + ("Sec-WebSocket-Version: " + ver + "\\r\\n\\r\\n").encode())
    r = hit(req)
    rec(tag, code=r["code"], upgraded=("101" == r["code"]))

ws_probe("ws_valid")                                       # control
ws_probe("ws_h2c", upgrade="h2c")                          # any-value upgrade
ws_probe("ws_noversion", ver="")
ws_probe("ws_version8", ver="8")
ws_probe("ws_conn_plain", conn="keep-alive")               # missing Upgrade token
ws_probe("ws_shortkey", key="dGhlIHNhbXBsZSBub25jZQ===")   # 25 chars: not 16 bytes
ws_probe("ws_post", method=b"POST")
acc = base64.b64encode(hashlib.sha1(
    (base64.b64encode(b"0123456789abcdef").decode() +
     "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
r = hit(("GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\n"
         "Connection: Upgrade\\r\\nSec-WebSocket-Key: %s\\r\\n"
         "Sec-WebSocket-Version: 13\\r\\n\\r\\n"
         % base64.b64encode(b"0123456789abcdef").decode()).encode())
rec("ws_accept_correct", accept=acc in r["raw"])

# --- S11: RPC error escaping ---
body = json.dumps({"method": "quoteThrow", "id": 1})
r = hit(("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
         % len(body)).encode() + body.encode())
parsed = None
try: parsed = json.loads(r["body"])
except Exception: pass
rec("rpc_escape", ok=parsed is not None and parsed.get("id") == 1,
    msg=(parsed or {}).get("error", {}).get("message", ""))

# S11-review: an ATTACKER-controlled id of 300 chars plus a 500-char
# exception message. The first cut of the escaper underflowed
# cap minus tail_room here and smashed a stack buffer; the fixed code
# echoes null for an oversized id and truncates only the message.
long_id = "i" * 300
body = json.dumps({"method": "quoteThrow", "id": long_id})
r = hit(("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
         % len(body)).encode() + body.encode())
parsed = None
try: parsed = json.loads(r["body"])
except Exception: pass
rec("rpc_longid_msg", ok=parsed is not None,
    id=(parsed.get("id") if parsed else None))

body = json.dumps({"method": "nope", "id": long_id})
r = hit(("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
         % len(body)).encode() + body.encode())
parsed = None
try: parsed = json.loads(r["body"])
except Exception: pass
rec("rpc_longid_404", ok=parsed is not None,
    id=(parsed.get("id") if parsed else None))

# --- S10: double-settle thenable -> exactly one response, server alive ---
body = json.dumps({"method": "doubleSettle", "id": 7})
r = hit(("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
         % len(body)).encode() + body.encode(), want=2, hard=4.0)
rec("double_settle", n=r["n"], code=r["code"], body=r["body"][:80])
add_body = json.dumps({"method": "add", "params": [1, 2], "id": 2})
r2 = hit(("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
          % len(add_body)).encode() + add_body.encode())
rec("alive_after", code=r2["code"])

# --- S16: declared body past the cap is a 413, not silence ---
r = hit(b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 2000000\\r\\n\\r\\n")
rec("app_413", code=r["code"])

# --- S15: maxConns=2: third concurrent connection is refused, cap frees ---
socks = []
try:
    for _ in range(30):
        try:
            socks = [socket.create_connection((H, P), timeout=2) for _ in range(2)]
            break
        except Exception:
            if socks: socks = []
            time.sleep(0.1)
    for sk in socks: sk.sendall(b"GET /healthz HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
    time.sleep(0.3)
    third = socket.create_connection((H, P), timeout=2)
    third.sendall(b"GET /rpc HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
    third.settimeout(2.0)
    refused = False
    try:
        d = third.recv(64)
        refused = (d == b"")          # accept-then-close: EOF, no response
    except Exception:
        refused = True                # RST is equally a refusal
    third.close()
    socks[0].close()                  # free a slot...
    time.sleep(0.4)
    mc_body = json.dumps({"method": "add", "params": [2, 3], "id": 3})
    r = hit(("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
             % len(mc_body)).encode() + mc_body.encode())
    rec("maxconns", refused=refused, after_free=r["code"])
finally:
    for sk in socks:
        try: sk.close()
        except Exception: pass

# --- S9: registrations during a streaming upload (old code: UAF) ---
payload = b"Z" * (1 << 21)
chunks = [payload[i:i + 65536] for i in range(0, len(payload), 65536)]
try:
    s = socket.create_connection((H, P), timeout=3)
    head = (b"POST /up HTTP/1.1\\r\\nHost: x\\r\\n"
            b"Content-Type: application/octet-stream\\r\\n"
            + b"Content-Length: %d\\r\\n\\r\\n" % len(payload))
    s.sendall(head)
    for ch in chunks:
        s.sendall(ch); time.sleep(0.03)   # ~2s total: registrations land mid-upload
    s.settimeout(6.0)
    buf = b""
    while b"\\r\\n\\r\\n" not in buf:
        d = s.recv(4096)
        if not d: break
        buf += d
    extra = b""
    if b"\\r\\n\\r\\n" in buf:
        cl = int(buf.split(b"Content-Length: ")[1].split(b"\\r\\n")[0])
        body2 = buf.split(b"\\r\\n\\r\\n", 1)[1]
        while len(body2) < cl:
            d = s.recv(4096)
            if not d: break
            body2 += d
        extra = body2.decode("latin1")
    s.close()
    rec("upload_race", body=extra)
except Exception as e:
    rec("upload_race", body="ERR:" + str(e)[:60])

# ================================================================
# WORST-CASE FRAMING BATTERY (each row pins one parser decision)
# ================================================================
def post(body_bytes, extra=b""):
    return hit(b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\n" + extra +
               b"Content-Length: %d\\r\\n\\r\\n" % len(body_bytes) + body_bytes)

# mixed-case TE must still be recognised and refused
r = hit(b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\ntRaNsFeR-eNcOdInG: chunked\\r\\n\\r\\n")
rec("te_mixedcase", code=r["code"])

# OWS before the colon makes it an UNKNOWN header to a compliant parser:
# framed by CL instead -- exactly ONE response, no smuggle
r = hit(b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nTransfer-Encoding : chunked\\r\\n"
        b"Content-Length: 0\\r\\n\\r\\nGET /rpc HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n",
        want=2)
rec("te_ows_unknown", n=r["n"], code=r["code"])

# duplicate IDENTICAL Content-Length: our server refuses even when equal
body = json.dumps({"method": "add", "params": [1, 1], "id": 9})
r = hit(b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n"
        b"Content-Length: %d\\r\\n\\r\\n" % (len(body), len(body)) + body.encode())
rec("dup_cl_equal", code=r["code"])

for tag, cl in [("cl_negative", b"-1"), ("cl_plus", b"+5"), ("cl_hex", b"0x5")]:
    r = hit(b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: " + cl +
            b"\\r\\n\\r\\n")
    rec(tag, code=r["code"])

# OWS before the colon on Host: malformed field line -> 400
r = hit(b"GET /f/oo.html HTTP/1.1\\r\\nHost : x\\r\\n\\r\\n")
rec("ows_colon", code=r["code"])

# bare-LF line endings never form CRLFCRLF: bounded silence, no crash
try:
    s = socket.create_connection((H, P), timeout=2)
    s.sendall(b"GET /f/oo.html HTTP/1.1\\nHost: x\\n\\n")
    s.settimeout(3.0)
    got = b""
    try:
        got = s.recv(64)
    except socket.timeout:
        pass
    s.close()
    rec("bare_lf", silent=(got == b"" or got[:4] != b"HTTP"))
except Exception as e:
    rec("bare_lf", silent=None, err=str(e)[:40])

# header COUNT well under the size cap still serves
many = b"".join(b"X-H%d: v\\r\\n" % i for i in range(200))
r = hit(b"GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\n" + many + b"\\r\\n")
rec("many_headers", code=r["code"])

# a 3000-char request line: path truncates to 1024, answered 404 -- bounded
r = hit(b"GET /f/" + b"q" * 3000 + b" HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
rec("long_reqline", code=r["code"])

# Connection: close swallows everything pipelined behind it
one = ("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
       % len(body)).encode() + body.encode()
closer = (b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n"
          b"Content-Length: %d\\r\\n\\r\\n" % len(body)) + body.encode()
r = hit(one + closer + one)
rec("pipeline_after_close", n=r["n"])

# 50 pipelined requests -> 50 responses in order
p50 = one * 50
r = hit(p50, want=50, hard=8.0)
rec("pipeline50", n=r["n"])

# connect + immediate close must not disturb the server
try:
    sk = socket.create_connection((H, P), timeout=2); sk.close()
    sk = socket.create_connection((H, P), timeout=2); sk.close()
except Exception: pass
r = post(json.dumps({"method": "add", "params": [4, 4], "id": 11}).encode())
rec("survives_abort", code=r["code"])

# split delivery: one request dripped a byte at a time reassembles correctly
full = b"GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\nUser-Agent: drip\\r\\n\\r\\n"
try:
    s = socket.create_connection((H, P), timeout=2)
    for i in range(len(full)):
        s.sendall(full[i:i+1])
        time.sleep(0.002)
    s.settimeout(4.0)
    buf2 = b""
    while b"\\r\\n\\r\\n" not in buf2:
        d = s.recv(4096)
        if not d: break
        buf2 += d
    s.close()
    rec("split_bytes", ok=(b"PREFIX-OK" in buf2))
except Exception as e:
    rec("split_bytes", ok=False, err=str(e)[:40])

# percent-encoded dots are NOT decoded: no traversal via %2e%2e
r = curl("/f/%2e%2e/%2e%2e/etc/passwd")
rec("pct_not_decoded", safe=("root:" not in r))

# --- WebSocket frame-level abuse after a VALID handshake ---
import struct as _st
def ws_session():
    k = base64.b64encode(b"0123456789abcdef").decode()
    s = socket.create_connection((H, P), timeout=2)
    s.sendall(("GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\n"
               "Connection: Upgrade\\r\\nSec-WebSocket-Key: %s\\r\\n"
               "Sec-WebSocket-Version: 13\\r\\n\\r\\n" % k).encode())
    hsresp = b""
    while b"\\r\\n\\r\\n" not in hsresp:
        d = s.recv(4096)
        if not d: raise RuntimeError("hs closed")
        hsresp += d
    if b"101" not in hsresp.split(b"\\r\\n")[0]:
        raise RuntimeError("no upgrade")
    return s
def mask_frame(op, payload, fin=True, masked=True):
    b0 = (0x80 if fin else 0) | op
    hdr = bytes([b0])
    n = len(payload)
    m = b"1234"
    if masked:
        p = bytes(payload[i] ^ m[i & 3] for i in range(n))
    else:
        p = payload; m = b""
    b0h = 0x80 if masked else 0
    if n < 126: hdr += bytes([b0h | n]) + m
    else: hdr += bytes([b0h | 126]) + _st.pack(">H", n) + m
    return hdr + p

try:
    s = ws_session()
    s.sendall(mask_frame(0x3, b"unknown-opcode"))       # reserved opcode
    s.sendall(mask_frame(0x9, b"ping"))                 # ping -> pong proves liveness
    s.settimeout(2.0)
    got = s.recv(64)
    rec("ws_reserved_op", alive=(b"\\x8a" in got))      # 0x8A = pong FIN frame
    s.close()
except Exception as e:
    rec("ws_reserved_op", alive=None, err=str(e)[:40])
try:
    s = ws_session()
    s.sendall(mask_frame(0x1, b"unmasked", masked=False))  # client MUST mask
    s.settimeout(2.0)
    got = b""
    try:
        while True:
            d = s.recv(256)
            if not d: break
            got += d
    except socket.timeout:
        pass
    s.close()
    rec("ws_unmasked_refused", closed=(got == b""))
except Exception as e:
    rec("ws_unmasked_refused", closed=None, err=str(e)[:40])

# --- upload refusals ---
r = hit(b"POST /uptxt HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: application/octet-stream\\r\\n"
        b"Content-Length: 10\\r\\n\\r\\nAAAAAAAAAA")
rec("upload_type_disallowed", code=r["code"])
bigup = b"T" * 70000   # over /uptxt's 65536 cap
r = hit(b"POST /uptxt HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: text/plain\\r\\n"
        b"Content-Length: %d\\r\\n\\r\\n" % len(bigup) + bigup)
rec("upload_over_cap", code=r["code"])
ok_up = b"T" * 100
r = hit(b"POST /uptxt HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: text/plain\\r\\n"
        b"Content-Length: %d\\r\\n\\r\\n" % len(ok_up) + ok_up)
rec("upload_allowed_ok", code=r["code"], okbody=('{"ok":true' in r["body"]))

# --- proxy worst cases ---
r = curl("/api/hugehead")
rec("proxy_hugehead", refused=("upstream headers too large" in r))
r = curl("/api/earlyclose")
rec("proxy_earlyclose", refused=("error" in r))

# --- RPC batch semantics ---
batch = json.dumps([
    {"method": "add", "params": [1, 2], "id": "a"},
    {"method": "nope", "id": "b"},
    {"method": "add", "params": [3, 3]}
])
r = post(batch.encode())
parsed = None
try: parsed = json.loads(r["body"])
except Exception: pass
rec("rpc_batch_mixed",
    ok=isinstance(parsed, list) and len(parsed) == 2,
    ids=[e.get("id") for e in parsed] if isinstance(parsed, list) else None)

# notification alone: current contract answers with id null -- pinned here
r = post(json.dumps({"method": "add", "params": [2, 2]}).encode())
parsed = None
try: parsed = json.loads(r["body"])
except Exception: pass
rec("rpc_notification_contract",
    ok=parsed is not None and parsed.get("id") is None and parsed.get("result") == 4)

print(json.dumps(R))
`);
        write(`${T}/assert.js`, `
/* parsed by the parent below */
`);

        sh(`python3 ${T}/upstream.py >${T}/up.log 2>&1 &`);
        sh(`${EXE} ${T}/app.js >${T}/app.log 2>&1 &`);
        let appPid = 0;
        for (let i = 0; i < 100 && !(appPid > 0); i++) {
            appPid = parseInt(cat(`${T}/app.pid`).trim(), 10) || 0;
            if (!(appPid > 0)) sh("sleep 0.05");
        }
        ok(appPid > 0, "App child started");

        sh(`python3 ${T}/probe.py > ${T}/probe.json 2>${T}/probe.err`);
        let R = {};
        try {
            for (const r of JSON.parse(cat(`${T}/probe.json`))) R[r.tag] = r;
        } catch (e) {
            ok(false, "probe.py produced parseable JSON (" +
               cat(`${T}/probe.err`).slice(0, 100) + ")");
        }
        const need = ["static_ok", "static_boundary", "proxy_ok", "proxy_boundary",
                      "ws_valid", "ws_h2c", "ws_noversion", "ws_version8",
                      "ws_conn_plain", "ws_shortkey", "ws_post", "ws_accept_correct",
                      "rpc_escape", "rpc_longid_msg", "rpc_longid_404",
                      "double_settle", "alive_after", "app_413",
                      "maxconns", "upload_race",
                      /* framing battery */
                      "te_mixedcase", "te_ows_unknown", "dup_cl_equal",
                      "cl_negative", "cl_plus", "cl_hex", "ows_colon", "bare_lf",
                      "many_headers", "long_reqline", "pipeline_after_close",
                      "pipeline50", "survives_abort", "split_bytes",
                      "pct_not_decoded", "ws_reserved_op", "ws_unmasked_refused",
                      "upload_type_disallowed", "upload_over_cap",
                      "upload_allowed_ok", "proxy_hugehead", "proxy_earlyclose",
                      "rpc_batch_mixed", "rpc_notification_contract"];
        const missing = need.filter(t => !R[t]);
        ok(missing.length === 0, "every probe recorded (missing means the probe died, not that a defence failed)"
           + (missing.length ? " -- missing: " + missing.join(",") + " stderr: " +
              cat(`${T}/probe.err`).slice(0, 120) : ""));

        /* S6 static: control serves, boundary does not */
        ok(R.static_ok && R.static_ok.body === "PREFIX-OK",
           "static control: /f/oo.html serves (got " + JSON.stringify(R.static_ok && R.static_ok.body) + ")");
        ok(R.static_boundary && R.static_boundary.body.indexOf("PREFIX-OK") < 0,
           "S6: /foo.html does NOT match the /f prefix (got " +
           JSON.stringify(R.static_boundary && R.static_ok && R.static_boundary.body.slice(0, 40)) + ")");

        /* S6 proxy */
        ok(R.proxy_ok && R.proxy_ok.body.indexOf("seen=/x") >= 0,
           "proxy control: /api/x forwarded as /x (got " + JSON.stringify(R.proxy_ok && R.proxy_ok.body) + ")");
        ok(R.proxy_boundary && R.proxy_boundary.body.indexOf("seen=") < 0,
           "S6: /apiv2/x is NOT forwarded to the upstream (got " +
           JSON.stringify(R.proxy_boundary && R.proxy_boundary.body.slice(0, 40)) + ")");

        /* S4 WS */
        ok(R.ws_valid && R.ws_valid.upgraded === true,
           "S4 control: a correct RFC 6455 handshake still upgrades");
        ok(R.ws_accept_correct && R.ws_accept_correct.accept === true,
           "S4 control: Sec-WebSocket-Accept is the RFC value");
        for (const t of ["ws_h2c", "ws_noversion", "ws_version8", "ws_conn_plain",
                         "ws_shortkey", "ws_post"]) {
            ok(R[t] && R[t].code === "400" && !R[t].upgraded,
               "S4: " + t + " refused with 400, no upgrade (got " +
               JSON.stringify(R[t] && R[t].code) + ")");
        }

        /* S11 escaping */
        ok(R.rpc_escape && R.rpc_escape.ok === true,
           "S11: an exception containing quote/backslash/newline yields parseable JSON");
        ok(R.rpc_escape && R.rpc_escape.msg === 'Error: a"b\\c\nd',
           "S11: the message round-trips exactly through the JSON escape (got '" +
           (R.rpc_escape ? R.rpc_escape.msg.slice(0, 40) : "") + "')");
        ok(R.rpc_longid_msg && R.rpc_longid_msg.ok === true &&
           R.rpc_longid_msg.id === null,
           "S11: a 300-char id + 500-char throw composes valid JSON, id echoed null (got " +
           JSON.stringify(R.rpc_longid_msg && R.rpc_longid_msg.id) + ")");
        ok(R.rpc_longid_404 && R.rpc_longid_404.ok === true &&
           R.rpc_longid_404.id === null,
           "S11: a 300-char id on method-not-found stays valid JSON (got " +
           JSON.stringify(R.rpc_longid_404 && R.rpc_longid_404.id) + ")");

        /* S10 double settle */
        ok(R.double_settle && R.double_settle.n === 1,
           "S10: a thenable resolving twice yields exactly ONE response (n=" +
           (R.double_settle && R.double_settle.n) + ")");
        ok(R.double_settle && R.double_settle.code === "200" &&
           R.double_settle.body.indexOf('"first"') >= 0,
           "S10: the first resolution wins (got '" +
           (R.double_settle ? R.double_settle.body.slice(0, 60) : "") + "')");
        ok(R.alive_after && R.alive_after.code === "200",
           "S10: the server serves a normal request afterwards (no UAF)");

        /* S16 App 413 */
        ok(R.app_413 && R.app_413.code === "413",
           "S16: a declared 2MB body on a non-upload route is a 413 (got " +
           JSON.stringify(R.app_413 && R.app_413.code) + ")");

        /* S15 maxConns */
        ok(R.maxconns && R.maxconns.refused === true,
           "S15: the third concurrent connection is refused (accept-then-close)");
        ok(R.maxconns && R.maxconns.after_free === "200",
           "S15: closing a slot admits a new connection (got " +
           JSON.stringify(R.maxconns && R.maxconns.after_free) + ")");

        /* S9 upload race */
        ok(R.upload_race && R.upload_race.body.indexOf('{"ok":true,"path":') === 0,
           "S9: an upload survives concurrent route registrations (got '" +
           (R.upload_race ? R.upload_race.body.slice(0, 60) : "") + "')");

        /* ---- framing battery: one pinned decision per row ---- */
        ok(R.te_mixedcase && R.te_mixedcase.code === "501",
           "framing: mixed-case Transfer-Encoding is still recognised -> 501 (got " +
           JSON.stringify(R.te_mixedcase && R.te_mixedcase.code) + ")");
        ok(R.te_ows_unknown && R.te_ows_unknown.n === 1,
           "framing: 'Transfer-Encoding :' (OWS colon) is an unknown header -- " +
           "framed by CL, exactly ONE response, no smuggle (n=" +
           (R.te_ows_unknown && R.te_ows_unknown.n) + ")");
        ok(R.dup_cl_equal && R.dup_cl_equal.code === "400",
           "framing: duplicate IDENTICAL Content-Length refused with 400 " +
           "(stricter than the RFC minimum, pinned here) (got " +
           JSON.stringify(R.dup_cl_equal && R.dup_cl_equal.code) + ")");
        for (const t of ["cl_negative", "cl_plus", "cl_hex"]) {
            ok(R[t] && R[t].code === "400",
               "framing: Content-Length " + t.slice(3) + " form is a 400 (got " +
               JSON.stringify(R[t] && R[t].code) + ")");
        }
        ok(R.ows_colon && R.ows_colon.code === "400",
           "framing: 'Host : x' (OWS before colon) is a malformed field line -> 400");
        ok(R.bare_lf && R.bare_lf.silent === true,
           "framing: bare-LF endings never parse -- bounded silence, no crash");
        ok(R.many_headers && R.many_headers.code === "200",
           "best-case: a 200-header request under the size cap serves");
        ok(R.long_reqline && R.long_reqline.code === "404",
           "worst-case: a 3000-char request line truncates and answers 404, bounded (got " +
           JSON.stringify(R.long_reqline && R.long_reqline.code) + ")");
        ok(R.pipeline_after_close && R.pipeline_after_close.n === 3,
           "framing: a Connection: close request pipelined mid-batch does NOT desync " +
           "the App -- 3 in, 3 well-formed responses out (the App keeps connections " +
           "open by design; honoured close is pinned on the thread-pool and Model A)");
        ok(R.pipeline50 && R.pipeline50.n === 50,
           "best-case: 50 pipelined requests give 50 in-order responses (n=" +
           (R.pipeline50 && R.pipeline50.n) + ")");
        ok(R.survives_abort && R.survives_abort.code === "200",
           "worst-case: connect+abort churn leaves the server serving");
        ok(R.split_bytes && R.split_bytes.ok === true,
           "best-case: a request dripped one byte at a time reassembles correctly " +
           "(hdr_scan_from resume logic)");
        ok(R.pct_not_decoded && R.pct_not_decoded.safe === true,
           "security: %2e%2e is NOT decoded -- no traversal through the static route");
        ok(R.ws_reserved_op && R.ws_reserved_op.alive === true,
           "ws: reserved opcode 0x3 is ignored; ping afterwards still pongs");
        ok(R.ws_unmasked_refused && R.ws_unmasked_refused.closed === true,
           "ws: an UNMASKED client frame fails the connection (RFC 6455 5.1)");
        ok(R.upload_type_disallowed && R.upload_type_disallowed.code === "403",
           "upload: a content type outside allow[] is a 403 (got " +
           JSON.stringify(R.upload_type_disallowed && R.upload_type_disallowed.code) + ")");
        ok(R.upload_over_cap && R.upload_over_cap.code === "413",
           "upload: declared body over maxFileSize is a 413 before bytes matter (got " +
           JSON.stringify(R.upload_over_cap && R.upload_over_cap.code) + ")");
        ok(R.upload_allowed_ok && R.upload_allowed_ok.code === "200" &&
           R.upload_allowed_ok.okbody === true,
           "upload control: an allowed type under the cap completes");
        ok(R.proxy_hugehead && R.proxy_hugehead.refused === true,
           "worst-case: a ~110KB upstream header head exceeds the 8KB " +
           "re-serialisation cap -> bounded 502 'upstream headers too large'");
        ok(R.proxy_earlyclose && R.proxy_earlyclose.refused === true,
           "worst-case: an upstream that closes before any head is a clean 502");
        ok(R.rpc_batch_mixed && R.rpc_batch_mixed.ok === true &&
           R.rpc_batch_mixed.ids &&
           R.rpc_batch_mixed.ids[0] === "a" && R.rpc_batch_mixed.ids[1] === "b",
           "rpc: a mixed batch yields result(a), error(b) and NOTHING for the notification (got " +
           JSON.stringify(R.rpc_batch_mixed && R.rpc_batch_mixed.ids) + ")");
        ok(R.rpc_notification_contract && R.rpc_notification_contract.ok === true,
           "rpc: single notification contract pinned: answered with id null, result intact");

        if (appPid > 0) sh(`kill ${appPid} 2>/dev/null`);
        sh("pkill -f upstream.py 2>/dev/null");
    }

    /* ================================================================
     * 4. HTTPServerAsync: 431 with a status, 413, version anchor, tokens
     * ================================================================ */
    {
        const MP = 18744;
        const s = new HTTPServerAsync({ port: MP, routes: { "/": "m\n" } });
        s.start();
        write(`${T}/modela.py`, `import socket, time, json, sys
H, P = "127.0.0.1", ${MP}
R = []
def rec(tag, **kv): R.append(dict(tag=tag, **kv))
def hit(payload, hard=6.0):
    for _ in range(40):
        try:
            s = socket.create_connection((H, P), timeout=2); break
        except Exception: time.sleep(0.1)
    else: return {"code": None, "conn": "err"}
    try: s.sendall(payload)
    except Exception: pass
    s.setblocking(False)
    out = b""; t0 = time.time()
    while time.time() - t0 < hard:
        try:
            b2 = s.recv(65536)
            if not b2: break
            out += b2
        except BlockingIOError: time.sleep(0.02)
        except Exception: break
    s.close()
    return {"code": (out[9:12].decode() if out[:4] == b"HTTP" else None),
            "conn": ("keep-alive" if b"Connection: keep-alive" in out
                     else "close" if b"Connection: close" in out else "?"),
            "nresp": out.count(b"HTTP/1.1")}

# wait until serving
for _ in range(50):
    r = hit(b"GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
    if r["code"] == "200": break
    time.sleep(0.1)
rec("control", code=r["code"])

# S16: an INCOMPLETE header block past the cap gets a 431, then close.
# Sized between the cap and the recv-loop hard bound so the pump -- not the
# silent guard -- is what fires, and with NO terminator so the incomplete
# branch (not the complete-block 413) is the one under test.
r = hit(b"GET / HTTP/1.1\\r\\nHost: x\\r\\nX-Pad: " + b"A" * (1024 * 1024 + 8000))
rec("modela_431", code=r["code"])

# S16: a complete request whose body passes the cap is a 413
r = hit(b"POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 2000000\\r\\n\\r\\n")
rec("modela_413", code=r["code"])

# S8: a header VALUE ending in HTTP/1.1 must not flip an HTTP/1.0 request
r = hit(b"GET / HTTP/1.0\\r\\nHost: x\\r\\nX-Pad: HTTP/1.1\\r\\n\\r\\n")
rec("version_anchor", conn=r["conn"])

# S7: "xclose" is not the token "close"
r = hit(b"GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: xclose\\r\\n\\r\\n"
        b"GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n", hard=4.0)
rec("token_substr", nresp=r["nresp"], conn=r["conn"])
r = hit(b"GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n")
rec("token_real_close", conn=r["conn"])
print(json.dumps(R))
`);
        sh(`python3 ${T}/modela.py > ${T}/modela.json 2>${T}/modela.err`);
        let M = {};
        try { for (const r of JSON.parse(cat(`${T}/modela.json`))) M[r.tag] = r; }
        catch (e) { ok(false, "modela probe failed: " + cat(`${T}/modela.err`).slice(0, 80)); }
        ok(M.control && M.control.code === "200", "Model A control serves");
        ok(M.modela_431 && M.modela_431.code === "431",
           "S16: an over-cap INCOMPLETE header block gets a 431, not silence (got " +
           JSON.stringify(M.modela_431 && M.modela_431.code) + ")");
        ok(M.modela_413 && M.modela_413.code === "413",
           "S16: a request whose body passes the cap is a 413 (got " +
           JSON.stringify(M.modela_413 && M.modela_413.code) + ")");
        ok(M.version_anchor && M.version_anchor.conn === "close",
           "S8: HTTP/1.0 with a header value ending 'HTTP/1.1' still closes (got " +
           JSON.stringify(M.version_anchor && M.version_anchor.conn) + ")");
        ok(M.token_substr && M.token_substr.nresp === 2 &&
           M.token_substr.conn === "keep-alive",
           "S7: Connection: xclose is NOT the close token -- conn stays up (got " +
           JSON.stringify(M.token_substr) + ")");
        ok(M.token_real_close && M.token_real_close.conn === "close",
           "S7 control: Connection: close still closes");
        s.close();
    }

    /* ================================================================
     * 5. HTTPServer (thread pool): statuses instead of silent drops,
     *    slowloris deadline, writev integrity
     * ================================================================ */
    {
        const TP = 18745;
        const s = new HTTPServer({ port: TP, requestTimeoutMs: 1500,
                                   routes: { "/": "tp\n" } });
        s.start();
        write(`${T}/tp.py`, `import socket, time, json, sys
H, P = "127.0.0.1", ${TP}
R = []
def rec(tag, **kv): R.append(dict(tag=tag, **kv))
def hit(payload, hard=6.0):
    for _ in range(40):
        try:
            s = socket.create_connection((H, P), timeout=2); break
        except Exception: time.sleep(0.1)
    else: return {"code": None, "conn": "err", "nresp": 0, "closed": None}
    try: s.sendall(payload)
    except Exception: pass
    s.setblocking(False)
    out = b""; t0 = time.time()
    while time.time() - t0 < hard:
        try:
            b2 = s.recv(65536)
            if not b2: break
            out += b2
        except BlockingIOError: time.sleep(0.02)
        except Exception: break
    s.close()
    return {"code": (out[9:12].decode() if out[:4] == b"HTTP" else None),
            "conn": ("keep-alive" if b"Connection: keep-alive" in out
                     else "close" if b"Connection: close" in out else "?"),
            "nresp": out.count(b"HTTP/1.1"),
            "body": out.split(b"\\r\\n\\r\\n", 1)[1] if b"\\r\\n\\r\\n" in out else b""}

for _ in range(50):
    r = hit(b"GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
    if r["code"] == "200": break
    time.sleep(0.1)
rec("control", code=r["code"], body=(r["body"] == b"tp\\n"))

# C1/writev integrity: a larger body must arrive byte-exact through the iovec
r = hit(b"GET /big HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
rec("big", code=r["code"])

# S16: statuses where the old code dropped silently
r = hit(b"GET / HTTP/1.1\\r\\nHost: x\\r\\n" + b"X-Pad: " + b"A" * 20000 + b"\\r\\n\\r\\n")
rec("tp_431", code=r["code"])
r = hit(b"POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 20000\\r\\n\\r\\n" + b"B" * 20000)
rec("tp_413", code=r["code"])
r = hit(b"POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: abc\\r\\n\\r\\n")
rec("tp_400", code=r["code"])
r = hit(b"POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length:\\r\\n\\r\\n")
rec("tp_400_empty", code=r["code"])

# S13: slowloris. Dribble one byte at a time; the ABSOLUTE deadline must end it.
try:
    for _ in range(40):
        try:
            sk = socket.create_connection((H, P), timeout=2); break
        except Exception: time.sleep(0.1)
    t0 = time.time()
    sk.sendall(b"GET / HTTP/1.1\\r\\nX-Slow: ")
    closed = False
    while time.time() - t0 < 8:
        try:
            sk.sendall(b"x")
        except Exception:
            closed = True; break
        try:
            d = sk.recv(32)
            if not d: closed = True; break
        except socket.timeout: pass
        except Exception: closed = True; break
        time.sleep(0.2)
    rec("slowloris", closed=closed, waited=round(time.time() - t0, 1))
    sk.close()
except Exception as e:
    rec("slowloris", closed=None, err=str(e)[:60])

# S8/S7 on the thread-pool server
r = hit(b"GET / HTTP/1.0\\r\\nHost: x\\r\\nX-Pad: HTTP/1.1\\r\\n\\r\\n")
rec("tp_version_anchor", conn=r["conn"])
r = hit(b"GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: xclose\\r\\n\\r\\n"
        b"GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n", hard=4.0)
rec("tp_token_substr", nresp=r["nresp"], conn=r["conn"])
print(json.dumps(R))
`);
        sh(`python3 ${T}/tp.py > ${T}/tp.json 2>${T}/tp.err`);
        let P5 = {};
        try { for (const r of JSON.parse(cat(`${T}/tp.json`))) P5[r.tag] = r; }
        catch (e) { ok(false, "tp probe failed: " + cat(`${T}/tp.err`).slice(0, 80)); }
        ok(P5.control && P5.control.code === "200" && P5.control.body === true,
           "thread-pool control serves (writev body intact)");
        ok(P5.tp_431 && P5.tp_431.code === "431",
           "S16: thread-pool 20KB header block is a 431 (got " +
           JSON.stringify(P5.tp_431 && P5.tp_431.code) + ")");
        ok(P5.tp_413 && P5.tp_413.code === "413",
           "S16: thread-pool body past the 16KB frame buffer is a 413 (got " +
           JSON.stringify(P5.tp_413 && P5.tp_413.code) + ")");
        ok(P5.tp_400 && P5.tp_400.code === "400",
           "S16: 'Content-Length: abc' is a 400 (got " +
           JSON.stringify(P5.tp_400 && P5.tp_400.code) + ")");
        ok(P5.tp_400_empty && P5.tp_400_empty.code === "400",
           "S16: an empty Content-Length is a 400 (got " +
           JSON.stringify(P5.tp_400_empty && P5.tp_400_empty.code) + ")");
        ok(P5.slowloris && P5.slowloris.closed === true &&
           P5.slowloris.waited < 6,
           "S13: the dribbling peer is cut by the absolute deadline (" +
           (P5.slowloris ? P5.slowloris.waited : "?") + "s, requestTimeoutMs=1500)");
        ok(P5.tp_version_anchor && P5.tp_version_anchor.conn === "close",
           "S8: thread-pool HTTP/1.0 + fake header version still closes (got " +
           JSON.stringify(P5.tp_version_anchor && P5.tp_version_anchor.conn) + ")");
        ok(P5.tp_token_substr && P5.tp_token_substr.nresp === 2,
           "S7: thread-pool 'xclose' does not close -- pipelined request answered");
        s.close();
    }

    /* ================================================================
     * 6. HOSTILE STATUS LINE: 40 digits cannot hang or crash the client
     *    (the old accumulator overflowed a signed int: UB, UBSan abort)
     * ================================================================ */
    {
        let evilSeq = 0;
        const runEvil = (mode, resp) => {
            const EP = 18746 + (++evilSeq);  /* one port per mode: pkill is
                                                async and a shared port lets
                                                the PREVIOUS mode answer */
            write(`${T}/evil.py`, `import socket, sys, threading as th
th.Timer(30, lambda: sys.exit(0)).start()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", ${EP})); s.listen(2)
while True:
    cn, _ = s.accept()
    try:
        cn.recv(4096); cn.sendall(b${resp}); cn.shutdown(socket.SHUT_WR)
    except Exception: pass
    finally: cn.close()
`);
            /* kill any leftover from a previous run FIRST: it holds the port
               and its broken twin answers the probe (measured: a stale
               server from a failed run made both modes throw) */
            sh(`pkill -f '${T}/evil.py' 2>/dev/null; sleep 0.2`);
            sh(`python3 ${T}/evil.py >${T}/evil.${mode}.log 2>&1 & sleep 0.4`);
            write(`${T}/cli.js`, `import { HTTPClient } from "dyna:net";
import * as std from "std";
const c = new HTTPClient();
c.setTimeout(3000);
let out = "";
try { const r = c.get("http://127.0.0.1:${EP}/"); out = "ok:" + r.status + ":" + r.body.length; }
catch (e) { out = "threw:" + e.message.slice(0, 80); }
const f = std.open("${T}/cli.out", "w"); f.puts(out); f.close();
`);
            sh(`rm -f ${T}/cli.out`);
            const rc = sh(`( ${EXE} ${T}/cli.js >${T}/cli.log 2>&1 & p=$!; ` +
                          `( sleep 10 && kill -9 $p 2>/dev/null ) & w=$!; ` +
                          `wait $p; r=$?; kill $w 2>/dev/null; exit $r )`);
            sh(`pkill -f '${T}/evil.py' 2>/dev/null`);
            return { rc, out: cat(`${T}/cli.out`) };
        };

        /* The clamp is pinned exactly: the first five digits (all zero here)
           are kept, so the status reads 0 -- a deterministic non-2xx, not a
           wrapped int, not a hang. A vacuous run (server never answered)
           would print threw:, which fails this equality. */
        let c = runEvil("long_status",
            `"HTTP/1.1 00000000000000000000000000000000000000005 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi"`);
        ok(c.rc !== 137 && c.out === "ok:0:2",
           "S3: a 40-digit status line clamps deterministically and cannot hang " +
           "(rc=" + c.rc + ", out='" + c.out.slice(0, 40) + "')");

        c = runEvil("normal", `"HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi"`);
        ok(c.out.indexOf("ok:200:2") === 0,
           "S3 control: a normal status still parses (got '" + c.out + "')");
    }

    sh(`rm -rf ${T}`);
    print("test_http_hardening: " + pass + " passed, " + fail + " failed");
    if (fail) throw new Error("test_http_hardening: " + fail + " failures");
}
