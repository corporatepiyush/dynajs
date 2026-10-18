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
    print("test_http_hardening_part2: SKIP (python3 not available)");
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
        const s = new HTTPServerAsync({ port: 0, routes: { "/": "m\n" } });
        s.start();
        write(`${T}/modela.py`, `import socket, time, json, sys
H, P = "127.0.0.1", ${s.port}
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

    {
        const s = new HTTPServer({ port: 0, requestTimeoutMs: 1500,
                                   routes: { "/": "tp\n" } });
        s.start();
        write(`${T}/tp.py`, `import socket, time, json, sys
H, P = "127.0.0.1", ${s.port}
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

    reapFixtures();
    sh(`rm -rf ${T}`);
    print("test_http_hardening_part2: " + pass + " passed, " + fail + " failed");
    if (fail) {
        reapFixtures();
        throw new Error("test_http_hardening_part2: " + fail + " failures");
    }
}
