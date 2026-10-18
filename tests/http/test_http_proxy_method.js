// flags: --std
// timeout: 300
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
const hexToStr = (h) => {
    let s = "";
    for (let i = 0; i + 1 < h.length; i += 2)
        s += String.fromCharCode(parseInt(h.substr(i, 2), 16));
    return s;
};

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_proxy_method: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_proxy_meth.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/srv.js`, `
import { App } from "dyna:http";
import * as std from "std";
const app = new App({ port: 0 });
app.proxy("/api", { host: "127.0.0.1", port: +scriptArgs[2] });
/* A normal route with its own method door, so the two paths can be compared
   on the SAME bytes rather than argued about. */
app.get("/echo", req => ({ body: { ok: 1 } }));
app.start();
{ const f = std.open(scriptArgs[1], "w"); f.puts(String(app.port)); f.close(); }
setInterval(() => {}, 1000);
`);

    write(`${T}/drive.py`, `
import os, socket, sys, threading, time

CR = bytes([13]); LF = bytes([10]); CRLF = CR + LF
T = sys.argv[1]
OKBODY = b"HTTP/1.1 200 OK" + CRLF + b"Content-Length: 2" + CRLF + CRLF + b"UP"

# (name, method bytes) -- every C0 byte is covered by the sweep below.
CASES = [
    ("get",     b"GET"),
    ("post",    b"POST"),
    ("head",    b"HEAD"),
    ("put",     b"PUT"),
    ("delete",  b"DELETE"),
    ("patch",   b"PATCH"),
    ("options", b"OPTIONS"),
    # an UNREGISTERED but well-formed token: a proxy must forward it. Dropping
    # methods it does not recognise is how a proxy breaks WebDAV.
    ("propfind", b"PROPFIND"),
    ("m-search", b"M-SEARCH"),
    ("m24",     b"A" * 24),
    ("m25",     b"A" * 25),
    ("m0",      b""),
    ("bogus",   b"BOGUS"),
    ("tchar",   b"!#$%&'*+-.^_" + bytes([96]) + b"|~09azAZ"),
    ("nul",     b"G" + bytes([0]) + b"T"),
    ("cr",      b"G" + CR + b"T"),
    ("lf",      b"G" + LF + b"T"),
    ("sp",      b"G T"),
    ("tab",     b"G" + bytes([9]) + b"T"),
    ("del",     b"G" + bytes([127]) + b"T"),
    ("hi",      b"G" + bytes([127]) + b"T"),
    ("soh",     b"G" + bytes([1]) + b"T"),
    ("vt",      b"G" + bytes([11]) + b"T"),
    ("ff",      b"G" + bytes([12]) + b"T"),
    ("us",      b"G" + bytes([31]) + b"T"),
]

# every C0 byte as a suffix on a valid-looking method, plus DEL
for _c in list(range(0x00, 0x20)) + [0x7f]:
    CASES.append(("c0_%02x" % _c, b"GET" + bytes([_c] + [ord("x")])))

up = socket.socket(); up.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
up.bind(("127.0.0.1", 0)); up.listen(64)
UPPORT = up.getsockname()[1]
seen = []

def one(c):
    try:
        c.settimeout(4.0)
        d = b""
        while CRLF + CRLF not in d:
            x = c.recv(65536)
            if not x:
                break
            d += x
        seen.append(d)
        c.sendall(OKBODY)
        c.shutdown(socket.SHUT_WR); time.sleep(0.02)
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass

def serve():
    while True:
        try: c, _ = up.accept()
        except OSError: return
        threading.Thread(target=one, args=(c,), daemon=True).start()

threading.Thread(target=serve, daemon=True).start()
open(os.path.join(T, "up.port"), "w").write(str(UPPORT))

PROXY = 0
for _ in range(600):
    try:
        v = int(open(os.path.join(T, "proxy.port")).read().strip())
        if v > 0:
            PROXY = v
            break
    except Exception:
        pass
    time.sleep(0.05)
if PROXY == 0:
    print("NOPROXY\\tERR\\t-", flush=True)
    raise SystemExit(0)

