// flags: --std
import { App, HTTPClient, HTTPServer } from "dyna:net";
import * as std from "std";
import * as os from "os";

let n = 0, fails = 0;
function assert(cond, msg) {
    n++;
    if (!cond) { fails++; print("  FAIL: " + msg); }
}

const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_perf.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;

const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
sh(`rm -rf ${T}; mkdir -p ${T}`);

{
    write(T + "/appfix.js", `
import { App } from "dyna:net";
import * as std from "std";
const app = new App({ port: 0 });
app.rpc("/rpc", { ping: () => "pong" });
app.start();
const f = std.open("${T}/appfix.port", "w");
f.puts(String(app.port));
f.close();
`);
    sh(`rm -f ${T}/appfix.port; ./dynajs --std ${T}/appfix.js >${T}/appfix.log 2>&1 & echo $! > ${T}/appfix.pid`);
    let appport = 0;
    for (let i = 0; i < 200 && !appport; i++) {
        appport = parseInt((cat(T + "/appfix.port") || "").trim(), 10) || 0;
        if (!appport) sh("sleep 0.05");
    }
    assert(appport > 0, "app fixture published its port (log: " +
           cat(T + "/appfix.log").slice(0, 80) + ")");

    write(T + "/a.py", `import socket, json, sys
H, P = "127.0.0.1", ${appport}
def raw(payload, n=1):
    s = socket.create_connection((H, P), timeout=3)
    s.sendall(payload)
    s.settimeout(3)
    out = b""
    try:
        while out.count(b"HTTP/1.1") < n:
            b2 = s.recv(65536)
            if not b2: break
            out += b2
    except socket.timeout: pass
    s.close()
    return out
R = {}

# 70 distinct names, then repeat name #0: the dup MUST be refused (400).
# A 64-entry probe table would have evicted name #0 and answered 200 --
# the boundary this pins is ring-size == header-cap, not ring-size == 64.
hs = b"".join(b"X-H%d: v\\r\\n" % i for i in range(70))
body = b'{"jsonrpc":"2.0","method":"ping","id":1}'
req = (b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: application/json\\r\\n"
       + hs + b"X-H0: again\\r\\nContent-Length: %d\\r\\n\\r\\n" % len(body) + body)
out = raw(req)
def code(out):
    return out[9:12].decode() if out[:4] == b"HTTP" else None
R["dup_past_64"] = (code(out) == "400")

# The count cap counts EVERY field line, so the three built-ins (Host,
# Content-Type, Content-Length) ride along: 253+3=256 serves AT the cap,
# 252+3=255 serves under it, 254+3=257 is 431 one past it.
for cnt, tag in ((252, "h255"), (253, "h256"), (254, "h257")):
    hs = b"".join(b"Y-%d: v\\r\\n" % i for i in range(cnt))
    req = (b"POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: application/json\\r\\n"
           + hs + b"Content-Length: %d\\r\\n\\r\\n" % len(body) + body)
    out = raw(req)
    R[tag] = code(raw(req))

# request-target truncation: 2000 'a's is cut at the path buffer (1023),
# routed as the PREFIX that was registered -- served, not refused.
long = b"/longpath/" + b"a" * 2000
req = b"GET " + long + b" HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"
out = raw(req)
R["long_target"] = code(out)  # "404" (no route, truncated target ok), never "400"
print(json.dumps(R))
`);
    sh(`python3 ${T}/a.py > ${T}/a.json 2>${T}/a.err`);
    let R = {};
    try { R = JSON.parse(cat(T + "/a.json")); }
    catch (e) { assert(false, "app probe failed: " + cat(T + "/a.err").slice(0, 120)); }
    assert(R.dup_past_64 === true,
           "duplicate header past 70 distinct names is refused 400 (got " +
           JSON.stringify(R.dup_past_64) + ")");
    assert(R.h255 === "200", "255 headers serves (got " + R.h255 + ")");
    assert(R.h256 === "200", "256 headers serves at the cap (got " + R.h256 + ")");
    assert(R.h257 === "431", "257 headers is 431 one past the cap (got " + R.h257 + ")");
    assert(R.long_target === "404",
           "2000-byte target truncates and routes (404), never 400 (got " + R.long_target + ")");
    sh(`p=$(cat ${T}/appfix.pid 2>/dev/null); ` +
       `case "$p" in ''|*[!0-9]*) ;; *) kill $p 2>/dev/null; sleep 0.2; kill -9 $p 2>/dev/null;; esac`);
}

