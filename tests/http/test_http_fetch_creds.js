// flags: --std
// timeout: 180
import { fetch } from "dyna:net";
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_fetch_creds: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_creds.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/origins.py`, `
import socket, sys, threading, time, os

NL = chr(13) + chr(10)
LOGFILE = sys.argv[1]
PORTS = {}
LOG = []
LOCK = threading.RLock()

def flush():
    out = []
    with LOCK:
        for name, port, path, head in LOG:
            out.append("--- " + name + " port=" + str(port) + " path=" + path)
            for ln in head.split(NL)[1:]:
                if ln.strip():
                    out.append("    " + ln)
            out.append("")
    with open(LOGFILE, "w") as f:
        f.write(chr(10).join(out) + chr(10))

def bind(name, redirect):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    PORTS[name] = s.getsockname()[1]
    s.listen(64)

    def handle(c):
        try:
            c.settimeout(5.0)
            buf = b""
            while NL.encode() + NL.encode() not in buf:
                b = c.recv(65536)
                if not b:
                    return
                buf += b
            head = buf.split(NL.encode() + NL.encode(), 1)[0].decode("latin1")
            req = head.split(NL, 1)[0]
            path = req.split(" ")[1].split("?")[0] if " " in req else "/"
            with LOCK:
                LOG.append((name, PORTS[name], path, head))
                flush()
            if redirect is not None and path.startswith("/redir"):
                loc = redirect(PORTS[name])
                body = ("HTTP/1.1 302 Found" + NL + "Location: " + loc + NL +
                        "Content-Length: 0" + NL + "Connection: close" + NL + NL).encode()
            else:
                body = (b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" +
                        NL.encode() + NL.encode() + b"hi")
            c.sendall(body)
            time.sleep(0.05)
        except OSError:
            pass
        finally:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            c.close()

    def loop():
        while True:
            conn, _ = s.accept()
            threading.Thread(target=handle, args=(conn,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()

# C redirects to ITSELF: same scheme, same host, same port.
bind("C", lambda p: "http://127.0.0.1:%d/final" % p)
bind("B", None)
bind("A", lambda p: "http://127.0.0.1:%d/final" % PORTS["B"])

with open(sys.argv[2], "w") as f:
    f.write("%d %d %d" % (PORTS["A"], PORTS["B"], PORTS["C"]))
while True:
    time.sleep(1)
`);

    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid; ${cmd} >${T}/${tag}.log 2>&1 & echo $! > ${T}/${tag}.pid`);
    };
    const waitFile = (path) => {
        for (let i = 0; i < 300; i++) {
            if (cat(path).trim() !== "") return true;
            sh("sleep 0.05");
        }
        return false;
    };
    const reap = () => sh(`for f in ${T}/*.pid; do [ -f "$f" ] || continue; ` +
        `p=$(cat "$f" 2>/dev/null); case "$p" in ''|*[!0-9]*) continue;; esac; ` +
        `case "$(ps -p $p -o command= 2>/dev/null)" in *"${T}/"*) ` +
        `kill $p 2>/dev/null;; esac; done; rm -rf ${T}`);

    startBg(`python3 ${T}/origins.py ${T}/log.txt ${T}/ports.txt`, "origins");
    if (!waitFile(`${T}/ports.txt`)) {
        print("  fixture log: " + cat(`${T}/origins.log`).slice(0, 400));
        reap();
        throw new Error("test_http_fetch_creds: origins never bound");
    }
    const PORTS = cat(`${T}/ports.txt`).trim().split(/\s+/).map(Number);
    const [PA, PB, PC] = PORTS;
    ok(PA > 0 && PB > 0 && PC > 0 && PA !== PB && PB !== PC && PA !== PC,
       "fixture: three DISTINCT ports, so each hop's origin change is real [" +
       PORTS.join(",") + "]");

    const H = {
        "authorization": "Basic Q1JFVFRPUkE=",
        "cookie": "sid=SECRET",
        "cookie2": "$Version=1",
        "proxy-authorization": "Basic UFJPWktQ1JFVA==",
        "x-keep": "kept",
    };
    const CREDS = ["authorization", "cookie", "cookie2", "proxy-authorization"];

    const hops = () => {
        const out = [];
        for (const block of cat(`${T}/log.txt`).split(/^--- /m)) {
            if (block.trim() === "") continue;
            const m = /^(\S+) port=(\d+) path=(\S*)/.exec(block);
            if (!m) continue;
            const hdrs = {};
            for (const line of block.split("\n").slice(1)) {
                const c = line.indexOf(":");
                if (c > 0) hdrs[line.slice(0, c).trim().toLowerCase()] = line.slice(c + 1).trim();
            }
            out.push({ name: m[1], port: parseInt(m[2], 10), path: m[3], hdrs });
        }
        return out;
    };
    const hdrAt = (h, name) => {
        const want = name.toLowerCase();
        for (const k of Object.keys(h.hdrs)) if (k === want) return h.hdrs[k];
        return undefined;
    };

    const mark = () => hops().length;
    const since = (m) => hops().slice(m);

    const mCross = mark();
    try {
        const r = await fetch(`http://127.0.0.1:${PA}/redir`, { headers: H });
        ok(r.status === 200, "cross-origin: the redirect chain completes [" + r.status + "]");
    } catch (e) {
        ok(false, "cross-origin: fetch threw [" + String(e && e.message || e) + "]");
    }
    await new Promise(res => setTimeout(res, 200));
    {
        const hs = since(mCross);
        ok(hs.length === 2,
           "cross-origin: both hops were seen by the servers [" + hs.length + "]");
        const first = hs[0], second = hs[1];
        ok(first && first.name === "A" && first.path === "/redir",
           "cross-origin: hop 0 is origin A /redir [" + JSON.stringify(first && first.path) + "]");
        ok(second && second.name === "B" && second.path === "/final",
           "cross-origin: hop 1 is origin B /final -- a different origin [" +
           JSON.stringify(second && second.name) + "]");
        for (const c of CREDS)
            ok(first && hdrAt(first, c) === H[c],
               "cross-origin: hop 0 actually sent " + c + " [" +
               (first ? String(hdrAt(first, c)) : "?") + "]");
        for (const c of CREDS)
            ok(second && hdrAt(second, c) === undefined,
               "M02: origin B never sees " + c + " -- got " +
               (second ? String(hdrAt(second, c)) : "?"));
        ok(second && hdrAt(second, "x-keep") === "kept",
           "cross-origin: a NON-credential header still survives [" +
           (second ? String(hdrAt(second, "x-keep")) : "?") + "]");
    }

    const mSame = mark();
    try {
        const r = await fetch(`http://127.0.0.1:${PC}/redir`, { headers: H });
        ok(r.status === 200, "same-origin: the redirect chain completes [" + r.status + "]");
    } catch (e) {
        ok(false, "same-origin: fetch threw [" + String(e && e.message || e) + "]");
    }
    await new Promise(res => setTimeout(res, 200));
    {
        const hs = since(mSame);
        ok(hs.length === 2, "same-origin: both hops were seen [" + hs.length + "]");
        const second = hs[1];
        ok(hs[0] && hs[1] && hs[0].port === hs[1].port,
           "same-origin: both hops are on the SAME port [" +
           (hs[0] ? hs[0].port : "?") + "/" + (hs[1] ? hs[1].port : "?") + "]");
        for (const c of CREDS)
            ok(second && hdrAt(second, c) === H[c],
               "M02: same-origin PRESERVES " + c + " -- got " +
               (second ? String(hdrAt(second, c)) : "?"));
    }

    const mDirect = mark();
    try {
        const r = await fetch(`http://127.0.0.1:${PB}/final`, { headers: H });
        ok(r.status === 200, "direct: a plain request completes [" + r.status + "]");
    } catch (e) {
        ok(false, "direct: fetch threw [" + String(e && e.message || e) + "]");
    }
    await new Promise(res => setTimeout(res, 200));
    {
        const hs = since(mDirect);
        const only = hs[0];
        ok(hs.length === 1, "direct: exactly one hop was seen [" + hs.length + "]");
        for (const c of CREDS)
            ok(only && hdrAt(only, c) === H[c],
               "direct: a non-redirect request still sends " + c + " -- got " +
               (only ? String(hdrAt(only, c)) : "?"));
    }

    reap();
}

print("test_http_fetch_creds: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_fetch_creds: " + fail + " failures");