def ask(method, path=b"/api/thing"):
    s = socket.create_connection(("127.0.0.1", PROXY), 5.0)
    s.settimeout(1.5)
    acc = [b""]
    try:
        s.sendall(method + b" " + path + b" HTTP/1.1" + CRLF +
                  b"Host: victim" + CRLF + b"Content-Length: 0" + CRLF + CRLF)
        while CRLF + CRLF not in acc[0]:
            x = s.recv(65536)
            if not x:
                break
            acc[0] += x
        i = acc[0].find(CRLF + CRLF)
        if i >= 0:
            cl = 0
            for ln in acc[0][:i].split(CRLF)[1:]:
                if b":" in ln:
                    k, v = ln.split(b":", 1)
                    if k.strip().lower() == b"content-length":
                        try: cl = int(v.strip())
                        except ValueError: cl = 0
            while len(acc[0]) - (i + 4) < cl:
                x = s.recv(65536)
                if not x:
                    break
                acc[0] += x
    except socket.timeout:
        acc[0] += b"<<TIMEOUT>>"
    except OSError as e:
        acc[0] += ("<<%s>>" % e).encode()
    s.close()
    parts = acc[0].split(CRLF)
    st = parts[0].split(b" ")[1] if len(parts[0].split(b" ")) > 1 else b""
    return st

def ask_local(method):
    """The SAME bytes against a NORMAL route, so the two paths are compared on
    one fixture instead of argued about."""
    s = socket.create_connection(("127.0.0.1", PROXY), 5.0)
    s.settimeout(1.5)
    acc = [b""]
    try:
        s.sendall(method + b" /echo HTTP/1.1" + CRLF + b"Host: v" + CRLF +
                  b"Content-Length: 0" + CRLF + CRLF)
        while CRLF + CRLF not in acc[0]:
            x = s.recv(65536)
            if not x:
                break
            acc[0] += x
    except socket.timeout:
        acc[0] += b"<<TIMEOUT>>"
    except OSError as e:
        acc[0] += ("<<%s>>" % e).encode()
    s.close()
    parts = acc[0].split(CRLF)
    return parts[0].split(b" ")[1] if len(parts[0].split(b" ")) > 1 else b""

for name, method in CASES:
    before = len(seen)
    st = ask(method)
    # did the UPSTREAM receive a request line for this case at all?
    got = b""
    for d in seen[before:]:
        got = d
    lo = ask_local(method)
    print("%s\\t%s\\t%s\\t%s" % (name, st.decode("latin-1", "replace"),
                                    got[:40].hex(),
                                    lo.decode("latin-1", "replace")), flush=True)
