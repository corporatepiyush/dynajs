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
    print("test_http_proxy_framing: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_proxy_frame.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/srv.js`, `
import { App } from "dyna:http";
import * as std from "std";
const app = new App({ port: 0 });
app.proxy("/api", { host: "127.0.0.1", port: +scriptArgs[2] });
app.start();
{ const f = std.open(scriptArgs[1], "w"); f.puts(String(app.port)); f.close(); }
setInterval(() => {}, 1000);
`);

    write(`${T}/drive.py`, `
import os, socket, sys, threading, time

CR = bytes([13]); LF = bytes([10]); CRLF = CR + LF
T = sys.argv[1]
OK = b"HTTP/1.1 200 OK" + CRLF
CHUNKED_TAIL = b"0" + CRLF + CRLF

CASES = {
    # ---- the declared length and the delivered length agree: relay ------
    "cl_exact":   OK + b"Content-Length: 2" + CRLF + CRLF + b"UP",
    "cl0":        OK + b"Content-Length: 0" + CRLF + CRLF,
    "cl_big":     OK + b"Content-Length: 1024" + CRLF + CRLF + b"Z" * 1024,
    # ---- the declared length and the delivered length disagree: REFUSE ---
    "cl_short":   OK + b"Content-Length: 2" + CRLF + CRLF + b"UPX",
    "cl_long":    OK + b"Content-Length: 5" + CRLF + CRLF + b"UP",
    "cl_zero_1":  OK + b"Content-Length: 0" + CRLF + CRLF + b"X",
    # the smuggle: a short declared length, then a WHOLE second response
    "smuggle":    OK + b"Content-Length: 2" + CRLF + CRLF + b"UPX" +
                  b"HTTP/1.1 200 OK" + CRLF + b"Content-Length: 4" + CRLF + CRLF +
                  b"JUNK",
    # ---- a length that is not a plain decimal: REFUSE -------------------
    "cl_plus":    OK + b"Content-Length: +2" + CRLF + CRLF + b"UP",
    "cl_junk":    OK + b"Content-Length: abc" + CRLF + CRLF + b"UP",
    "cl_empty":   OK + b"Content-Length:" + CRLF + CRLF + b"UP",
    "cl_huge":    OK + b"Content-Length: 999999999" + CRLF + CRLF + b"UP",
    "cl_osp":     OK + b"Content-Length:  2  " + CRLF + CRLF + b"UP",
    # duplicates: agreeing is legal, disagreeing is refused (RFC 9112 6.3)
    "cl_2eq":     OK + b"Content-Length: 2" + CRLF + b"Content-Length: 2" +
                 CRLF + CRLF + b"UP",
    "cl_2diff":   OK + b"Content-Length: 2" + CRLF + b"Content-Length: 3" +
                 CRLF + CRLF + b"UP",
    # ---- Transfer-Encoding: refused, never relayed -----------------------
    "chunked":    OK + b"Transfer-Encoding: chunked" + CRLF + CRLF +
                 b"2" + CRLF + b"UP" + CRLF + CHUNKED_TAIL,
    "chunked_x":  OK + b"Transfer-Encoding: chunked" + CRLF + CRLF +
                 b"2;ext=1" + CRLF + b"UP" + CRLF + b"0" + CRLF + b"X-T: 1" +
                 CRLF + CRLF,
    "te_lower":   OK + b"transfer-encoding: chunked" + CRLF + CRLF +
                 b"2" + CRLF + b"UP" + CRLF + CHUNKED_TAIL,
    "te_id":      OK + b"Transfer-Encoding: identity" + CRLF +
                 b"Content-Length: 2" + CRLF + CRLF + b"UP",
    "cl_te":      OK + b"Content-Length: 2" + CRLF +
                 b"Transfer-Encoding: chunked" + CRLF + CRLF +
                 b"2" + CRLF + b"UP" + CRLF + CHUNKED_TAIL,
    # ---- close-delimited: EOF is the framing rule, and must keep working -
    "nocl":       OK + b"Connection: close" + CRLF + CRLF + b"body to close",
    "nocl_empty": OK + b"Connection: close" + CRLF + CRLF,
    "h10_cl":     b"HTTP/1.0 200 OK" + CRLF + b"Content-Length: 2" + CRLF +
                 CRLF + b"UP",
    "h10_nocl":   b"HTTP/1.0 200 OK" + CRLF + b"Connection: close" + CRLF +
                 CRLF + b"body to close",
    # ---- statuses that cannot carry a body are relayed as they arrived ----
    "s204":       b"HTTP/1.1 204 No Content" + CRLF + b"Content-Length: 0" +
                 CRLF + CRLF,
    "s404":       b"HTTP/1.1 404 Not Found" + CRLF + b"Content-Length: 2" +
                 CRLF + CRLF + b"UP",
    # ---- a head that never terminates: refused, not waited on -------------
    "no_head":    b"garbage with no blank line at all",
    # ---- NEAR-MISS NAMES. A framing header is found by NAME, and the name
    # comparison must be exact: a case-insensitive compare of the name's LENGTH
    # bytes against the literal matches a PREFIX, so "Content-Len: 99" would be
    # read as a Content-Length -- and one that is not a Content-Length is the
    # smuggling primitive this whole file exists to stop. Each of these is a
    # near miss and must be an ORDINARY header, framing left alone.
    "pfx_len":    OK + b"Content-Length: 2" + CRLF + b"Content-Len: 99" + CRLF + CRLF + b"UP",
    "pfx_l":      OK + b"Content-Length: 2" + CRLF + b"Content-L: 99" + CRLF + CRLF + b"UP",
    "pfx_long":   OK + b"Content-Length: 2" + CRLF + b"Content-Length-Extra: 99" + CRLF + CRLF + b"UP",
    "pfx_x":      OK + b"Content-Length: 2" + CRLF + b"X-Content-Length: 99" + CRLF + CRLF + b"UP",
    "pfx_te":     OK + b"Content-Length: 2" + CRLF + b"Transfer-Encodin: chunked" + CRLF + CRLF + b"UP",
    # the real thing, in cases the comparison must accept
    "pfx_upper":  b"HTTP/1.1 200 OK" + CRLF + b"CONTENT-LENGTH: 2" + CRLF + b"X-A: b" + CRLF + CRLF + b"UP",
    "pfx_mixed":  b"HTTP/1.1 200 OK" + CRLF + b"CoNtEnT-LeNgTh: 2" + CRLF + b"X-A: b" + CRLF + CRLF + b"UP",
    # a Content-Length inside a VALUE is not the header
    "cl_in_value": OK + b"Content-Length: 2" + CRLF + b"X-A: Content-Length: 2" + CRLF + CRLF + b"UP",
}

up = socket.socket(); up.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
up.bind(("127.0.0.1", 0)); up.listen(64)
UPPORT = up.getsockname()[1]

def one(c):
    try:
        c.settimeout(4.0)
        d = b""
        while CRLF + CRLF not in d:
            x = c.recv(65536)
            if not x:
                break
            d += x
        # The proxy RE-SERIALISES the path: /api/x reaches the upstream as /x.
        p = d.split(b" ")[1].decode("latin-1").lstrip("/") if b" " in d else ""
        c.sendall(CASES.get(p, CASES["cl_exact"]))
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
    print("NOPROXY\tERR", flush=True)
    raise SystemExit(0)

def ask(path):
    acc = [b""]

    def pull():
        x = s.recv(65536)
        if x:
            acc[0] += x
        return x

    s = socket.create_connection(("127.0.0.1", PROXY), 5.0)
    s.settimeout(1.5)
    try:
        s.sendall(b"GET /api/" + path.encode() + b" HTTP/1.1" + CRLF +
                  b"Host: victim" + CRLF + b"Content-Length: 0" + CRLF + CRLF)
        while CRLF + CRLF not in acc[0]:
            if not pull():
                break
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
                if not pull():
                    break
    except socket.timeout:
        acc[0] += b"<<TIMEOUT>>"
    except OSError as e:
        acc[0] += ("<<%s>>" % e).encode()
    s.close()
    return acc[0]

def facts(buf):
    i = buf.find(CRLF + CRLF)
    if i < 0:
        return (b"", -1, b"")
    parts = buf[:i].split(CRLF)
    st = parts[0].split(b" ")[1] if len(parts[0].split(b" ")) > 1 else b""
    cl = -1
    for ln in parts[1:]:
        if b":" in ln:
            k, v = ln.split(b":", 1)
            if k.strip().lower() == b"content-length":
                try: cl = int(v.strip())
                except ValueError: cl = -1
    return (st, cl, buf[i + 4:])

for name in sorted(CASES):
    st, cl, body = facts(ask(name))
    print("%s\\t%s\\t%d\\t%d\\t%s" % (
        name, st.decode("latin-1", "replace"), cl, len(body),
        body[:200].hex()), flush=True)

# LIVENESS. The App answered every case or the whole loop would have stalled;
# this proves the PROCESS is still there, which is the difference between a
# refusal and a SIGSEGV. One more request, after everything.
st, cl, body = facts(ask("cl_exact"))
print("alive\\t%s\\t%d\\t%d\\t%s" % (st.decode("latin-1", "replace"), cl,
                                    len(body), body[:200].hex()), flush=True)
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
    ok(UPPORT > 0, "fixture: the origin bound a port");
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
                if (f.length < 5 || !f[0]) continue;
                rows[f[0]] = { status: f[1], declared: +f[2], actual: +f[3],
                               body: hexToStr(f[4]) };
            }
            const r = (n) => rows[n];
            const is5xx = (x) => x && x.status && x.status[0] === "5";
            // RFC 9110 8.6: a 204 must not carry Content-Length, so the
            // relayed 204 is self-consistent with no declared length and an
            // empty (bodyless) actual.
            const selfConsistent = (x) => !!x && (x.declared === x.actual ||
                (x.status === "204" && x.declared === -1 && x.actual === 0));

            for (const n of ["cl_exact", "cl0", "cl_big", "cl_osp"]) {
                const x = r(n);
                ok(!!x && x.status === "200" && selfConsistent(x),
                   n + ": a declared length that matches what arrived is proxied " +
                   "[status=" + (x ? x.status : "?") + " declared=" + (x ? x.declared : "?") +
                   " actual=" + (x ? x.actual : "?") + "]");
            }
            ok(!!r("cl_exact") && r("cl_exact").body === "UP",
               "cl_exact: the body is exactly the two declared bytes");
            ok(!!r("cl_big") && r("cl_big").declared === 1024 && r("cl_big").actual === 1024,
               "cl_big: a 1 KiB body arrives whole and whole only " +
               "[declared=" + (r("cl_big") ? r("cl_big").declared : "?") +
               " actual=" + (r("cl_big") ? r("cl_big").actual : "?") + "]");

            for (const n of ["cl_short", "cl_long", "cl_zero_1", "smuggle"]) {
                const x = r(n);
                ok(is5xx(x),
                   n + ": a delivered length that contradicts the declared one is a " +
                   "named 502, not a quiet truncation [status=" + (x ? x.status : "?") + "]");
                ok(!!x && x.body.indexOf("HTTP/1.") < 0,
                   n + ": no second HTTP response is delivered as body [" +
                   (x ? x.body.slice(0, 60) : "?") + "]");
            }

            for (const n of ["cl_plus", "cl_junk", "cl_empty", "cl_huge", "cl_2diff"]) {
                ok(is5xx(r(n)),
                   n + ": an unusable Content-Length is a named 502 [status=" +
                   (r(n) ? r(n).status : "?") + "]");
            }
            ok(!!r("cl_2eq") && r("cl_2eq").status === "200" && selfConsistent(r("cl_2eq")),
               "cl_2eq: an AGREEING duplicate Content-Length stays legal [status=" +
               (r("cl_2eq") ? r("cl_2eq").status : "?") + "]");

            for (const n of ["chunked", "chunked_x", "te_lower", "te_id", "cl_te"]) {
                const x = r(n);
                ok(is5xx(x),
                   n + ": an upstream Transfer-Encoding is refused rather than " +
                   "relayed under a re-declared Content-Length [status=" +
                   (x ? x.status : "?") + "]");
            }

            for (const n of ["nocl", "nocl_empty", "h10_cl", "h10_nocl", "s204", "s404"]) {
                const x = r(n);
                ok(!!x && x.status !== "" && !is5xx(x) && selfConsistent(x),
                   n + ": close-delimited / HTTP/1.0 framing still proxies " +
                   "[status=" + (x ? x.status : "?") + " declared=" + (x ? x.declared : "?") +
                   " actual=" + (x ? x.actual : "?") + "]");
            }
            ok(!!r("nocl") && r("nocl").body === "body to close",
               "nocl: the close-delimited body arrives whole [" +
               (r("nocl") ? r("nocl").body : "?") + "]");
            ok(!!r("s204") && r("s204").status === "204",
               "s204: a 204 is relayed as a 204, not rewritten");
            ok(!!r("s404") && r("s404").status === "404" && r("s404").body === "UP",
               "s404: an upstream 404 and its body are relayed");

            ok(is5xx(r("no_head")),
               "no_head: an upstream that never terminates its head is a named 502 " +
               "and the suite does not wait on it [status=" +
               (r("no_head") ? r("no_head").status : "?") + "]");

            for (const n of ["pfx_len", "pfx_l", "pfx_long", "pfx_x", "pfx_te",
                             "cl_in_value"]) {
                const x = r(n);
                ok(!!x && x.status === "200" && x.declared === 2 && x.actual === 2,
                   n + ": a near-miss header name is an ordinary header, not the " +
                   "framing one [status=" + (x ? x.status : "?") + " declared=" +
                   (x ? x.declared : "?") + " actual=" + (x ? x.actual : "?") + "]");
            }
            for (const n of ["pfx_upper", "pfx_mixed"]) {
                const x = r(n);
                ok(!!x && x.status === "200" && x.declared === 2 && x.actual === 2,
                   n + ": the real Content-Length is honoured whatever its case " +
                   "[declared=" + (x ? x.declared : "?") + "]");
            }

            const a = r("alive");
            ok(!!a && a.status === "200" && a.body === "UP",
               "the App PROCESS survived every framing shape: the use-after-free " +
               "on the EOF branch killed it (SIGSEGV) for any close-delimited " +
               "response, and a dead process cannot answer a final request " +
               "[status=" + (a ? a.status : "?") + "]");
        }
        reap();
    }
}

print("test_http_proxy_framing: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_proxy_framing: " + fail + " failures");
