// flags: --std
// timeout: 300
import { HTTPClient } from "dyna:net";
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_status_range: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_status.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/srv.js`, `
import { App, HTTPServerAsync } from "dyna:net";
import * as std from "std";

const app = new App({ port: 0 });
/* The DYNAMIC door: a handler-supplied envelope status, at response time. */
app.get("/env", (req) => ({ status: +(req.query && req.query.s) || 200, body: "hi" }));
app.start();
{ const f = std.open(scriptArgs[1], "w"); f.puts(String(app.port)); f.close(); }

/* The STATIC door: a status fixed at REGISTRATION, on the server that owns
   dyn_route_copy. It refuses there, so a refused value can never reach the
   wire -- which is why this door is pinned by its THROW, not by a request. */
const routes = {};
for (const s of [200, 404, 500, 599]) routes["/st" + s] = { status: s, body: "hi" };
const srv = new HTTPServerAsync({ port: 0, routes });
srv.start();
{ const f = std.open(scriptArgs[2], "w"); f.puts(String(srv.port)); f.close(); }
setInterval(() => {}, 1000);
`);

    write(`${T}/evil.py`, `
import socket, sys, threading
NL = bytes([13]) + bytes([10])
CASES = {
    "/s100": 100, "/s101": 101, "/s199": 199, "/s200": 200, "/s226": 226,
    "/s301": 301, "/s308": 308, "/s400": 400, "/s404": 404, "/s418": 418,
    "/s451": 451, "/s499": 499, "/s500": 500, "/s502": 502, "/s503": 503,
    "/s598": 598, "/s599": 599,
    "/s600": 600, "/s601": 601, "/s649": 649, "/s700": 700, "/s999": 999,
    "/s1000": 1000, "/s1200": 1200, "/s1999": 1999, "/s2000": 2000, "/s99999": 99999,
    "/s100_then_200": 100, "/s103_then_200": 103,
}
INTERIM = ("/s100_then_200", "/s103_then_200")
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(64)
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
def one(c):
    try:
        c.settimeout(4.0)
        d = b""
        while NL + NL not in d:
            x = c.recv(65536)
            if not x:
                break
            d += x
        p = d.split(b" ")[1].decode("latin-1") if b" " in d else "/"
        st = CASES.get(p, 200)
        if p in INTERIM:
            out = (b"HTTP/1.1 " + str(st).encode() + b" Continue" + NL +
                   b"Content-Length: 0" + NL + NL +
                   b"HTTP/1.1 200 OK" + NL + b"Content-Length: 2" + NL + NL + b"hi")
        else:
            out = (b"HTTP/1.1 " + str(st).encode() + b" Weird" + NL +
                   b"Content-Length: 2" + NL + NL + b"hi")
        c.sendall(out)
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass
# The accept loop is the MAIN thread on purpose: a daemon thread plus a main
# thread that returns kills the process, and with it the listening socket.
while True:
    try: c, _ = s.accept()
    except OSError: break
    threading.Thread(target=one, args=(c,), daemon=True).start()
`);

    write(`${T}/probe.py`, `
import os, socket, sys, time
CR = bytes([13]); LF = bytes([10]); CRLF = CR + LF
T = sys.argv[1]
def port(name):
    try: return int(open(os.path.join(T, name)).read().strip())
    except Exception: return 0
APP, SRV = port("app.port"), port("srv.port")
if APP == 0 or SRV == 0:
    print("NOFIXTURE")
    raise SystemExit(0)
def ask(p, path):
    s = socket.create_connection(("127.0.0.1", p), 5.0)
    s.settimeout(2.0)
    acc = [b""]
    try:
        s.sendall(b"GET " + path.encode() + b" HTTP/1.1" + CRLF +
                  b"Host: v" + CRLF + b"Connection: close" + CRLF + CRLF)
        while True:
            x = s.recv(65536)
            if not x: break
            acc[0] += x
    except socket.timeout: acc[0] += b"<<TIMEOUT>>"
    except OSError as e: acc[0] += ("<<%s>>" % e).encode()
    s.close()
    parts = acc[0].split(CRLF)
    f = parts[0].split(b" ") if parts else [b""]
    return f[1].decode("latin-1", "replace") if len(f) > 1 else "?"
for s in (200, 404, 500, 599, 600, 601, 700, 999):
    print("dyn\t%d\t%s" % (s, ask(APP, "/env?s=" + str(s))), flush=True)
for s in (200, 404, 500, 599):
    print("static\t%d\t%s" % (s, ask(SRV, "/st" + str(s))), flush=True)
