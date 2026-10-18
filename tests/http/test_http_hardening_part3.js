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
    print("test_http_hardening_part3: SKIP (python3 not available)");
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
        const s = new HTTPServerAsync({ port: 0, routes: { "/": "ok\n" } });
        s.start();
        const c = new HTTPClient();
        try {
            const url = "http://127.0.0.1:" + s.port + "/";
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

    {
        write(`${T}/capture.py`, `import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(4)
open("${T}/cap.port", "w").write(str(s.getsockname()[1]))
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
        # read the body too when Content-Length is declared
        sep = d.find(b"\\r\\n\\r\\n")
        if sep != -1:
            head = d[:sep].decode("latin1")
            for hl in head.split("\\r\\n"):
                if hl.lower().startswith("content-length:"):
                    need = int(hl.split(":", 1)[1].strip() or 0)
                    while len(d) < sep + 4 + need:
                        b3 = cn.recv(65536)
                        if not b3: break
                        d += b3
        raw = d.decode("latin1").replace("\\r\\n", "\\n")
        cn.sendall(("HTTP/1.1 200 OK\\r\\nContent-Length: %d\\r\\n\\r\\n%s"
                    % (len(raw), raw)).encode())
    except Exception: pass
    finally: cn.close()
`);
        startBg(`python3 ${T}/capture.py`, "cap");
        const CAP = waitPort("cap");
        const c = new HTTPClient();
        try {
            const r = c.request("PATCH", `http://127.0.0.1:${CAP}/x`);
            ok(r.status === 200 && r.body.indexOf("PATCH /x") === 0,
               "a valid custom method (PATCH) reaches the wire verbatim (got '" +
               r.body.slice(0, 30) + "')");
        } catch (e) {
            ok(false, "PATCH request threw: " + e.message.slice(0, 60));
        }
        const r2 = c.get(`http://127.0.0.1:${CAP}/w`, { "X-One": "1", "X-Two": "b" });
        ok(r2.body.indexOf("X-One: 1") >= 0 && r2.body.indexOf("X-Two: b") >= 0,
           "client: object headers arrive on the wire verbatim (got '" +
           r2.body.replace(/\n/g, "|").slice(0, 90) + "')");

        const bytes = new Uint8Array(257);
        for (let i = 0; i < 257; i++) bytes[i] = i & 0xff;
        const r3 = c.post(`http://127.0.0.1:${CAP}/bin`, bytes);
        ok(r3.body.indexOf("Content-Length: 257") >= 0,
           "client: a 257-byte binary body declares CL 257 (got '" +
           r3.body.slice(-60).replace(/\n/g, "|") + "')");

        const r4 = c.get(`http://127.0.0.1:${CAP}/q?a=1&b=two`);
        ok(r4.body.indexOf("GET /q?a=1&b=two ") === 0,
           "client: query strings are preserved verbatim");
        const r5 = c.get(`http://127.0.0.1:${CAP}/p#frag`);
        ok(r5.body.indexOf("GET /p ") === 0,
           "client: fragments are stripped from the request-target, never " +
           "sent on the wire (dynajs.d.ts: a request-url fragment is stripped " +
           "before anything else)");

        let arrThrew = null;
        try { c.get(`http://127.0.0.1:${CAP}/x`, ["X-Evil: 1"]); }
        catch (e) { arrThrew = e; }
        ok(arrThrew && /not an array/.test(arrThrew.message),
           "client: an Array as headers is refused (numeric-named junk), got: " +
           (arrThrew ? arrThrew.message.slice(0, 50) : "no throw"));

        c.close();
        sh(`kill $(cat ${T}/cap.pid) 2>/dev/null`);
    }

    {
        const throws = (f, re, m) => {
            let e = null;
            try { f(); } catch (x) { e = x; }
            ok(e && re.test(e.message), m + " (got: " +
               (e ? e.message.slice(0, 70) : "no throw") + ")");
        };
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x": { status: -5, body: "y" } } }),
               /status must be in \[100, 599\]/, "Async: status -5 refused");
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x": { status: 1000, body: "y" } } }),
               /status must be in \[100, 599\]/, "Async: status 1000 refused");
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x": { status: 600, body: "y" } } }),
               /status must be in \[100, 599\]/, "Async: status 600 refused (the boundary)");
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x": { status: 200, body: "y", contentType: "text/plain\r\nX-Evil: 1" } } }),
               /control characters/, "Async: contentType CRLF refused (response splitting)");
        throws(() => new HTTPServerAsync({ port: 0, routes: { "/x\r\nEvil: 1": "y" } }),
               /control characters/, "Async: route path CRLF refused");
        throws(() => new HTTPServer({ port: 0, routes: { "/x": { status: 99, body: "y" } } }),
               /status must be in \[100, 599\]/, "ThreadPool: status 99 refused");
        throws(() => new HTTPServer({ port: 0, routes: { "/x": { status: 200, body: "y", contentType: "a\r\nb" } } }),
               /control characters/, "ThreadPool: contentType CRLF refused");
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

    {
        const runEvil = (mode, resp, delay) => {
            write(`${T}/evil.py`, `import socket, sys, time, threading as th
th.Timer(30, lambda: sys.exit(0)).start()
DELAY = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(2)
open("${T}/evil.${mode}.port", "w").write(str(s.getsockname()[1]))
while True:
    cn, _ = s.accept()
    try:
        cn.recv(4096)
        if DELAY: time.sleep(DELAY)
        cn.sendall(b"${resp}"); cn.shutdown(socket.SHUT_WR)
    except Exception: pass
    finally: cn.close()
`);
            sh(`pkill -f '${T}/evil.py' 2>/dev/null; sleep 0.2`);
            startBg(`python3 ${T}/evil.py ${delay}`, `evil.${mode}`);
            const EP = waitPort(`evil.${mode}`);
            write(`${T}/cli.js`, `import { HTTPClient } from "dyna:net";
import * as std from "std";
const c = new HTTPClient();
c.setTimeout(3000);
let out = "";
try { const r = c.get("http://127.0.0.1:${EP}/");
     out = "ok:" + r.status + ":" + r.body.length +
           "|H=" + String(r.headers["X-Dup"] ?? r.headers["x-dup"] ?? "");
}
catch (e) { out = "threw:" + e.message.slice(0, 80); }
const f = std.open("${T}/cli.out", "w"); f.puts(out); f.close();
`);
            sh(`rm -f ${T}/cli.out`);
            const rc = sh(`( ${EXE} --std ${T}/cli.js >${T}/cli.log 2>&1 & p=$!; ` +
                          `( exec >/dev/null 2>&1; sleep 10 && kill -9 $p ) & w=$!; ` +
                          `wait $p; r=$?; kill $w 2>/dev/null; exit $r )`);
            sh(`pkill -f '${T}/evil.py' 2>/dev/null`);
            return { rc, out: cat(`${T}/cli.out`) };
        };

        let c = runEvil("long_status",
            "HTTP/1.1 00000000000000000000000000000000000000005 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.rc !== 137 && c.out.indexOf("threw:") === 0,
           "S3: a 40-digit status line is refused, not clamped into a status " +
           "(rc=" + c.rc + ", out='" + c.out.slice(0, 40) + "')");

        c = runEvil("status_2000", "HTTP/1.1 2000 Weird\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.rc !== 137 && c.out.indexOf("threw:") === 0,
           "M03: a 4-digit status is refused, not surfaced as status=2000 " +
           "(rc=" + c.rc + ", out='" + c.out.slice(0, 40) + "')");

        c = runEvil("status_99999", "HTTP/1.1 99999 Weird\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.rc !== 137 && c.out.indexOf("threw:") === 0,
           "M03: a 5-digit status is refused, not surfaced as status=99999 " +
           "(rc=" + c.rc + ", out='" + c.out.slice(0, 40) + "')");

        c = runEvil("status_1999_then_200",
            "HTTP/1.1 1999 Weird\\r\\n\\r\\nHTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.rc !== 137 && c.out.indexOf("threw:") === 0,
           "M03: 1999 is refused instead of discarded as an interim " +
           "(rc=" + c.rc + ", out='" + c.out.slice(0, 40) + "')");
        c = runEvil("status_199_then_200",
            "HTTP/1.1 199 Odd\\r\\n\\r\\nHTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.rc !== 137 && c.out.indexOf("ok:200:2") === 0,
           "M03 control: a legal 3-digit interim is still skipped " +
           "(rc=" + c.rc + ", out='" + c.out.slice(0, 40) + "')");

        c = runEvil("cl_and_te",
            "HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\nContent-Length: 5\\r\\n\\r\\n0\\r\\n\\r\\n", 0);
        ok(c.rc !== 137 && c.out.indexOf("threw:") === 0,
           "③: TE+CL on one response is rejected, not silently dechunked (got '" +
           c.out.slice(0, 50) + "')");

        c = runEvil("normal",
            "HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.out.indexOf("ok:200:2") === 0,
           "S3 control: a normal status still parses (got '" + c.out + "')");

        c = runEvil("dup_cl_same",
            "HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.out.indexOf("ok:200:2") === 0,
           "client: IDENTICAL duplicate Content-Length is accepted (asymmetric " +
           "with the server's strict refusal, pinned here) (got '" + c.out + "')");

        c = runEvil("chunked_ext",
            "HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n5;ext=1\\r\\nHELLO\\r\\n0\\r\\nX-Trailer: t\\r\\n\\r\\n", 0);
        ok(c.out.indexOf("ok:200:5") === 0,
           "client: chunk extensions and a trailer section decode to the payload (got '" +
           c.out + "')");

        c = runEvil("no_reason",
            "HTTP/1.1 200\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.out.indexOf("ok:200:2") === 0,
           "client: a status line with no reason phrase parses (got '" + c.out + "')");

        c = runEvil("multi_cookie",
            "HTTP/1.1 200 OK\\r\\nX-Dup: first\\r\\nX-Dup: second\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.out.indexOf("|H=first, second") > 0,
           "client: repeated response headers are combined in order, RFC 9110 5.3 (got '" + c.out + "')");

        c = runEvil("nul_body",
            "HTTP/1.1 200 OK\\r\\nContent-Length: 3\\r\\n\\r\\na\\x00b", 0);
        ok(c.out.indexOf("ok:200:3") === 0,
           "client: NUL inside the body keeps the declared length (got '" + c.out + "')");

        c = runEvil("slow_headers",
            "HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 4);
        ok(c.rc !== 137 && c.out.indexOf("threw:") === 0,
           "client: setTimeout bounds a slow-response server (got '" +
           c.out.slice(0, 40) + "')");

        c = runEvil("interim_100",
            "HTTP/1.1 100 Continue\\r\\n\\r\\nHTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.out.indexOf("ok:200:2") === 0,
           "client: an interim 100 is skipped, the final response is answered (got '" +
           c.out.slice(0, 60) + "')");

        c = runEvil("nul_value",
            "HTTP/1.1 200 OK\\r\\nX-Nul: a\\x00b\\r\\nContent-Length: 2\\r\\n\\r\\nhi", 0);
        ok(c.out.indexOf("threw:") === 0,
           "client: a NUL in a header value is a malformed response (got '" +
           c.out.slice(0, 60) + "')");

    }

    {
        write(`${T}/capture2.py`, `import socket, sys, threading as th
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(16)
open("${T}/cap2.port", "w").write(str(s.getsockname()[1]))
th.Timer(60, lambda: sys.exit(0)).start()
def serve(cn):
    try:
        d = b""
        while b"\\r\\n\\r\\n" not in d:
            b2 = cn.recv(4096)
            if not b2: return
            d += b2
        sep = d.find(b"\\r\\n\\r\\n")
        head = d[:sep].decode("latin1")
        need = 0
        for hl in head.split("\\r\\n"):
            if hl.lower().startswith("content-length:"):
                try: need = int(hl.split(":",1)[1].strip() or 0)
                except ValueError: need = 0
        while len(d) < sep + 4 + need:
            b3 = cn.recv(65536)
            if not b3: break
            d += b3
        raw = d.decode("latin1")
        cn.sendall(("HTTP/1.1 200 OK\\r\\nContent-Length: %d\\r\\n\\r\\n%s" % (len(raw), raw)).encode())
    except Exception: pass
    finally:
        try: cn.close()
        except Exception: pass
while True:
    c, _ = s.accept()
    th.Thread(target=serve, args=(c,), daemon=True).start()
`);
        startBg(`python3 ${T}/capture2.py`, "cap2");
        const CAP2 = waitPort("cap2");
        const cc = new HTTPClient();
        const curl2 = `http://127.0.0.1:${CAP2}/`;
        const rD = cc.post(curl2, "abc", { "Content-Length": "3", "X-Keep": "1" });
        const dupCL = (rD.body.match(/Content-Length:/gi) || []).length;
        ok(rD.status === 200 && dupCL === 1 && rD.body.indexOf("X-Keep: 1") >= 0,
           "wire: caller Content-Length is dropped; ours is the only framing (got " +
           dupCL + " CL lines)");
        const rH = cc.get(curl2, { "Host": "evil.example" });
        ok(rH.status === 200 &&
           (rH.body.match(/Host:/gi) || []).length === 1 &&
           rH.body.indexOf("evil.example") < 0,
           "wire: caller Host is dropped; ours is the only one");
        const rT = cc.post(curl2, "abc", { "Transfer-Encoding": "chunked" });
        ok(rT.status === 200 && rT.body.indexOf("Transfer-Encoding:") < 0 &&
           (rT.body.match(/Content-Length:/gi) || []).length === 1,
           "wire: caller Transfer-Encoding is dropped; framing stays by length");
        const rN = cc.get(curl2, "X-Foo: bar");
        ok(rN.status === 200 && rN.body.indexOf("X-Foo: bar\r\nConnection: close") >= 0,
           "wire: a string header block without a trailing CRLF is terminated, " +
           "not glued onto Connection (got '" +
           rN.body.replace(/\r\n/g, "|").slice(-40) + "')");
        cc.close();
        sh(`kill $(cat ${T}/cap2.pid) 2>/dev/null`);
    }

    {
        write(`${T}/up2.py`, `import socket, threading as th, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(8)
open("${T}/up2.port", "w").write(str(s.getsockname()[1]))
th.Timer(60, lambda: sys.exit(0)).start()
def serve(cn):
    try:
        d = b""
        cn.settimeout(2)
        while True:
            b2 = cn.recv(4096)
            if not b2: break
            d += b2
            if b"\\r\\n\\r\\n" in d: break
        open("${T}/up2.last.bin", "ab").write(b"====\\n" + d)
        body = b"CAPTURED"
        cn.sendall(b"HTTP/1.1 200 OK\\r\\nContent-Length: 8\\r\\nConnection: close\\r\\n\\r\\n" + body)
    except Exception: pass
    finally:
        try: cn.close()
        except Exception: pass
while True:
    c, _ = s.accept()
    th.Thread(target=serve, args=(c,), daemon=True).start()
`);
        write(`${T}/app2.js`, `import { App } from "dyna:net";
import { Path } from "dyna:file";
import * as std from "std";
const UP2 = parseInt(scriptArgs[1], 10);   /* the upstream's ephemeral port */
const app = new App({ port: 0, idleTimeoutMs: 8000 });
app.rpc("/rpc", { ping: () => "pong" });
app.proxy("/p", { host: "127.0.0.1", port: UP2 });
app.ws("/ws", { open: () => {}, message: () => {}, close: () => {} });
app.start();
const g = std.open("${T}/app2.port", "w"); g.puts(String(app.port)); g.close();
`);
        startBg(`python3 ${T}/up2.py`, "up2");
        const UP2 = waitPort("up2");
        startBg(`${EXE} --std ${T}/app2.js ${UP2}`, "app2sh");
        const AP2 = waitPort("app2");
        sh(`rm -f ${T}/up2.last.bin`);
        write(`${T}/atk2.py`, `import socket, sys, json
H, P = "127.0.0.1", ${AP2}
R = []
def rec(tag, **kv): R.append(dict(tag=tag, **kv))
def hit(req, tag, timeout=4.0):
    try:
        s = socket.create_connection((H, P), timeout=timeout)
        s.sendall(req)
        s.settimeout(timeout)
        buf = b""
        try:
            while True:
                b2 = s.recv(65536)
                if not b2: break
                buf += b2
        except Exception: pass
        s.close()
        rec(tag, code=(buf[9:12].decode("latin1") if buf[:4] == b"HTTP" else None))
    except Exception as e:
        rec(tag, code=None, err=str(e)[:50])
BODY = b'{"method":"ping","id":1}'
hit(b"GET /rp\\nX-Evil: 1 HTTP/1.1\\r\\nHost: h\\r\\n\\r\\n", "lf_target")
hit(("GET /rp\\x01x HTTP/1.1\\r\\nHost: h\\r\\n\\r\\n").encode("latin1"), "ctl_target")
hit(b"POST /rpc HTTP/1.1\\r\\nContent-Length: %d\\r\\n\\r\\n" % len(BODY) + BODY, "no_host")
hit(b"POST /rpc HTTP/1.1\\r\\nHost: a\\r\\nHost: b\\r\\nContent-Length: %d\\r\\n\\r\\n" % len(BODY) + BODY, "dup_host")
hit(b"POST /rpc HTTP/1.1\\r\\nHost: h\\r\\nX-Nul: a\\x00b\\r\\nContent-Length: %d\\r\\n\\r\\n" % len(BODY) + BODY, "ctl_value")
hit(b"POST /rpc HTTP/1.1\\r\\nHost: h\\r\\nContent-Length: %d\\r\\n\\r\\n" % len(BODY) + BODY, "control_ok")
hit(b"GET /p/lf\\nX: 1 HTTP/1.1\\r\\nHost: h\\r\\nContent-Length: 0\\r\\n\\r\\n", "proxy_lf")
hit(b"GET /p/clean HTTP/1.1\\r\\nHost: h\\r\\n\\r\\n", "proxy_clean")
print(json.dumps(R))
`);
        sh(`python3 ${T}/atk2.py > ${T}/atk2.json 2>${T}/atk2.err`);
        let A = {};
        try { JSON.parse(cat(`${T}/atk2.json`)).forEach(r => A[r.tag] = r); }
        catch (e) { ok(false, "atk2 failed: " + cat(`${T}/atk2.err`).slice(0, 80)); }
        ok(A.lf_target && A.lf_target.code === "400",
           "W: a bare LF in the request-target is a 400 (got " + JSON.stringify(A.lf_target && A.lf_target.code) + ")");
        ok(A.ctl_target && A.ctl_target.code === "400",
           "W: a CTL byte in the request-target is a 400 (got " + JSON.stringify(A.ctl_target && A.ctl_target.code) + ")");
        ok(A.no_host && A.no_host.code === "400",
           "W: an HTTP/1.1 request without Host is a 400 (RFC 9110 7.2; got " + JSON.stringify(A.no_host && A.no_host.code) + ")");
        ok(A.dup_host && A.dup_host.code === "400",
           "W: two Host lines are a 400 (RFC 9110 7.2; got " + JSON.stringify(A.dup_host && A.dup_host.code) + ")");
        ok(A.ctl_value && A.ctl_value.code === "400",
           "W: a NUL in a request header value is a 400 (got " + JSON.stringify(A.ctl_value && A.ctl_value.code) + ")");
        ok(A.control_ok && A.control_ok.code === "200",
           "W: a well-formed rpc request still dispatches (got " + JSON.stringify(A.control_ok && A.control_ok.code) + ")");
        ok(A.proxy_lf && A.proxy_lf.code === "400",
           "W: the proxy refuses an LF-carrying target instead of forwarding it (got " + JSON.stringify(A.proxy_lf && A.proxy_lf.code) + ")");
        ok(A.proxy_clean && A.proxy_clean.code === "200",
           "W: a clean proxy request still forwards (got " + JSON.stringify(A.proxy_clean && A.proxy_clean.code) + ")");
        {
            const up = cat(`${T}/up2.last.bin`);
            ok(up.indexOf("====") >= 0 && /====\nGET [^\n]*HTTP\/1\.1/.test(up),
               "W: the proxy re-serialises a one-line request line upstream (got '" +
               up.replace(/\r/g, "<CR>").replace(/\n/g, "<LF>").slice(0, 90) + "')");
        }
        {
            const WSKEY = "dGhlIHNhbXBsZSBub25jZQ==";
            write(`${T}/wsprobe.py`, `import socket
s = socket.create_connection(("127.0.0.1", ${AP2}), timeout=3)
s.sendall(b"GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\nSec-WebSocket-Key: ${WSKEY}\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n")
s.settimeout(3)
hs = b""
while b"\\r\\n\\r\\n" not in hs:
    b2 = s.recv(4096)
    if not b2: break
    hs += b2
payload = b"\\xff\\xfe\\xfebad"
mask = b"\\x01\\x02\\x03\\x04"
masked = bytes(payload[i] ^ mask[i & 3] for i in range(len(payload)))
out = b""
try:
    s.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + masked)
    while True:
        b2 = s.recv(4096)
        if not b2: break
        out += b2
except Exception: pass
open("${T}/ws.out", "w").write(out.hex())
`);
            sh(`python3 ${T}/wsprobe.py >${T}/ws.log 2>&1`);
            const wsRaw = cat(`${T}/ws.out`);
            ok(wsRaw.indexOf("880203ef") >= 0,
               "W: an invalid-UTF-8 text frame draws a close frame with code 1007 (got '" +
               wsRaw.slice(0, 40) + "')");
        }
        sh(`pkill -f '${T}/app2.js' 2>/dev/null; pkill -f '${T}/up2.py' 2>/dev/null`);
    }

    {
        const s = new HTTPServerAsync({ port: 0, host: "::",
                                        routes: { "/": "dual\n" } });
        s.start();
        sh(`rm -f ${T}/v6.out`);
        sh(`python3 - <<'PYEOF' > ${T}/v6.out 2>&1
import socket, json
R = {}
for tag, host in (("v6", "::1"), ("v4map", "127.0.0.1")):
    try:
        sk = socket.create_connection((host, ${s.port}), timeout=2)
        sk.sendall(b"GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n")
        sk.settimeout(3)
        out = b""
        while True:
            d = sk.recv(4096)
            if not d: break
            out += d
        sk.close()
        R[tag] = "ok" if b"dual" in out else "badbody:" + repr(out[:48])
    except Exception as e:
        R[tag] = "err:" + str(e)[:40]
print(json.dumps(R))
PYEOF`);
        let V = {};
        try { V = JSON.parse(cat(`${T}/v6.out`).trim()); }
        catch (e) { ok(false, "v6 probe failed: " + cat(`${T}/v6.out`).slice(0, 60)); }
        ok(V.v6 === "ok", "⑤: host '::' serves ::1 natively (got " + JSON.stringify(V.v6) + ")");
        ok(V.v4map === "ok", "⑤: the same socket serves 127.0.0.1 v4-mapped (got " +
           JSON.stringify(V.v4map) + ")");
        s.close();

        write(`${T}/app6.js`, `import { App } from "dyna:net";
import { pid } from "dyna:sys";
import * as std from "std";
const f = std.open("${T}/app6.pid", "w"); f.puts(String(pid())); f.close();
const app = new App({ port: 0, host: "::" });
app.rpc("/h", { ok: () => "dual-app" });
app.start();
const g = std.open("${T}/app6.port", "w"); g.puts(String(app.port)); g.close();
`);
        sh(`${EXE} --std ${T}/app6.js >${T}/app6.log 2>&1 &`);
        let app6Port = 0;
        for (let i = 0; i < 100 && !(app6Port > 0); i++) {
            app6Port = parseInt(cat(`${T}/app6.port`).trim(), 10) || 0;
            if (!(app6Port > 0)) sh("sleep 0.05");
        }
        ok(app6Port > 0, "App v6 child started and bound");
        write(`${T}/v6a.py`, `import socket, json
R = {}
for tag, host in (("v6", "::1"), ("v4map", "127.0.0.1")):
    try:
        sk = socket.create_connection((host, ${app6Port}), timeout=2)
        body = b'{"method":"ok","id":1}'
        sk.sendall(b"POST /h HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: %d\\r\\nConnection: close\\r\\n\\r\\n" % len(body) + body)
        sk.settimeout(3)
        out = b""
        while True:
            d = sk.recv(4096)
            if not d: break
            out += d
        sk.close()
        R[tag] = "ok" if b"dual-app" in out else "badbody:" + repr(out[:48])
    except Exception as e:
        R[tag] = "err:" + str(e)[:40]
print(json.dumps(R))
`);
        sh(`rm -f ${T}/v6a.out`);
        sh(`python3 ${T}/v6a.py > ${T}/v6a.out 2>&1`);
        let VA = {};
        try { VA = JSON.parse(cat(`${T}/v6a.out`).trim()); }
        catch (e) { ok(false, "App v6 probe failed: " + cat(`${T}/v6a.out`).slice(0, 60)); }
        ok(VA.v6 === "ok", "⑤: App host '::' serves ::1 through dyn_aio_listen (got " +
           JSON.stringify(VA.v6) + ")");
        ok(VA.v4map === "ok", "⑤: App dual-stack serves 127.0.0.1 too (got " +
           JSON.stringify(VA.v4map) + ")");
        sh(`pkill -f '${T}/app6.js' 2>/dev/null`);
    }

    reapFixtures();
    sh(`rm -rf ${T}`);
    print("test_http_hardening_part3: " + pass + " passed, " + fail + " failed");
    if (fail) {
        reapFixtures();
        throw new Error("test_http_hardening_part3: " + fail + " failures");
    }
}