{
    const s = new HTTPServer({ port: 0, routes: { "/": "ok\n" } });
    s.start();
    write(T + "/b.py", `import socket, json
H, P = "127.0.0.1", ${s.port}
def raw(payload):
    c = socket.create_connection((H, P), timeout=3)
    c.sendall(payload); c.settimeout(3)
    try: out = c.recv(65536)
    except socket.timeout: out = b""
    c.close(); return out
R = {}
# complete header block of ~20KB: header TOO LARGE -> 431
pad = b"Z-Pad: " + b"A" * 20000 + b"\\r\\n"
R["big_header"] = raw(b"GET / HTTP/1.1\\r\\nHost: x\\r\\n" + pad + b"\\r\\n")[9:12].decode()
# fitting header, declared body past the 16KiB frame -> 413
R["big_body"] = raw(b"POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 20000\\r\\n\\r\\n"
                    + b"B" * 20000)[9:12].decode()
# one byte UNDER the frame cap on the head: still served
hdr = b"GET / HTTP/1.1\\r\\nHost: x\\r\\n" + b"Z: " + b"a" * 16000 + b"\\r\\n\\r\\n"
R["head_under_cap"] = (raw(hdr)[9:12].decode() if len(hdr) < 16*1024 else "SKIP")
print(json.dumps(R))
`);
    sh(`python3 ${T}/b.py > ${T}/b.json 2>${T}/b.err`);
    let R = {};
    try { R = JSON.parse(cat(T + "/b.json")); }
    catch (e) { assert(false, "caps probe failed: " + cat(T + "/b.err").slice(0, 120)); }
    assert(R.big_header === "431",
           "complete 20KB header block is 431 (got " + R.big_header + ")");
    assert(R.big_body === "413",
           "fitting header + 20KB declared body is 413 (got " + R.big_body + ")");
    if (R.head_under_cap !== "SKIP")
        assert(R.head_under_cap === "200",
               "header block under the cap serves (got " + R.head_under_cap + ")");
    s.close();
}

{
    const s = new HTTPServer({ port: 0, routes: { "/": "c\n" } });
    s.start();
    write(T + "/c.py", `import socket, json, time
H, P = "127.0.0.1", ${s.port}
socks = []
errs = 0; oks = 0; denied = 0
try:
    for i in range(400):
        try:
            c = socket.create_connection((H, P), timeout=5)
            socks.append(c)
        except Exception as e:
            denied += 1
    # every connection that EXISTS gets one request on its own socket
    for c in socks:
        try:
            c.sendall(b"GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
        except Exception:
            errs += 1
    for c in socks:
        c.settimeout(5)
        try:
            out = c.recv(4096)
            if out[9:12] == b"200": oks += 1
            elif out[:4] == b"HTTP": errs += 1
            else: errs += 1
        except Exception:
            errs += 1
finally:
    for c in socks:
        try: c.close()
        except Exception: pass
print(json.dumps({"oks": oks, "errs": errs, "denied": denied}))
`);
    sh(`python3 ${T}/c.py > ${T}/c.json 2>${T}/c.err`);
    let R = {};
    try { R = JSON.parse(cat(T + "/c.json")); }
    catch (e) { assert(false, "flood probe failed: " + cat(T + "/c.err").slice(0, 120)); }
    assert(R.oks >= 390,
           "400 simultaneous connections: >=390 answered 200 (got " + R.oks + ")");
    assert(R.errs === 0,
           "zero non-2xx/wrong responses under 400 conns (got " + R.errs + ")");
    s.close();
}

{
    let fired = 0;
    const s = new HTTPServer({ port: 0, routes: { "/": "t\n" } });
    s.start();
    const c = new HTTPClient();
    setTimeout(() => { fired = 1; }, 60);
    const r = c.get("http://127.0.0.1:" + s.port + "/");
    assert(r.status === 200, "sync client served while timer pending");
    for (let spin = 0; !fired && spin < 100; spin++)
        await sleep(10);
    assert(fired === 1, "JS timer fired through the direct reactor wait");
    c.close();
    s.close();
}

if (fails === 0) {
    print("test_http_perf_regress: all tests passed (" + n + " assertions)");
} else {
    print("test_http_perf_regress: " + fails + " failures of " + n);
    throw new Error("test_http_perf_regress failed");
}