print("DONE\t-", flush=True)
`);

    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid; ${cmd} >${T}/${tag}.log 2>&1 & echo $! > ${T}/${tag}.pid`);
    };
    const waitPort = (tag) => {
        for (let i = 0; i < 300; i++) {
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

    startBg(`python3 ${T}/drive.py ${T}`, "drv");
    for (let i = 0; i < 200; i++) {
        if (parseInt((cat(`${T}/up.port`) || "").trim(), 10) > 0) break;
        sh("sleep 0.05");
    }
    const UPPORT = parseInt((cat(`${T}/up.port`) || "").trim(), 10) || 0;
    ok(UPPORT > 0, "fixture: the recording origin bound a port");
    if (UPPORT === 0) {
        print("  driver log: " + cat(`${T}/drv.log`).slice(0, 400));
        reap();
    } else {
        startBg(`${EXE} --std ${T}/srv.js ${T}/app.port ${UPPORT}`, "app");
        const P = waitPort("app");
        ok(P > 0, "fixture: the App bound an ephemeral port");
        if (P === 0) {
            print("  app log: " + cat(`${T}/app.log`).slice(0, 400));
        } else {
            write(`${T}/proxy.port`, String(P));
            for (let i = 0; i < 800; i++) {
                const l = cat(`${T}/drv.log`);
                if (l.indexOf("DONE") >= 0 || l.indexOf("NOPROXY") >= 0) break;
                sh("sleep 0.05");
            }
            const rows = {};
            for (const line of cat(`${T}/drv.log`).split("\n")) {
                const f = line.split("\t");
                if (f[0] === "DONE") continue;
                if (f.length < 4 || !f[0]) continue;
                rows[f[0]] = { status: f[1], upstream: hexToStr(f[2]),
                               local: f[3] };
            }
            const r = (n) => rows[n];
            const notForwarded = (n) => {
                const x = r(n);
                return !!x && x.upstream === "";
            };

            ok(!!r("get") && r("get").local === "200",
               "get: the route that DOES list GET answers 200 on the normal path");
            for (const n of ["get", "post", "head", "put", "delete", "patch",
                             "options", "propfind", "m-search", "tchar", "m24"]) {
                const x = r(n);
                ok(!!x && x.status === "200" && x.upstream.indexOf(x.upstream) === 0 &&
                   x.upstream.length > 0,
                   n + ": a well-formed method is proxied and reaches the upstream " +
                   "[status=" + (x ? x.status : "?") + " upstream=" +
                   JSON.stringify((x ? x.upstream : "").slice(0, 30)) + "]");
            }
            ok(!!r("get") && r("get").upstream.indexOf("GET /thing HTTP/1.1") === 0,
               "get: the upstream received exactly the re-serialised request line " +
               "[" + JSON.stringify(r("get") ? r("get").upstream : "?") + "]");
            ok(!!r("propfind") && r("propfind").upstream.indexOf("PROPFIND") === 0,
               "propfind: an UNREGISTERED but well-formed token is forwarded -- a " +
               "proxy that dropped methods it does not know would break WebDAV");
            ok(!!r("m24") && r("m24").status === "200",
               "m24: a 24-byte method is at the cap and still proxied");
            ok(isBad(r("m25")), "m25: a 25-byte method is past the cap and refused");

            function isBad(x) { return !!x && x.status && x.status[0] === "4"; }

            for (const n of ["nul", "cr", "lf", "sp", "tab", "del", "soh", "vt",
                             "ff", "us", "m0"]) {
                ok(isBad(r(n)),
                   n + ": a method that is not a token is refused [status=" +
                   (r(n) ? r(n).status : "?") + "]");
                ok(notForwarded(n),
                   n + ": ...and NOTHING reached the upstream [upstream=" +
                   JSON.stringify(r(n) ? r(n).upstream : "?") + "]");
            }

            for (let c = 0x00; c <= 0x1f; c++) {
                const n = "c0_" + (c < 16 ? "0" : "") + c.toString(16);
                ok(isBad(r(n)) && notForwarded(n),
                   "method with 0x" + c.toString(16).padStart(2, "0") +
                   " appended: refused AND not forwarded [status=" +
                   (r(n) ? r(n).status : "?") + " upstream=" +
                   JSON.stringify(r(n) ? r(n).upstream : "?") + "]");
            }
            ok(isBad(r("c0_7f")) && notForwarded("c0_7f"),
               "method with DEL appended: refused AND not forwarded");

            for (const n of ["nul", "cr", "lf", "sp", "tab", "del", "soh", "vt",
                             "ff", "us", "m0", "c0_01", "c0_0a", "c0_7f"]) {
                const x = r(n);
                ok(!!x && isBad(x) && isBad({ status: x.local }),
                   n + ": the proxy AND the normal route agree -- both refuse " +
                   "[proxy=" + (x ? x.status : "?") + " normal=" + (x ? x.local : "?") + "]");
            }
            for (const n of ["post", "put", "delete", "options", "bogus"]) {
                const x = r(n);
                ok(!!x && x.local === "405" && x.status === "200",
                   n + ": a well-formed method the route does not list is a 405 on " +
                   "the normal route, while the proxy FORWARDS it -- forwarding a " +
                   "method you do not recognise is the proxy's job " +
                   "[normal=" + (x ? x.local : "?") + " proxy=" + (x ? x.status : "?") + "]");
            }
        }
        reap();
    }
}

print("test_http_proxy_method: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_proxy_method: " + fail + " failures");
