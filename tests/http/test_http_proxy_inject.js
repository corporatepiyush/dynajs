// flags: --std
// timeout: 300
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const hexToStr = (h) => {
    let s = "";
    for (let i = 0; i + 1 < h.length; i += 2)
        s += String.fromCharCode(parseInt(h.substr(i, 2), 16));
    return s;
};
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_proxy_inject: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_proxy_inj.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
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
import socket, os, sys, threading, time

CR = bytes([13]); LF = bytes([10]); CRLF = CR + LF

def head(body, extra=b""):
    # extra is WHOLE header lines, each already CRLF-terminated, so the head
    # ends with a blank line. One CRLF here is a head that never terminates.
    return b"HTTP/1.1 200 OK" + CRLF + b"Content-Length: 2" + CRLF + \
           extra + CRLF + body

# Every C0 byte individually, plus DEL. Each is a VALUE, never a terminator.
CTL = {}
for _c in list(range(0x00, 0x20)) + [0x7f]:
    CTL["c0_%02x" % _c] = head(b"UP", b"X-Evil: a" + bytes([_c]) + b"b" + CRLF)

CASES = {}
CASES.update(CTL)
# the two terminator-shaped cases, which are NOT injections (see the header)
CASES["crlf_pair"]   = head(b"UP", b"X-Evil: a" + CRLF + b"X-Second: b" + CRLF)
CASES["dblcrlf"]     = head(b"UP", b"X-Evil: a" + CRLF)
# high bytes and malformed UTF-8: legal obs-text, must stay on ONE line
CASES["c1_80"]       = head(b"UP", b"X-Evil: a" + bytes([0xc2, 0x80]) + b"b" + CRLF)
CASES["c1_85"]       = head(b"UP", b"X-Evil: a" + bytes([0xc2, 0x85]) + b"b" + CRLF)
CASES["c1_9f"]       = head(b"UP", b"X-Evil: a" + bytes([0xc2, 0x9f]) + b"b" + CRLF)
CASES["u2028"]       = head(b"UP", b"X-Evil: a" + bytes([0xe2, 0x80, 0xa8]) + b"b" + CRLF)
CASES["overlong"]    = head(b"UP", b"X-Evil: a" + bytes([0xc0, 0xaf]) + b"b" + CRLF)
CASES["surrogate"]   = head(b"UP", b"X-Evil: a" + bytes([0xed, 0xa0, 0x80]) + b"b" + CRLF)
CASES["bare_e9"]     = head(b"UP", b"X-Evil: caf" + bytes([0xe9]) + b"X" + CRLF)
# length: none of these is a control, so none may be refused
for _n in (255, 256, 257, 500, 4000):
    CASES["len%d" % _n] = head(b"UP", b"X-Evil: " + b"a" * _n + CRLF)
# the status line, copied verbatim before this change
CASES["st_barelf"]   = b"HTTP/1.1 200 O" + LF + b"K" + CRLF + b"Content-Length: 2" + CRLF + CRLF + b"UP"
CASES["st_barecr"]   = b"HTTP/1.1 200 O" + CR + b"K" + CRLF + b"Content-Length: 2" + CRLF + CRLF + b"UP"
CASES["st_ctl"]      = b"HTTP/1.1 200 O" + bytes([1]) + b"K" + CRLF + b"Content-Length: 2" + CRLF + CRLF + b"UP"
CASES["st_4digit"]   = b"HTTP/1.1 2000 W" + CRLF + b"Content-Length: 2" + CRLF + CRLF + b"UP"
CASES["st_nospace"]  = b"HTTP/1.1200 W" + CRLF + b"Content-Length: 2" + CRLF + CRLF + b"UP"
CASES["st_ok"]       = b"HTTP/1.1 404 Not Found" + CRLF + b"Content-Length: 2" + CRLF + CRLF + b"UP"
# the header NAME, copied verbatim before this change
CASES["name_ctl"]    = head(b"UP", b"X-" + bytes([1]) + b"Evil: a" + CRLF)
CASES["name_sp"]     = head(b"UP", b"X Evil: a" + CRLF)
CASES["name_nl"]     = head(b"UP", b"X" + LF + b"Evil: a" + CRLF)
CASES["name_high"]   = head(b"UP", b"X" + bytes([0xe9]) + b"vil: a" + CRLF)
CASES["name_fold"]   = head(b"UP", b"X-A: a" + CRLF + b"\tX-B: b" + CRLF)
CASES["fold_nocolon"] = head(b"UP", b"X-A: a" + CRLF + b"\tcont" + CRLF)
# hop-by-hop stripping must still work, and ordinary headers must survive
CASES["hop"]         = head(b"UP", b"Connection: keep-alive" + CRLF +
                                      b"Keep-Alive: timeout=5" + CRLF +
                                      b"Proxy-Authenticate: Basic" + CRLF +
                                      b"X-Keep: kept" + CRLF)
