// flags: --std
// Part of the former tests/http/test_http_hardening.js, split so the parts run in parallel.
import "../lib/httpc.js";
import { App, HTTPClient, HTTPServer, HTTPServerAsync } from "dyna:net";
import { pid } from "dyna:sys";
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_hardening_part1: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_hard.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid; ${cmd} >${T}/${tag}.log 2>&1 & echo $! > ${T}/${tag}.pid`);
    };
    const waitPort = (tag) => {
        for (let i = 0; i < 200; i++) {
            const p = parseInt((cat(`${T}/${tag}.port`) || "").trim(), 10) || 0;
            if (p > 0) return p;
            sh("sleep 0.05");
        }
        return 0;
    };
    const reapFixtures = () => {
        sh(`for f in ${T}/*.pid; do [ -f "$f" ] || continue; ` +
           `p=$(cat "$f" 2>/dev/null); case "$p" in ''|*[!0-9]*) continue;; esac; ` +
           `case "$(ps -p $p -o command= 2>/dev/null)" in *"${T}/"*) ` +
           `kill $p 2>/dev/null; sleep 0.2; kill -9 $p 2>/dev/null;; esac; done`);
    };
    write(`${T}/reap.py`, `import glob, os, signal, subprocess, sys, time
T, parent = sys.argv[1], int(sys.argv[2])
def alive(p):
    try:
        os.kill(p, 0); return True
    except OSError:
        return False
while alive(parent):
    time.sleep(0.2)
for _ in range(40):
    left = 0
    for f in glob.glob(os.path.join(T, "*.pid")):
        try: p = int(open(f).read().strip())
        except Exception: continue
        if p <= 0 or p == os.getpid(): continue
        try: cmd = subprocess.check_output(["ps", "-p", str(p), "-o", "command="], text=True)
        except Exception: continue
        if T + "/" in cmd:
            try: os.kill(p, signal.SIGKILL)
            except Exception: pass
            left += 1
    if not left: break
    time.sleep(0.2)
subprocess.run(["rm", "-rf", T], check=False)
`);
    sh(`python3 ${T}/reap.py ${T} ${pid()} >/dev/null 2>&1 &`);

    

    {
        sh(`mkdir -p ${T}/www ${T}/updir ${T}/mtr`);
        sh(`printf USER-METRICS > ${T}/mtr/index.html`);
        sh(`printf 'PREFIX-OK' > ${T}/www/oo.html`);
        write(`${T}/upstream.py`, `import socket, sys, threading as th
th.Timer(120, lambda: sys.exit(0)).start()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(8)
open("${T}/up.port", "w").write(str(s.getsockname()[1]))
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
const UP = parseInt(scriptArgs[1], 10);   /* the upstream's ephemeral port */
const app = new App({ port: 0, idleTimeoutMs: 8000, maxConns: 2,
                   metrics: true });
app.static("/f", new Path("${T}/www"));
app.proxy("/api", { host: "127.0.0.1", port: UP });
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
/* A user route at the built-ins' path wins: registration beats convention */
app.static("/metrics", new Path("${T}/mtr"));
/* ④ outbound backpressure: each open floods 6 MB at a peer that never
 * reads -- the 4 MB outbound cap must end the connection, not grow the
 * aio queue without bound. */
const MB = new Uint8Array(1048576);
/* 24 MB: large enough that kernel socket buffers cannot absorb the flood on
 * any platform (Linux loopback autotunes into the MB range), so the 4 MB
 * outbound userspace cap is what ends these connections. */
app.ws("/wsflood", { open: (ws) => {
  for (let i = 0; i < 24; i++) ws.send(MB);
}, message: () => {}, close: () => {} });
app.sse("/sseflood", { open: (sse) => {
  for (let i = 0; i < 24; i++) sse.send("F".repeat(1048576));
}, close: () => {} });
/* route registrations while an upload is in flight: the old code held a
 * pointer into the realloc'd route array across event-loop turns (UAF) */