`);


    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid; ${cmd} >${T}/${tag}.log 2>&1 & echo $! > ${T}/${tag}.pid`);
    };
    const waitPort = (tag) => {
        for (let i = 0; i < 400; i++) {
            const p = parseInt((cat(`${T}/${tag}.port`) || "").trim(), 10) || 0;
            if (p > 0) return p;
            sh("sleep 0.05");
        }
        return 0;
    };
    const reap = () => sh(`for f in ${T}/*.pid; do [ -f "$f" ] || continue; ` +
        `p=$(cat "$f" 2>/dev/null); case "$p" in ''|*[!0-9]*) continue;; esac; ` +
        `case "$(ps -p $p -o command= 2>/dev/null)" in *"${T}/"*) ` +
        `kill $p 2>/dev/null;; esac; done; rm -rf ${T}`);

    startBg(`python3 ${T}/evil.py ${T}/evil.port`, "evil");
    const ORIGIN = waitPort("evil");
    ok(ORIGIN > 0, "fixture: the origin bound a port");
    if (ORIGIN === 0) {
        print("  fixture log: " + cat(`${T}/evil.log`).slice(0, 400));
        reap();
    } else {
        const get = (path) => {
            try {
                const r = new HTTPClient().get(`http://127.0.0.1:${ORIGIN}${path}`);
                return { ok: true, status: r.status };
            } catch (e) {
                return { ok: false, err: String(e && e.message || e) };
            }
        };
        for (const s of [200, 226, 301, 308, 400, 404, 418, 451, 499, 500, 502,
                         503, 598, 599]) {
            const r = get("/s" + s);
            ok(r.ok && r.status === s,
               "client: status " + s + " is accepted and reported as itself " +
               "[got " + (r.ok ? r.status : "threw: " + r.err) + "]");
        }
        ok(get("/s599").ok && get("/s599").status === 599,
           "client: 599 is the LAST accepted status");
        for (const s of [600, 601, 649, 700, 999]) {
            const r = get("/s" + s);
            ok(!r.ok,
               "client: status " + s + " is REFUSED -- 600-999 is IANA " +
               "private-use space, not a status this engine can mean " +
               "[got " + (r.ok ? "status " + r.status : "threw: " + r.err) + "]");
        }
        for (const s of [1000, 1200, 1999, 2000, 99999]) {
            const r = get("/s" + s);
            ok(!r.ok,
               "client: a " + String(s).length + "-digit status is still refused " +
               "by M-03's digit rule, not by the range [got " +
               (r.ok ? "status " + r.status : "threw: " + r.err) + "]");
        }
        for (const p of ["/s100_then_200", "/s103_then_200"]) {
            const r = get(p);
            ok(r.ok && r.status === 200,
               "client: " + p + " still skips the interim and reports the FINAL " +
               "response [got " + (r.ok ? r.status : "threw: " + r.err) + "]");
        }
        const good = get("/s200");
        ok(good.ok && good.status === 200 && get("/s200").ok,
           "client control: an ordinary 200 still works end to end");

        startBg(`${EXE} --std ${T}/srv.js ${T}/app.port ${T}/srv.port`, "app");
        const APP = waitPort("app");
        const SRV = waitPort("srv");
        ok(APP > 0, "fixture: the App bound a port");
        ok(SRV > 0, "fixture: the static HTTPServer bound a port");
        if (APP === 0 || SRV === 0) {
            print("  app log: " + cat(`${T}/app.log`).slice(0, 600));
        } else {
            sh(`python3 ${T}/probe.py ${T} > ${T}/probe.out 2>&1`);
            const rows = { dyn: {}, static: {} };
            let nofix = false;
            for (const line of cat(`${T}/probe.out`).split("\n")) {
                const f = line.split("\t");
                if (f[0] === "NOFIXTURE") nofix = true;
                if (f.length < 3 || !rows[f[0]]) continue;
                rows[f[0]][f[1]] = f[2];
            }
            ok(!nofix, "fixture: the prober reached both server doors");
            for (const s of [200, 404, 500, 599]) {
                ok(rows.dyn[s] === String(s),
                   "App envelope door: status " + s + " is served as itself " +
                   "[got " + JSON.stringify(rows.dyn[s]) + "]");
            }
            for (const s of [600, 601, 700, 999]) {
                ok(rows.dyn[s] !== String(s),
                   "App envelope door: status " + s + " never reaches the wire as " +
                   "itself [got " + JSON.stringify(rows.dyn[s]) + "]");
            }
            for (const s of [200, 404, 500, 599]) {
                ok(rows.static[s] === String(s),
                   "static route door: a status of " + s + " registered and is " +
                   "served as itself [got " + JSON.stringify(rows.static[s]) + "]");
            }
        }
        reap();
    }
}

print("test_http_status_range: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_status_range: " + fail + " failures");
