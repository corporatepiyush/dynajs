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
const CH = (...cs) => String.fromCharCode(...cs);
const CR = CH(13), LF = CH(10);

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_client_framing: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_frame.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/evil.py`, `
import socket, sys, threading, time, os

NL = chr(13) + chr(10)
CR, LF = chr(13), chr(10)
PORTFILE = sys.argv[1]

def cl(body, declared, extra=b""):
    return b"".join([b"HTTP/1.1 200 OK" + NL.encode() +
                     b"Content-Length: " + str(declared).encode() + NL.encode() + NL.encode(),
                     body, extra])

def st(line, tail=None):
    if tail is None:
        tail = b"Content-Length: 2" + NL.encode() + NL.encode() + b"hi"
    return b"HTTP/1.1 " + line + NL.encode() + tail

CASES = {
    # ---- H-02: Content-Length enforcement --------------------------------
    "/h1":  ("CL:3 with 6 bytes sent",        cl(b"ABCDEF", 3)),
    "/h2":  ("CL:0 with 5 bytes sent",        cl(b"HELLO", 0)),
    "/h3":  ("CL:1, A, then a WHOLE 2nd response",
             cl(b"A", 1, b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 4" +
                        NL.encode() + NL.encode() + b"BODYTRAILING-JUNK")),
    "/h4":  ("CL:1 with 4096 bytes sent",     cl(b"Z" * 4096, 1)),
    "/h5":  ("CL:4 with 5 bytes sent",        cl(b"ABCDE", 4)),
    "/h6":  ("CL:4 plus a trailing CRLF",     cl(b"ABCD", 4, NL.encode())),
    "/h7":  ("CL:0 plus a whole pipelined response",
             cl(b"", 0, b"HTTP/1.1 204 No Content" + NL.encode() + NL.encode())),
    "/h8":  ("CL:6 with exactly 6 sent",      cl(b"ABCDEF", 6)),
    "/h9":  ("CL:3 with exactly 3 sent",      cl(b"ABC", 3)),
    "/h10": ("CL:1 with exactly 1 sent",      cl(b"A", 1)),
    "/h11": ("CL:0 with nothing sent",        cl(b"", 0)),
    "/h12": ("CL:3 with only 2 sent, then close", cl(b"AB", 3)),
    "/h13": ("no CL at all, read-to-close",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Connection: close" + NL.encode() +
             NL.encode() + b"ABCDEF"),
    "/h14": ("CL:6, 6 sent, then a 2nd response",
             cl(b"ABCDEF", 6, b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" +
                            NL.encode() + NL.encode() + b"hi")),
    "/h15": ("chunked, the SAME bytes as /h1",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Transfer-Encoding: chunked" + NL.encode() +
             NL.encode() + b"6" + NL.encode() + b"ABCDEF" + NL.encode() + b"0" + NL.encode() + NL.encode()),
    "/h16": ("chunked 1 byte, then a whole 2nd response",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Transfer-Encoding: chunked" + NL.encode() +
             NL.encode() + b"1" + NL.encode() + b"A" + NL.encode() + b"0" + NL.encode() +
             NL.encode() + b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 4" +
             NL.encode() + NL.encode() + b"BODYJUNK"),
    "/h17": ("CL:1, A, then 64 KiB of junk",
             cl(b"A", 1, b"Q" * 65536)),

    # ---- M-01: bare CR / bare LF in a response header value --------------
    "/m1":  ("bare CR in a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: v" + CR.encode() + b"Set-Cookie: p=1" + NL.encode() + NL.encode() + b"hi"),
    "/m2":  ("bare LF in a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: a" + LF.encode() + b"b" + NL.encode() + NL.encode() + b"hi"),
    "/m3":  ("CR then space at the end of a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: v" + CR.encode() + b" " + NL.encode() + NL.encode() + b"hi"),
    "/m4":  ("CR then CR",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: v" + CR.encode() + CR.encode() + NL.encode() + NL.encode() + b"hi"),
    # controls that must keep working
    "/m5":  ("control: two headers, CRLF framing",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: a" + NL.encode() + b"X-B: b" + NL.encode() + NL.encode() + b"hi"),
    "/m6":  ("control: TAB in a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: a" + chr(9).encode() + b"b" + NL.encode() + NL.encode() + b"hi"),
    "/m7":  ("control: obs-text 0xe9 in a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: caf" + bytes([0xe9]) + NL.encode() + NL.encode() + b"hi"),
    "/m8":  ("control: an empty value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A:" + NL.encode() + NL.encode() + b"hi"),
    "/m9":  ("control: a value with a colon in it",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"Location: http://h:1/p?q=1" + NL.encode() + NL.encode() + b"hi"),
    # controls that were ALREADY refused and must stay refused
    "/m10": ("control: NUL in a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: a" + bytes([0]) + b"b" + NL.encode() + NL.encode() + b"hi"),
    "/m11": ("control: VT in a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: a" + bytes([0x0b]) + b"b" + NL.encode() + NL.encode() + b"hi"),
    "/m12": ("control: DEL in a value",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A: a" + bytes([0x7f]) + b"b" + NL.encode() + NL.encode() + b"hi"),
    "/m13": ("control: CR in a header NAME",
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             b"X-A" + CR.encode() + b"B: v" + NL.encode() + NL.encode() + b"hi"),
    "/m14": ("control: LF-only line endings throughout",
             LF.encode().join([b"HTTP/1.1 200 OK", b"Content-Length: 2", b"X-A: v", b"", b"hi"])),

    # ---- M-03: the status line -------------------------------------------
    "/s1":  ("1999 (4 digits, 1xx-prefixed)",  st(b"1999 Weird")),
    "/s2":  ("1200 (4 digits, 1xx-prefixed)",  st(b"1200 Weird")),
    "/s3":  ("2000 (4 digits, 2xx-prefixed)",  st(b"2000 Weird")),
    "/s4":  ("99999 (5 digits)",               st(b"99999 Weird")),
    "/s5":  ("1999, then a real 200",
             st(b"1999 Weird", NL.encode()) +
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             NL.encode() + b"hi"),
    "/s6":  ("2 digits",                       st(b"20 Weird")),
    "/s7":  ("a non-digit status",             st(b"2xx Weird")),
    "/s8":  ("4 digits then junk",             st(b"2000X Weird")),
    # controls: legal statuses must keep arriving
    "/s10": ("100 Continue, then a real 200",
             st(b"100 Continue", NL.encode()) +
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             NL.encode() + b"hi"),
    "/s11": ("199 (legal interim), then a 200",
             st(b"199 Odd", NL.encode()) +
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             NL.encode() + b"hi"),
    "/s12": ("101 Switching Protocols",
             b"HTTP/1.1 101 Switching Protocols" + NL.encode() + b"Upgrade: x" + NL.encode() +
             b"Connection: Upgrade" + NL.encode() + NL.encode()),
    "/s13": ("100, no reason, then a 200",
             b"HTTP/1.1 100" + NL.encode() + NL.encode() +
             b"HTTP/1.1 200 OK" + NL.encode() + b"Content-Length: 2" + NL.encode() +
             NL.encode() + b"hi"),
    "/s14": ("099 (three digits, leading zero)", st(b"099 Odd")),
    "/s15": ("199 -- a 3-digit 1xx is an interim, so it is not a final status",
             st(b"199 Odd")),
    "/s16": ("200", st(b"200 OK")),
    "/s17": ("204, no reason, no body",
             b"HTTP/1.1 204" + NL.encode() + NL.encode()),
    "/s18": ("299 (2xx upper edge)", st(b"299 Odd")),
    "/s19": ("300", st(b"300 Multiple Choices")),
    "/s20": ("404", st(b"404 Not Found")),
    "/s21": ("499", st(b"499 Odd")),
    "/s22": ("500", st(b"500 Internal Server Error")),
    "/s23": ("599 (5xx upper edge)", st(b"599 Odd")),
    "/s24": ("999 (three digits, out of range)", st(b"999 Odd")),
    "/s24b": ("598 (just under the range edge)", st(b"598 Odd")),
    "/s25": ("HTTP/1.0 200", b"HTTP/1.0 200 OK" + NL.encode() + b"Content-Length: 2" +
                               NL.encode() + NL.encode() + b"hi"),
    "/s26": ("HTTP/2 200", b"HTTP/2 200 OK" + NL.encode() + b"Content-Length: 2" +
                           NL.encode() + NL.encode() + b"hi"),
}

def handle(c):
    try:
        c.settimeout(5.0)
        buf = b""
        while NL.encode() + NL.encode() not in buf:
            b = c.recv(65536)
            if not b:
                return
            buf += b
        req = buf.split(NL.encode(), 1)[0].decode("latin1")
        parts = req.split(" ")
        path = parts[1].split("?", 1)[0] if len(parts) >= 2 else "/"
        entry = CASES.get(path)
        if entry is None:
            return
        c.sendall(entry[1])
        time.sleep(0.05)
    except OSError:
        pass
    finally:
        try:
            c.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        c.close()

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0))
with open(PORTFILE, "w") as f:
    f.write(str(s.getsockname()[1]))
s.listen(128)
while True:
    conn, _ = s.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
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

    startBg(`python3 ${T}/evil.py ${T}/evil.port`, "evil");
    const P = waitPort("evil");
    ok(P > 0, "fixture: the oracle bound an ephemeral port");
    if (P === 0) {
        print("  fixture log: " + cat(`${T}/evil.log`).slice(0, 400));
        reap();
        throw new Error("test_http_client_framing: oracle never bound");
    }

    const client = new HTTPClient();
    const get = (path) => {
        try {
            const r = client.get(`http://127.0.0.1:${P}${path}`);
            return { ok: true, status: r.status, body: r.body,
                     len: r.bodyBytes ? r.bodyBytes.length : -1,
                     headers: r.headers };
        } catch (e) {
            return { ok: false, err: String(e && e.message || e) };
        }
    };
    const hdr = (headers, name) => {
        if (!headers) return undefined;
        const want = name.toLowerCase();
        for (const k of Object.keys(headers)) if (k.toLowerCase() === want) return headers[k];
        return undefined;
    };
    const refused = (label, path) => {
        const r = get(path);
        ok(!r.ok, label + " is REFUSED" + (r.ok ? " -- got status " + r.status +
                                              " body " + JSON.stringify(r.body).slice(0, 60) : ""));
        return r;
    };
    const served = (label, path, wantBody, wantStatus) => {
        const r = get(path);
        if (!r.ok) { ok(false, label + " should serve -- threw " + r.err); return null; }
        ok(r.body === wantBody, label + " body is exactly " + JSON.stringify(wantBody) +
           " -- got " + JSON.stringify(r.body).slice(0, 80));
        if (wantStatus !== undefined)
            ok(r.status === wantStatus,
               label + " status is " + wantStatus + " -- got " + r.status);
        return r;
    };

    served("H02 CL:3/6 sent", "/h1", "ABC", 200);
    served("H02 CL:0/5 sent", "/h2", "", 200);
    served("H02 CL:1 + a whole second response", "/h3", "A", 200);
    served("H02 CL:1/4096 sent", "/h4", "Z", 200);
    served("H02 CL:4/5 sent", "/h5", "ABCD", 200);
    served("H02 CL:4 + a trailing CRLF", "/h6", "ABCD", 200);
    served("H02 CL:0 + a pipelined response", "/h7", "", 200);
    served("H02 CL:1 + 64 KiB of junk", "/h17", "A", 200);
    served("H02 CL:6, 6 sent, then a second response", "/h14", "ABCDEF", 200);

    served("H02 control CL:6/6 sent", "/h8", "ABCDEF", 200);
    served("H02 control CL:3/3 sent", "/h9", "ABC", 200);
    served("H02 control CL:1/1 sent", "/h10", "A", 200);
    served("H02 control CL:0/nothing sent", "/h11", "", 200);
    served("H02 control no CL, read-to-close", "/h13", "ABCDEF", 200);
    served("H02 control chunked, the same bytes", "/h15", "ABCDEF", 200);
    served("H02 control chunked 1 byte + a 2nd response", "/h16", "A", 200);

    {
        const r = get("/h3");
        ok(r.ok && r.headers && hdr(r.headers, "content-length") === "1",
           "H02: the header region survives the cut intact [" +
           JSON.stringify(r.headers) + "]");
    }

    {
        const r = refused("H02: a body cut short of its declared length", "/h12");
        if (!r.ok) ok(/truncated/.test(r.err),
                      "H02: the truncation refusal NAMES the truncation [" + r.err + "]");
    }

    {
        const first = get("/h3");
        const second = get("/h3");
        ok(first.ok && first.body === "A",
           "reuse: the first request's body is its own declared byte");
        ok(second.ok && second.body === "A",
           "reuse: a SECOND identical request is unaffected by the first's leftover");
        const third = get("/h8");
        ok(third.ok && third.body === "ABCDEF",
           "reuse: a request after a fully-consumed one is also unaffected");
    }

    refused("M01: a bare CR in a response header value", "/m1");
    refused("M01: a bare LF in a response header value", "/m2");
    refused("M01: a CR then a space at the end of a value", "/m3");
    refused("M01: CR CR in a value", "/m4");

    {
        const r = get("/m5");
        ok(r.ok && r.ok && hdr(r.headers, "x-a") === "a" && hdr(r.headers, "x-b") === "b",
           "M01 control: two headers split on CRLF still parse [" +
           JSON.stringify(r.headers) + "]");
    }
    {
        const r = get("/m6");
        ok(r.ok && hdr(r.headers, "x-a") === "a\tb",
           "M01 control: TAB in a value is legal and survives [" +
           JSON.stringify(r.headers) + "]");
    }
    {
        const r = get("/m7");
        ok(r.ok && hdr(r.headers, "x-a") !== undefined,
           "M01 control: obs-text 0xe9 in a value is legal [" +
           JSON.stringify(r.headers) + "]");
    }
    {
        const r = get("/m8");
        ok(r.ok && hdr(r.headers, "x-a") === "",
           "M01 control: an empty value is legal [" + JSON.stringify(r.headers) + "]");
    }
    {
        const r = get("/m9");
        ok(r.ok && hdr(r.headers, "location") === "http://h:1/p?q=1",
           "M01 control: a colon inside a value is legal [" +
           JSON.stringify(r.headers) + "]");
    }

    refused("M01 control: NUL in a value", "/m10");
    refused("M01 control: VT in a value", "/m11");
    refused("M01 control: DEL in a value", "/m12");
    refused("M01 control: CR in a header name", "/m13");
    refused("M01 control: LF-only line endings", "/m14");

    refused("M03: a 4-digit 1xx-prefixed status (1999)", "/s1");
    refused("M03: a 4-digit 1xx-prefixed status (1200)", "/s2");
    refused("M03: a 4-digit 2xx-prefixed status (2000)", "/s3");
    refused("M03: a 5-digit status (99999)", "/s4");
    refused("M03: 1999 ahead of a real 200", "/s5");
    refused("M03: a 2-digit status", "/s6");
    refused("M03: a non-digit status", "/s7");
    refused("M03: 4 digits then junk", "/s8");
    refused("F4: 999 (three digits, out of range)", "/s24");

    for (const [path, want, label] of [
        ["/s16", 200, "200"],
        ["/s17", 204, "204 with no reason and no body"],
        ["/s18", 299, "299 (2xx upper edge)"],
        ["/s19", 300, "300"],
        ["/s20", 404, "404"],
        ["/s21", 499, "499"],
        ["/s22", 500, "500"],
        ["/s23", 599, "599 (5xx upper edge)"],
        ["/s24b", 598, "598 (just under the range edge)"],
        ["/s14", 99,  "099 (leading zero is still three digits)"],
        ["/s25", 200, "HTTP/1.0"],
        ["/s26", 200, "HTTP/2"],
    ]) {
        const r = get(path);
        ok(r.ok && r.status === want,
           "M03 control: " + label + " surfaces as " + want +
           " -- " + (r.ok ? "got " + r.status : "threw " + r.err));
    }

    for (const [path, label] of [
        ["/s10", "100 Continue then 200"],
        ["/s11", "199 then 200"],
        ["/s13", "100 with no reason then 200"],
    ]) {
        const r = get(path);
        ok(r.ok && r.status === 200,
           "M03 control: " + label + " still yields the final 200 -- " +
           (r.ok ? "got " + r.status : "threw " + r.err));
    }

    {
        const r = get("/s12");
        ok(r.ok && r.status === 101,
           "M03 control: 101 is final, not interim -- " +
           (r.ok ? "got " + r.status : "threw " + r.err));
    }

    {
        const r = get("/s15");
        ok(!r.ok || r.status !== 199,
           "M03 control: a bare interim 199 is never surfaced as a final status");
    }

    reap();
}

print("test_http_client_framing: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_client_framing: " + fail + " failures");