let n = 0;
const timer = setInterval(() => {
  app.upload("/up_gen" + (n++), { dir: new Path("${T}/updir") }, () => 0);
}, 30);
setTimeout(() => clearInterval(timer), 60000);
app.start();
const g = std.open("${T}/app.port", "w"); g.puts(String(app.port)); g.close();
`);
        write(`${T}/probe.py`, `import socket, time, json, sys, base64, hashlib
H, P = "127.0.0.1", int(sys.argv[1])
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
    # Both held connections must be ADMITTED before the third is probed: a
    # connection the previous probe just closed can still hold a slot, and then
    # one of these two is the refused one and the "third" walks in.
    for _ in range(30):
        held = []
        try:
            for _ in range(2):
                sk = socket.create_connection((H, P), timeout=2)
                held.append(sk)
                sk.sendall(b"GET /healthz HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
                sk.settimeout(2.0)
                if not sk.recv(64).startswith(b"HTTP/1.1"):
                    raise RuntimeError("not admitted")
            socks = held
            break
        except Exception:
            for sk in held:
                try: sk.close()
                except Exception: pass
            time.sleep(0.1)
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

# --- S15b: a slot freed by a peer close is free for the very next accept ---
# io_uring may surface an accept before the EOF that preceded it; the cap must
# still count the closed connection as gone (measured 29% refusals before).
time.sleep(0.4)
cap_refused = 0; cap_rounds = 0
def admitted(sk):
    sk.sendall(b"GET /healthz HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
    sk.settimeout(2.0)
    try: return sk.recv(64).startswith(b"HTTP/1.1")
    except Exception: return False
for _ in range(60):
    trio = []
    try:
        p0 = socket.create_connection((H, P), timeout=2); trio.append(p0)
        if not admitted(p0): continue
        p0.close()
        a0 = socket.create_connection((H, P), timeout=2); trio.append(a0)
        b0 = socket.create_connection((H, P), timeout=2); trio.append(b0)
        cap_rounds += 1
        if not (admitted(a0) and admitted(b0)): cap_refused += 1
    except Exception:
        pass
    finally:
        for sk in trio:
            try: sk.close()
            except Exception: pass
        time.sleep(0.03)
rec("cap_after_close", rounds=cap_rounds, refused=cap_refused)

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

# Connection: close swallows everything pipelined behind it (recorded again,
# with the close semantics, in the App-close section below)
one = ("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
       % len(body)).encode() + body.encode()
closer = (b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n"
          b"Content-Length: %d\\r\\n\\r\\n" % len(body)) + body.encode()
hit(one + closer + one)

# 50 pipelined requests -> 50 responses in order
p50 = one * 50
r = hit(p50, want=50, hard=8.0)
rec("pipeline50", n=r["n"])

# connect + immediate close must not disturb the server. The follow-up
# request RETRIES across fresh connections: under parallel-suite load one
# attempt can lose the race, but a server genuinely wedged by the abort
# churn fails every attempt.
try:
    sk = socket.create_connection((H, P), timeout=2); sk.close()
    sk = socket.create_connection((H, P), timeout=2); sk.close()
except Exception: pass
code = None
for _ in range(4):
    r = post(json.dumps({"method": "add", "params": [4, 4], "id": 11}).encode())
    if r["code"] == "200":
        code = "200"; break
    time.sleep(0.25)
rec("survives_abort", code=code)

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

# notification alone: JSON-RPC 2.0 -- a request with no id NEVER gets an
# answer. The old code replied id:null; the spec fix made it silent.
r = post(json.dumps({"method": "add", "params": [2, 2]}).encode())
rec("rpc_notification_silent", n=r["n"], code=r["code"])

# --- ① App honors Connection: close / HTTP/1.0 default-close ---
def hit_eof(payload, hard=4.0):
    for _ in range(30):
        try:
            s = socket.create_connection((H, P), timeout=2); break
        except Exception: time.sleep(0.1)
    else: return {"code": None, "eof": None}
    try: s.sendall(payload)
    except Exception: pass
    s.settimeout(hard)
    out = b""; eof = False
    try:
        while True:
            d = s.recv(65536)
            if not d: eof = True; break
            out += d
            if b"}" in out or b"\\r\\n\\r\\n" in out:
                # got a response; wait briefly for the EOF that proves close
                try:
                    while True:
                        d = s.recv(4096)
                        if not d: eof = True; break
                        out += d
                except socket.timeout:
                    pass
                break
    except Exception: pass
    s.close()
    return {"code": (out[9:12].decode() if out[:4] == b"HTTP" else None),
            "eof": eof, "full": (b"PREFIX-OK" in out)}
r = hit_eof(b"GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n")
rec("app_close_token", code=r["code"], eof=r["eof"],
                   full=r["full"])
r = hit_eof(b"GET /f/oo.html HTTP/1.0\\r\\nHost: x\\r\\n\\r\\n")
rec("app_http10_default_close", code=r["code"], eof=r["eof"],
                   full=r["full"])
# control: keep-alive request stays open (no EOF within the window)
try:
    s = socket.create_connection((H, P), timeout=2)
    s.sendall(b"GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
    s.settimeout(1.5)
    out = b""
    kept = True
    try:
        while b"\\r\\n\\r\\n" not in out:
            d = s.recv(4096)
            if not d: kept = False; break
            out += d
        if kept:
            d = s.recv(16)   # any byte/EOF within 1.5s means it closed early
            kept = False if d == b"" else True
    except socket.timeout:
        pass
    s.close()
    rec("app_keepalive_control", kept=kept)
except Exception as e:
    rec("app_keepalive_control", kept=None, err=str(e)[:40])

# pipeline_after_close revisited: closer now ends the batch at itself
r = hit(one + closer + one, want=3, hard=4.0)
rec("pipeline_after_close", n=r["n"])
# P1 REGRESSION: a request pipelined BEHIND an upload must be dispatched
# when the upload finishes synchronously -- it used to stall until an
# unrelated byte arrived.
zero_up = (b"POST /up HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: text/plain\\r\\n"
           b"Content-Length: 0\\r\\n\\r\\n")
rpcbody = json.dumps({"method": "add", "params": [5, 5], "id": 31}).encode()
rpc = (b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\n\\r\\n"
       % len(rpcbody)) + rpcbody
r = hit(zero_up + rpc, want=2, hard=4.0)
raws = str(r.get("raw", ""))
rec("upload_pipelined", n=r["n"], up='"ok":true' in raws,
    rpc='"result":10' in raws, tail=raws[-120:])

# HEAD combined with Range: headers advertise the PARTIAL length, no body
import subprocess as _sp2
def curl_raw2(args):
    rr = _sp2.run(["curl", "-s", "--max-time", "4"] + args, capture_output=True)
    return rr.stdout.decode("latin1")
hd = curl_raw2(["-si", "-I", "-H", "Range: bytes=2-5",
               "http://127.0.0.1:%d/f/oo.html" % P])
rec("static_head_range", ok=("206" in hd.split("\\n")[0] and
                            "Content-Length: 4" in hd and
                            "PREFIX-OK" not in hd))

# metrics override: a user route at /metrics beats the built-in
mv = curl("/metrics")
rec("metrics_override", ok=("USER-METRICS" in mv and "dyn_http_" not in mv))

# --- ⑥ static HEAD + Range ---
import subprocess as _sp
def curl_raw(args):
    rr = _sp.run(["curl", "-s", "--max-time", "4"] + args,
                 capture_output=True)
    return rr.stdout.decode("latin1")
hd = curl_raw(["-I", "http://127.0.0.1:%d/f/oo.html" % P])
rec("static_head", ok=("200" in hd.split("\\r\\n")[0] and
                       "Content-Length: 9" in hd and
                       not hd.endswith("PREFIX-OK")))
rg = curl_raw(["-si", "-H", "Range: bytes=0-3",
               "http://127.0.0.1:%d/f/oo.html" % P])
rec("static_range_first4", body=rg)
rg = curl_raw(["-si", "-H", "Range: bytes=-4",
               "http://127.0.0.1:%d/f/oo.html" % P])
rec("static_range_suffix", body=rg)
rg = curl_raw(["-si", "-H", "Range: bytes=100-",
               "http://127.0.0.1:%d/f/oo.html" % P])
rec("static_range_unsat", code=rg[:40])
rg = curl_raw(["-si", "-H", "Range: bytes=0-2,5-8",
               "http://127.0.0.1:%d/f/oo.html" % P])
rec("static_range_multi_full", body=rg)

# --- ④ outbound backpressure: 6 MB pushed at a peer that reads NOTHING ---
def flood_probe(path, is_ws):
    try:
        s = socket.create_connection((H, P), timeout=2)
        if is_ws:
            k = base64.b64encode(b"0123456789abcdef").decode()
            s.sendall(("GET %s HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\n"
                       "Connection: Upgrade\\r\\nSec-WebSocket-Key: %s\\r\\n"
                       "Sec-WebSocket-Version: 13\\r\\n\\r\\n"
                       % (path, k)).encode())
        else:
            s.sendall(("GET %s HTTP/1.1\\r\\nHost: h\\r\\nAccept: text/event-stream\\r\\n\\r\\n"
                       % path).encode())
        s.settimeout(10.0)
        t0 = time.time()
        got = b""
        eof = False
        # STOP READING. A peer that keeps draining lets Linux's autotuned
        # loopback buffers absorb megabytes, so the outbound cap may never
        # register. A stalled peer fills kernel + userspace queues and the
        # 4 MB bound must end the connection; one final drain observes the
        # FIN. This is the invariant the bound exists for.
        try:
            got += s.recv(262144)          # first bytes / headers
            time.sleep(4.0)                # stall: let the queues back up
            while time.time() - t0 < 8:
                d = s.recv(262144)
                if not d: eof = True; break
                got += d
        except socket.timeout:
            pass
        s.close()
        return {"eof": eof, "waited": round(time.time() - t0, 1),
                "bytes": len(got)}
    except Exception as e:
        return {"eof": None, "err": str(e)[:50]}
rec("ws_flood_bounded", **flood_probe("/wsflood", True))
rec("sse_flood_bounded", **flood_probe("/sseflood", False))

print(json.dumps(R))
`);
        write(`${T}/assert.js`, `
/* parsed by the parent below */
`);

        startBg(`python3 ${T}/upstream.py`, "up");
        const UP = waitPort("up");
        startBg(`${EXE} --std ${T}/app.js ${UP}`, "appsh");
        let appPid = 0;
        for (let i = 0; i < 100 && !(appPid > 0); i++) {
            appPid = parseInt(cat(`${T}/app.pid`).trim(), 10) || 0;
            if (!(appPid > 0)) sh("sleep 0.05");
        }
        ok(appPid > 0, "App child started");
        const AP = waitPort("app");

        sh(`python3 ${T}/probe.py ${AP} > ${T}/probe.json 2>${T}/probe.err`);
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
                      "maxconns", "cap_after_close", "upload_race",
                      "te_mixedcase", "te_ows_unknown", "dup_cl_equal",
                      "cl_negative", "cl_plus", "cl_hex", "ows_colon", "bare_lf",
                      "many_headers", "long_reqline", "pipeline_after_close",
                      "pipeline50", "survives_abort", "split_bytes",
                      "pct_not_decoded", "ws_reserved_op", "ws_unmasked_refused",
                      "upload_type_disallowed", "upload_over_cap",
                      "upload_allowed_ok", "proxy_hugehead", "proxy_earlyclose",
                      "rpc_batch_mixed", "rpc_notification_silent",
                      "app_close_token", "app_http10_default_close",
                      "app_keepalive_control",
                      "static_head", "static_range_first4", "static_range_suffix",
                      "static_range_unsat", "static_range_multi_full",
                      "ws_flood_bounded", "sse_flood_bounded",
                      "upload_pipelined", "static_head_range",
                      "metrics_override"];
        const missing = need.filter(t => !R[t]);
        ok(missing.length === 0, "every probe recorded (missing means the probe died, not that a defence failed)"
           + (missing.length ? " -- missing: " + missing.join(",") + " stderr: " +
              cat(`${T}/probe.err`).slice(0, 120) : ""));

        ok(R.static_ok && R.static_ok.body === "PREFIX-OK",
           "static control: /f/oo.html serves (got " + JSON.stringify(R.static_ok && R.static_ok.body) + ")");
        ok(R.static_boundary && R.static_boundary.body.indexOf("PREFIX-OK") < 0,
           "S6: /foo.html does NOT match the /f prefix (got " +
           JSON.stringify(R.static_boundary && R.static_ok && R.static_boundary.body.slice(0, 40)) + ")");

        ok(R.proxy_ok && R.proxy_ok.body.indexOf("seen=/x") >= 0,
           "proxy control: /api/x forwarded as /x (got " + JSON.stringify(R.proxy_ok && R.proxy_ok.body) + ")");
        ok(R.proxy_boundary && R.proxy_boundary.body.indexOf("seen=") < 0,
           "S6: /apiv2/x is NOT forwarded to the upstream (got " +
           JSON.stringify(R.proxy_boundary && R.proxy_boundary.body.slice(0, 40)) + ")");

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

        ok(R.double_settle && R.double_settle.n === 1,
           "S10: a thenable resolving twice yields exactly ONE response (n=" +
           (R.double_settle && R.double_settle.n) + ")");
        ok(R.double_settle && R.double_settle.code === "200" &&
           R.double_settle.body.indexOf('"first"') >= 0,
           "S10: the first resolution wins (got '" +
           (R.double_settle ? R.double_settle.body.slice(0, 60) : "") + "')");
        ok(R.alive_after && R.alive_after.code === "200",
           "S10: the server serves a normal request afterwards (no UAF)");

        ok(R.app_413 && R.app_413.code === "413",
           "S16: a declared 2MB body on a non-upload route is a 413 (got " +
           JSON.stringify(R.app_413 && R.app_413.code) + ")");

        ok(R.maxconns && R.maxconns.refused === true,
           "S15: the third concurrent connection is refused (accept-then-close)");
        ok(R.maxconns && R.maxconns.after_free === "200",
           "S15: closing a slot admits a new connection (got " +
           JSON.stringify(R.maxconns && R.maxconns.after_free) + ")");
        ok(R.cap_after_close && R.cap_after_close.rounds >= 50 && R.cap_after_close.refused === 0,
           "S15b: a slot freed by a peer close admits the next connection (" +
           JSON.stringify(R.cap_after_close) + ")");

        ok(R.upload_race && R.upload_race.body.indexOf('{"ok":true,"id":"up_') === 0,
           "S9: an upload survives concurrent route registrations (got '" +
           (R.upload_race ? R.upload_race.body.slice(0, 60) : "") + "')");

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
        ok(R.upload_type_disallowed && R.upload_type_disallowed.code === "415",
           "upload: a content type outside allow[] is a 415 (got " +
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
        ok(R.static_head && R.static_head.ok === true,
           "static HEAD: 200 with the true Content-Length and no body (got " +
           JSON.stringify(R.static_head) + ")");
        ok(R.static_range_first4 && R.static_range_first4.body.indexOf("206") >= 0 &&
           R.static_range_first4.body.endsWith("PREF"),
           "static Range bytes=0-3 -> 206 Partial Content, first 4 bytes (got '" +
           (R.static_range_first4 ? R.static_range_first4.body.slice(-8) : "") + "')");
        ok(R.static_range_suffix && R.static_range_suffix.body.endsWith("X-OK") &&
           R.static_range_suffix.body.indexOf("206") >= 0,
           "static suffix Range -4 -> last 4 bytes 'X-OK' (got '" +
           (R.static_range_suffix ? R.static_range_suffix.body.slice(-8) : "") + "')");
        ok(R.static_range_unsat && R.static_range_unsat.code.indexOf("416") >= 0,
           "static Range past EOF -> 416 with bytes */9 failure form (got '" +
           (R.static_range_unsat ? R.static_range_unsat.code.slice(0, 30) : "") + "')");
        ok(R.static_range_multi_full &&
           R.static_range_multi_full.body.indexOf("200") >= 0 &&
           R.static_range_multi_full.body.endsWith("PREFIX-OK"),
           "static multi-range falls back to a full 200 (got '" +
           (R.static_range_multi_full ? R.static_range_multi_full.body.slice(0, 30) : "") + "')");
        ok(R.ws_flood_bounded && R.ws_flood_bounded.eof === true &&
           R.ws_flood_bounded.waited < 9,
           "backpressure: 6 MB ws flood at a non-reading peer ends the connection (" +
           JSON.stringify(R.ws_flood_bounded) + ")");
        ok(R.sse_flood_bounded && R.sse_flood_bounded.eof === true &&
           R.sse_flood_bounded.waited < 9,
           "backpressure: same bound on the sse push path (" +
           JSON.stringify(R.sse_flood_bounded) + ")");

        ok(R.upload_pipelined && R.upload_pipelined.n === 2 &&
           R.upload_pipelined.up === true &&
           R.upload_pipelined.rpc === true,
           "P1: a request pipelined behind a synchronous upload is dispatched " +
           "without waiting for another byte (n=" +
           (R.upload_pipelined && R.upload_pipelined.n) + ", up=" +
           (R.upload_pipelined && R.upload_pipelined.up) + ", rpc=" +
           (R.upload_pipelined && R.upload_pipelined.rpc) + ")");
        ok(R.static_head_range && R.static_head_range.ok === true,
           "static HEAD+Range: 206 headers with the PARTIAL length and no body (got " +
           JSON.stringify(R.static_head_range) + ")");
        ok(R.metrics_override && R.metrics_override.ok === true,
           "routing: a user route at /metrics beats the built-in -- " +
           "registration beats convention (got " + JSON.stringify(R.metrics_override) + ")");
        ok(R.rpc_notification_silent && R.rpc_notification_silent.n === 0,
           "rpc: a single notification (no id) gets NO response, per JSON-RPC 2.0 " +
           "(n=" + (R.rpc_notification_silent && R.rpc_notification_silent.n) + ")");
        ok(R.app_close_token && R.app_close_token.code === "200" &&
           R.app_close_token.eof === true && R.app_close_token.full === true,
           "close: Connection: close delivers the FULL body, then EOF (got " +
           JSON.stringify(R.app_close_token) + ")");
        ok(R.app_http10_default_close.full === true,
           "close: HTTP/1.0 default-close also delivers the full body");
        ok(R.app_http10_default_close && R.app_http10_default_close.code === "200" &&
           R.app_http10_default_close.eof === true,
           "close: HTTP/1.0 with no Connection header defaults to close (got " +
           JSON.stringify(R.app_http10_default_close) + ")");
        ok(R.app_keepalive_control && R.app_keepalive_control.kept === true,
           "close control: an HTTP/1.1 keep-alive request stays open");
        ok(R.pipeline_after_close && R.pipeline_after_close.n === 2,
           "close: a Connection: close request ends the pipelined batch at itself " +
           "-- the request behind it is NOT answered (n=" +
           (R.pipeline_after_close && R.pipeline_after_close.n) + ")");

        if (appPid > 0) sh(`kill ${appPid} 2>/dev/null`);
        sh(`pkill -f '${T}/upstream.py' 2>/dev/null`);
        reapFixtures();
    }

    reapFixtures();
    sh(`rm -rf ${T}`);
    print("test_http_hardening_part1: " + pass + " passed, " + fail + " failed");
    if (fail) {
        reapFixtures();
        throw new Error("test_http_hardening_part1: " + fail + " failures");
    }
}
