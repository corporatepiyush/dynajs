// flags: --std
// The App server must survive a battery of malformed HTTP and WebSocket input:
// after EVERY attack a well-formed RPC call has to come back with the right
// answer. Each attack is aimed at a different parser limit (length fields,
// header blocks, nesting, frame headers); "survived" is the property, not the
// exact status code, which other suites pin.
import * as os from "os";
import * as std from "std";

const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_httpsurv.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL: " + w); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_survival: SKIP (python3 not available)");
} else {
    sh(`rm -rf ${T}; mkdir -p ${T}/root ${T}/up`);
    write(`${T}/root/ok.txt`, "hi\n");
    write(`${T}/srv.js`, `import { App } from "dyna:net";
import { Path } from "dyna:file";
import { pid } from "dyna:sys";
import * as std from "std";
const app = new App({ port: 0, idleTimeoutMs: 3000 });
app.rpc("/rpc", { echo: (p) => p, add: (a, b) => a + b });
app.static("/s", new Path("${T}/root"));
app.upload("/u", { dir: new Path("${T}/up"), maxFileSize: 4096 }, () => ({ ok: 1 }));
app.ws("/ws", { open: () => {}, message: (c, d, bin) => c.send(bin ? d : "e:" + d), close: () => {} });
app.start();
const f = std.open("${T}/srv.pid", "w"); f.puts(pid() + " " + app.port); f.close();
`);

    write(`${T}/attack.py`, `import socket, json, sys, os, base64, struct, time
H, P = "127.0.0.1", int(sys.argv[1])

def raw(payload, wait=0.25):
    buf = b""
    try:
        s = socket.create_connection((H, P), 2)
        s.settimeout(wait)
        s.sendall(payload)
        end = time.time() + wait
        while time.time() < end:
            try:
                d = s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
        s.close()
    except OSError:
        pass
    return buf

def alive():
    body = b'{"jsonrpc":"2.0","method":"add","params":[2,3],"id":1}'
    req = b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\nContent-Length: %d\\r\\n\\r\\n" % len(body) + body
    for _ in range(10):
        if b'"result":5' in raw(req, 1.0):
            return True
        time.sleep(0.1)
    return False

def ws_frame(op, actual, declared, mask=True):
    h = bytes([0x80 | op])
    m = 0x80 if mask else 0
    if declared < 126:
        h += bytes([m | declared])
    elif declared <= 0xFFFF:
        h += bytes([m | 126]) + struct.pack(">H", declared)
    else:
        h += bytes([m | 127]) + struct.pack(">Q", declared)
    key = b"\\x01\\x02\\x03\\x04"
    pay = bytes([0x41]) * actual
    if mask:
        h += key
        pay = bytes(b ^ key[i & 3] for i, b in enumerate(pay))
    return h + pay

def ws_attack(frame, wait=0.3):
    try:
        s = socket.create_connection((H, P), 2)
        s.settimeout(1.0)
        k = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET /ws HTTP/1.1\\r\\nHost: x\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\n"
                   "Sec-WebSocket-Key: %s\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n" % k).encode())
        buf = b""
        while b"\\r\\n\\r\\n" not in buf:
            d = s.recv(4096)
            if not d:
                return "no-handshake"
            buf += d
        if b" 101 " not in buf.split(b"\\r\\n", 1)[0]:
            return "no-handshake"
        try:
            s.sendall(frame)
        except OSError:
            pass
        time.sleep(wait)
        s.close()
        return "sent"
    except OSError as e:
        return "error: %s" % e

CRLF = b"\\r\\n"
R = []
def attack(name, fn, needs_ws=False):
    got = fn()
    R.append({"name": name, "alive": alive(), "ws": got if needs_ws else None})

if not alive():
    print(json.dumps([{"name": "server not up", "alive": False, "ws": None}]))
    sys.exit(0)

attack("giant Content-Length, tiny body", lambda: raw(b"POST /rpc HTTP/1.1\\r\\nContent-Length: 999999999999999999\\r\\n\\r\\n{}"))
attack("negative Content-Length", lambda: raw(b"POST /rpc HTTP/1.1\\r\\nContent-Length: -1\\r\\n\\r\\n{}"))
attack("30-digit Content-Length", lambda: raw(b"POST /rpc HTTP/1.1\\r\\nContent-Length: " + b"9" * 30 + b"\\r\\n\\r\\n{}"))
attack("header block never terminated, then close", lambda: raw(b"GET /rpc HTTP/1.1\\r\\nX: " + b"a" * 500))
attack("2 MB header block", lambda: raw(b"GET /rpc HTTP/1.1\\r\\n" + (b"X: " + b"a" * 100 + CRLF) * 20000 + CRLF, 1.0))
attack("8 KB request path", lambda: raw(b"GET /" + b"a" * 8192 + b" HTTP/1.1\\r\\n\\r\\n"))
attack("NUL bytes in the path", lambda: raw(b"GET /rpc\\x00\\x00 HTTP/1.1\\r\\n\\r\\n"))
attack("unknown method", lambda: raw(b"FOOBAR /rpc HTTP/1.1\\r\\n\\r\\n"))
attack("500 pipelined requests in one write", lambda: raw(b"GET /rpc HTTP/1.1\\r\\n\\r\\n" * 500, 1.0))
attack("slowloris: one byte then idle", lambda: raw(b"G", 0.15))
attack("truncated JSON-RPC body", lambda: raw(b'POST /rpc HTTP/1.1\\r\\nContent-Length: 10\\r\\n\\r\\n{"jsonrpc'))
attack("JSON nested 10000 deep", lambda: raw(b"POST /rpc HTTP/1.1\\r\\nContent-Length: 20000\\r\\n\\r\\n" + b"[" * 10000 + b"]" * 10000, 1.0))
attack("__proto__ as the method", lambda: raw(b'POST /rpc HTTP/1.1\\r\\nContent-Length: 45\\r\\n\\r\\n{"jsonrpc":"2.0","method":"__proto__","id":1}'))
attack("static traversal to /etc/passwd", lambda: raw(b"GET /s/../../../../etc/passwd HTTP/1.1\\r\\n\\r\\n"))
attack("upload with a spoofed type", lambda: raw(b"POST /u HTTP/1.1\\r\\nContent-Type: image/png\\r\\nContent-Length: 3\\r\\n\\r\\nabc"))
attack("WS upgrade without a key", lambda: raw(b"GET /ws HTTP/1.1\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\n\\r\\n"))
attack("WS unmasked client frame", lambda: ws_attack(ws_frame(1, 5, 5, mask=False)), True)
attack("WS 64-bit declared length", lambda: ws_attack(ws_frame(2, 8, 0xFFFFFFFFFFFFFF00)), True)
attack("WS 2 GiB declared length, 8 bytes sent", lambda: ws_attack(ws_frame(2, 8, 0x7FFFFFFF)), True)
attack("WS ping over 125 bytes", lambda: ws_attack(ws_frame(9, 200, 200)), True)
attack("WS reserved opcode 0x5", lambda: ws_attack(ws_frame(5, 4, 4)), True)
attack("WS 2 MB message", lambda: ws_attack(ws_frame(1, 2 << 20, 2 << 20), 0.8), True)
print(json.dumps(R))
`);

    const exe = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
    sh(`${exe} --std ${T}/srv.js >${T}/srv.log 2>&1 &`);
    for (let i = 0; i < 200 && !cat(`${T}/srv.pid`).trim(); i++) os.sleep(25);
    const [srvPid, port] = cat(`${T}/srv.pid`).trim().split(" ").map((x) => parseInt(x, 10));
    ok(srvPid > 0 && port > 0, "server started and reported its port (log: " + cat(`${T}/srv.log`).slice(0, 160) + ")");

    if (port > 0) {
        sh(`python3 ${T}/attack.py ${port} > ${T}/att.json 2>${T}/att.err`);
        let R = [];
        try { R = JSON.parse(cat(`${T}/att.json`)); }
        catch (e) { ok(false, "attacker produced JSON (stderr: " + cat(`${T}/att.err`).slice(0, 200) + ")"); }
        ok(R.length === 22, "all 22 attacks ran (got " + R.length + ")");
        for (const r of R) {
            ok(r.alive, "server answers a valid RPC after: " + r.name);
            if (r.ws !== null)
                ok(r.ws === "sent", "the WebSocket handshake completed, so the frame really reached the parser: " + r.name + " (" + r.ws + ")");
        }
        ok(sh(`kill -0 ${srvPid} 2>/dev/null`) === 0, "server process is still running at the end");
    }
    if (srvPid > 0) sh(`kill ${srvPid} 2>/dev/null`);
    if (fail === 0) sh(`rm -rf ${T}`);
    print("test_http_survival: " + pass + " passed, " + fail + " failed");
    if (fail) throw new Error("test_http_survival: " + fail + " failures");
}