# Any Transfer-Encoding in an upstream RESPONSE is refused outright (RFC 9112
# 6.3 forbids forwarding one, and this proxy re-frames with its own
# Content-Length so it cannot verify any transfer coding). "identity" is the
# interesting case: it is legal-looking and every old server sends it.
CASES["hop_te"]      = head(b"UP", b"Transfer-Encoding: identity" + CRLF)
# a header with no colon at all: dropped, never emitted
CASES["nocolon"]     = head(b"UP", b"X-Evil-a" + CRLF)
# CONTROLS THAT MUST KEEP WORKING
CASES["legal"]       = head(b"UP", b"X-Evil: normal-value" + CRLF)
CASES["legal_tok"]   = head(b"UP", b"X_Evil.1: a:b/c!d$e&f'g*h+i-j%k^l" +
                                 bytes([96]) + b"m|n~o" + CRLF)
CASES["legal_empty"] = head(b"UP", b"X-Evil:" + CRLF)
CASES["legal_obs"]   = head(b"UP", b"X-Evil: caf" + bytes([0xe9]) + CRLF)
CASES["legal_url"]   = head(b"UP", b"Location: http://h:1/p?q=1&r=2" + CRLF)
# a GENUINE upstream Set-Cookie, on its own line. A conforming proxy relays
# it: the upstream sent that header, and dropping it would be a broken proxy,
# not a hardened one. See the header comment.
CASES["real_cookie"] = head(b"UP", b"X-Evil: a" + CRLF + b"Set-Cookie: real=1" + CRLF)

up = socket.socket(); up.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
up.bind(("127.0.0.1", 0)); up.listen(64)
UPPORT = up.getsockname()[1]

def serve():
    while True:
        try:
            c, _ = up.accept()
        except OSError:
            return
        threading.Thread(target=one, args=(c,), daemon=True).start()

def one(c):
    try:
        c.settimeout(4.0)
        d = b""
        while CRLF + CRLF not in d:
            x = c.recv(65536)
            if not x:
                break
            d += x
        # The proxy RE-SERIALISES the path: /api/<case> reaches the upstream
        # as /<case>. Key the case on that, not on the client's URL.
        p = d.split(b" ")[1].decode("latin-1").lstrip("/")
        c.sendall(CASES.get(p, CASES["legal"]))
        c.shutdown(socket.SHUT_WR); time.sleep(0.02)
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass

threading.Thread(target=serve, daemon=True).start()
T = sys.argv[1]
open(os.path.join(T, "up.port"), "w").write(str(UPPORT))

# Wait for the harness to announce the App's port. Bounded: a fixture that
# never gets one must not hang the suite.
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
    """One request; read the head, then exactly the declared body, then STOP.

    Reading to close would cost the full socket timeout on every refusal --
    the proxy's own 502 answers are keep-alive, not close -- and a 50-case
    suite would spend four minutes waiting for sockets nobody was going to
    close."""
    s = socket.create_connection(("127.0.0.1", PROXY), 5.0)
    s.settimeout(1.5)
    s.sendall(b"GET /api/" + path.encode() + b" HTTP/1.1" + CRLF +
              b"Host: victim" + CRLF + b"Content-Length: 0" + CRLF + CRLF)
    acc = [b""]

    def pull():
        x = s.recv(65536)
        if x:
            acc[0] += x
        return x

    try:
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
                        try:
                            cl = int(v.strip())
                        except ValueError:
                            cl = 0
            while len(acc[0]) - (i + 4) < cl:
                if not pull():
                    break
    except socket.timeout:
        acc[0] += b"<<TIMEOUT>>"
    s.close()
    return acc[0]

def facts(buf):
    """LINE-ANCHORED. A bare CR/LF inside a line, counted over the status line
    and every header line, is the only thing that means 'this head parses two
    ways'. A payload riding its own line does not."""
    i = buf.find(CRLF + CRLF)
    if i < 0:
        return (b"", 99, 0, [], b"")
    h = buf[:i]
    body = buf[i + 4:]
    lines = h.split(CRLF)
    bare = 0
    for ln in lines:
        for by in ln:
            if by == 13 or by == 10:
                bare += 1
    names = []
    for ln in lines[1:]:
        if b":" in ln:
            names.append(ln.split(b":", 1)[0].strip().lower().decode("latin-1"))
        else:
            names.append("<none>")
    parts = lines[0].split(b" ")
    st = parts[1] if len(parts) > 1 else b""
    return (st, bare, len(names), names, body)

for name in sorted(CASES):
    try:
        st, bare, nn, names, body = facts(ask(name))
    except Exception as e:
        print("%s\tERR\t%d\t0\t-\t%s" % (name, 1, ("%s" % e).encode().hex()), flush=True)
        continue
    # flush per line: stdout is a redirected file, so it is BLOCK buffered and
    # a harness waiting for the first row would wait forever.
    print("%s\t%s\t%d\t%d\t%s\t%s" % (
        name, st.decode("latin-1", "replace"), bare, nn,
        ",".join(names).encode().hex(), body[:400].hex()), flush=True)
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
    ok(UPPORT > 0, "fixture: the hostile origin bound a port");
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
            for (let i = 0; i < 600; i++) {
                if ((cat(`${T}/drv.log`) || "").indexOf("DONE") >= 0 ||
                    (cat(`${T}/drv.log`) || "").indexOf("NOPROXY") >= 0) break;
                sh("sleep 0.05");
            }
            const out = cat(`${T}/drv.log`);
            const rows = {};
            for (const line of out.split("\n")) {
                const f = line.split("\t");
                if (f[0] === "DONE") continue;
                if (f.length < 6 || !f[0]) continue;
                rows[f[0]] = {
                    status: f[1],
                    bare: +f[2],
                    nlines: +f[3],
                    names: f[4] === "-" ? [] : hexToStr(f[4]).split(","),
                    body: hexToStr(f[5]),
                };
            }
            const is5xx = (r) => r && r.status && r.status[0] === "5";
            const cookieLines = (r) => (r ? r.names.filter((n) => n === "set-cookie").length : 0);

            for (let c = 0x00; c <= 0x1f; c++) {
                const n = "c0_" + (c < 16 ? "0" : "") + c.toString(16);
                const r = rows[n];
                ok(!!r, "value 0x" + c.toString(16) + ": the case ran");
                if (!r) continue;
                ok(cookieLines(r) === 0, "value 0x" + c.toString(16).padStart(2, "0") +
                   ": no Set-Cookie line reaches the client [" + r.names.join(",") + "]");
                ok(r.bare === 0, "value 0x" + c.toString(16).padStart(2, "0") +
                   ": the head has no bare CR or LF in a line");
                ok(is5xx(r), "value 0x" + c.toString(16).padStart(2, "0") +
                   ": refused with a named 5xx, not a repaired head [status=" + r.status + "]");
            }
            let r = rows["c0_7f"];
            ok(!!r && cookieLines(r) === 0 && r.bare === 0 && is5xx(r),
               "value DEL: refused, no split, named 5xx [" + (r ? r.status + " " + r.names.join(",") : "missing") + "]");

            r = rows["crlf_pair"];
            ok(!!r && r.status === "200" && r.bare === 0 && r.nlines === 4,
               "CRLF pair: the upstream sent two headers and BOTH are relayed " +
               "[status=" + (r ? r.status : "?") + " lines=" + (r ? r.names.join(",") : "?") + "]");
            r = rows["dblcrlf"];
            ok(!!r && r.status === "200" && r.nlines === 3 && r.body === "UP",
               "double CRLF: the head ends where the upstream's ended, and the " +
               "rest is body, not a header [" +
               (r ? r.status + " " + r.names.join(",") + " body=" + r.body : "?") + "]");

            for (const n of ["c1_80", "c1_85", "c1_9f", "u2028", "overlong",
                             "surrogate", "bare_e9", "legal_obs"]) {
                r = rows[n];
                ok(!!r && r.status === "200" && r.bare === 0 && cookieLines(r) === 0 &&
                   r.names.indexOf("x-evil") >= 0,
                   n + ": a high byte in a value is one line, not a split " +
                   "[status=" + (r ? r.status : "?") + " lines=" + (r ? r.names.join(",") : "?") + "]");
            }

            for (const n of ["len255", "len256", "len257", "len500", "len4000"]) {
                r = rows[n];
                ok(!!r && r.status === "200" && r.names.indexOf("x-evil") >= 0,
                   n + ": a long value is relayed, not refused for its length " +
                   "[status=" + (r ? r.status : "?") + "]");
            }

            for (const n of ["st_barelf", "st_barecr", "st_ctl", "st_4digit", "st_nospace"]) {
                r = rows[n];
                ok(!!r && is5xx(r) && r.bare === 0 && cookieLines(r) === 0,
                   n + ": a malformed status line is refused, not relayed " +
                   "[status=" + (r ? r.status : "?") + "]");
            }
            r = rows["st_ok"];
            ok(!!r && r.status === "404" && r.bare === 0,
               "status line control: 404 and its reason phrase are relayed");

            for (const n of ["name_ctl", "name_sp", "name_nl", "name_high", "name_fold"]) {
                r = rows[n];
                ok(!!r && is5xx(r) && r.bare === 0 && cookieLines(r) === 0,
                   n + ": a name that is not a token is refused " +
                   "[status=" + (r ? r.status : "?") + " lines=" + (r ? r.names.join(",") : "?") + "]");
            }

            for (const n of ["legal", "legal_tok", "legal_empty", "legal_url"]) {
                r = rows[n];
                ok(!!r && r.status === "200" && r.bare === 0,
                   n + ": an ordinary upstream response is proxied unchanged " +
                   "[status=" + (r ? r.status : "?") + "]");
            }
            r = rows["hop"];
            ok(!!r && r.status === "200" && r.nlines === 3 &&
               r.names.join(",") === "x-keep,content-length,connection",
               "hop-by-hop: the ordinary header survives, the upstream's hop-by-hop " +
               "headers do not [" + (r ? r.status + " " + r.names.join(",") : "?") + "]");
            r = rows["hop_te"];
            ok(!!r && is5xx(r),
               "an upstream Transfer-Encoding (even `identity`) is refused, not " +
               "relayed under a re-declared Content-Length [" + (r ? r.status : "?") + "]");
            r = rows["fold_nocolon"];
            ok(!!r && r.status === "200" && r.names.indexOf("<none>") < 0 &&
               r.names.indexOf("x-a") >= 0,
               "an obs-fold line with no colon is dropped, never emitted as one " +
               "[" + (r ? r.status + " " + r.names.join(",") : "?") + "]");
            r = rows["nocolon"];
            ok(!!r && r.status === "200" && r.names.indexOf("<none>") < 0,
               "a header line with no colon is dropped, never emitted as one " +
               "[" + (r ? r.names.join(",") : "?") + "]");
            r = rows["real_cookie"];
            ok(!!r && r.status === "200" && cookieLines(r) === 1,
               "a GENUINE upstream Set-Cookie is relayed: at the HTTP layer the " +
               "upstream sent a header, and a proxy that dropped it would be " +
               "broken, not hardened [" + (r ? r.status + " " + r.names.join(",") : "?") + "]");

            r = rows["legal"];
            ok(!!r && r.status === "200",
               "the App survived every hostile response head (a dead App reads as " +
               "no response at all)");
        }
        reap();
    }
}

print("test_http_proxy_inject: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_proxy_inject: " + fail + " failures");
