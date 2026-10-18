// flags: --std
// timeout: 300
import { App, HTTPClient, HTTPServer } from "dyna:net";
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
const CH = (...cs) => String.fromCharCode(...cs);
const CR = CH(13), LF = CH(10), NUL = CH(0), DEL = CH(0x7f);

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_hdr_injection: SKIP (python3 not available)");
} else {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_inj.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/srv.js`, `
import { App } from "dyna:net";
import * as dstream from "dyna:stream";
import * as std from "std";

/* The App binds port 0, so the port it got is the only way in. Announced by
 * WRITING A FILE, not by printing: this process outlives the harness, and a
 * killed process never flushes stdout, so a print signal never arrives. */
const PORTFILE = scriptArgs[1];

const CR = String.fromCharCode(13), LF = String.fromCharCode(10);
const rep = (c, n) => { let s = ""; for (let i = 0; i < n; i++) s += c; return s; };

const app = new App({ port: 0 });

/* DOOR 1, dynamic route, contentType straight from the query string. The
   handler percent-decodes it, so %0d%0a arrives as a real CRLF. */
app.get("/ctq", req => ({
    status: 200,
    contentType: (req.query && req.query.ct) !== undefined ? req.query.ct : "text/plain",
    body: "hi",
}));

/* DOOR 1 again, handler side: the control-byte payloads are BUILT here and
 * handed to the door as a JS string, so what it is asked to validate is what
 * the test means. A NUL must reach the validator; a decoder may filter it. */
const CTL = { NUL: 0, SOH: 1, BEL: 7, BS: 8, TAB: 9, LF: 10, VT: 11, FF: 12,
              CR: 13, SO: 14, SI: 15, DLE: 16, ESC: 27, US: 31, DEL: 127 };
app.get("/ctl", req => ({
    status: 200,
    contentType: "text/plain" + String.fromCharCode(CTL[req.query.name]) + "X: y",
    body: "hi",
}));

/* The two CRLF shapes, handler side. */
app.get("/crlf", req => ({
    status: 200,
    contentType: "text/plain" + CR + LF + "Set-Cookie: admin=1",
    body: "hi",
}));
app.get("/dblcrlf", req => ({
    status: 200,
    contentType: "text/plain" + CR + LF + CR + LF + "INJECTEDBODY",
    body: "hi",
}));

/* DOOR 2, the stream result, reached through the JSON-RPC path: the same
   handler-controlled contentType on a chunked response. */
app.rpc("/strm", {
    strm: p => ({
        stream: dstream.fromBytes("hi"),
        contentType: (p && p.ct) !== undefined ? p.ct : "application/octet-stream",
    }),
});

/* Fixed list for the length and legal-form cases, so the payload never has
   to survive a query round trip. */
const FIXED = [
    "text/plain",                                  // 0 plain
    "",                                            // 1 empty
    "text/plain; charset=utf-8",                   // 2 charset parameter
    "text/html; charset=ISO-8859-1",               // 3 quoted charset
    "caf" + String.fromCharCode(0xe9) + "/plain",   // 4 obs-text, legal
    rep("x", 255),                                 // 5 last legal size
    rep("x", 256),                                 // 6 exactly the cap
    rep("x", 257),                                 // 7 one over the cap
    rep("x", 500),                                 // 8 the profiler's case
];
app.get("/ctfixed", req => ({
    status: 200,
    contentType: FIXED[+req.query.n],
    body: "hi",
}));

/* Defaults: a bare return, an envelope with a string body (documented to
   default to text/plain), and one with a body that must be JSON-encoded
   (which is where the application/json default applies). */
app.get("/plain", req => "hi");
app.get("/env", req => ({ body: "hi" }));
app.get("/envobj", req => ({ body: { a: 1 } }));

app.start();
{ const f = std.open(PORTFILE, "w"); f.puts(String(app.port)); f.close(); }
`);

    write(`${T}/raw.py`, `
import socket, sys
NL = chr(13) + chr(10)
port = int(sys.argv[1])
path = sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), 5.0)
s.settimeout(5.0)
req = "GET %s HTTP/1.1" % path + NL + "Host: x" + NL + "Connection: close" + NL + NL
s.sendall(req.encode())
buf = b""
try:
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
except socket.timeout:
    buf += b"<<TIMEOUT>>"
s.close()
sys.stdout.buffer.write(buf)
`);

    write(`${T}/rawrpc.py`, `
import socket, sys
NL = chr(13) + chr(10)
port = int(sys.argv[1])
case = sys.argv[2]

# JSON ESCAPES, so the parser hands the handler a real control byte.
PRE = "application/octet-stream"
CT = {
    "crlf":      '"' + PRE + chr(92) + "r" + chr(92) + "nSet-Cookie: s=1" + '"',
    "lf":        '"' + PRE + chr(92) + "nSet-Cookie: s=1" + '"',
    "nul":       '"' + PRE + chr(92) + "u0000Set-Cookie: s=1" + '"',
    "del":       '"' + PRE + chr(92) + "u007fSet-Cookie: s=1" + '"',
    "long500":   '"' + ("x" * 500) + '"',
    "legal":     '"text/plain; charset=utf-8"',
}
body = ('{"jsonrpc":"2.0","method":"strm","params":{"ct":' + CT[case] + '},"id":1}').encode()
s = socket.create_connection(("127.0.0.1", port), 5.0)
s.settimeout(5.0)
req = ("POST /strm HTTP/1.1" + NL + "Host: x" + NL +
       "Content-Type: application/json" + NL +
       "Content-Length: " + str(len(body)) + NL +
       "Connection: close" + NL + NL).encode()
s.sendall(req + body)
buf = b""
try:
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
except socket.timeout:
    buf += b"<<TIMEOUT>>"
s.close()
sys.stdout.buffer.write(buf)
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

    startBg(`${EXE} --std ${T}/srv.js ${T}/app.port`, "app");
    const P = waitPort("app");
    ok(P > 0, "fixture: the App bound an ephemeral port");
    if (P === 0) {
        print("  fixture log: " + cat(`${T}/app.log`).slice(0, 400));
        reap();
        throw new Error("test_http_hdr_injection: fixture never bound");
    }

    const rawGet = (path) => {
        sh(`python3 ${T}/raw.py ${P} '${path}' > ${T}/raw.out 2>${T}/raw.err`);
        return cat(`${T}/raw.out`);
    };
    const rawRpc = (kase) => {
        sh(`python3 ${T}/rawrpc.py ${P} ${kase} > ${T}/raw.out 2>${T}/raw.err`);
        return cat(`${T}/raw.out`);
    };

    const headOf = (r) => { const i = r.indexOf(CR + LF + CR + LF); return i < 0 ? r : r.slice(0, i); };
    const bodyOf = (r) => { const i = r.indexOf(CR + LF + CR + LF); return i < 0 ? "" : r.slice(i + 4); };
    const statusOf = (r) => (r.split(CR + LF)[0] || "");
    const lines = (r) => headOf(r).split(CR + LF);
    const hasHeader = (r, name) =>
        lines(r).some(l => l.toLowerCase().startsWith(name.toLowerCase() + ":"));
    const ctLine = (r) => lines(r).find(l => /^content-type:/i.test(l)) || "";
    const ctValue = (r) => {
        const l = ctLine(r);
        return /^content-type:/i.test(l) ? l.slice(l.indexOf(":") + 1).trim() : null;
    };
    const is500 = (r) => /^HTTP\/1\.1 500 /.test(statusOf(r));
    const pct = (s) => Array.from(s).map(ch => {
        const c = ch.charCodeAt(0);
        if (c < 0x21 || c > 0x7e) return "%" + c.toString(16).toUpperCase().padStart(2, "0");
        return ch === "%" ? "%25" : ch === " " ? "%20" : ch === "=" ? "%3D"
             : ch === ";" ? "%3B" : ch === ":" ? "%3A" : ch === "/" ? "%2F" : ch;
    }).join("");

    let r = rawGet("/crlf");
    ok(!hasHeader(r, "Set-Cookie"),
       "H01: a handler-side CRLF injects no Set-Cookie [" + headOf(r) + "]");
    ok(is500(r), "H01: a handler-side CRLF is a clean 500 [" + statusOf(r) + "]");
    ok(/contentType must not contain control/.test(bodyOf(r)),
       "H01: the refusal NAMES the reason [" + bodyOf(r).slice(0, 90) + "]");
    ok(lines(r).length === 4 && hasHeader(r, "Content-Length") && hasHeader(r, "Connection"),
       "H01: the 500 head is itself well formed [" + headOf(r) + "]");

    r = rawGet("/ctq?ct=" + pct("text/plain" + CR + LF + "Set-Cookie: admin=1"));
    ok(!hasHeader(r, "Set-Cookie"),
       "H01: a percent-encoded CRLF injects no Set-Cookie [" + headOf(r) + "]");
    ok(headOf(r).indexOf("admin=1") < 0,
       "H01: the injected text appears nowhere in the head [" + headOf(r) + "]");
    ok(is500(r), "H01: a percent-encoded CRLF is a clean 500 [" + statusOf(r) + "]");
    ok(/contentType must not contain control/.test(bodyOf(r)),
       "H01: the query-path refusal names the reason [" + bodyOf(r).slice(0, 90) + "]");

    r = rawGet("/dblcrlf");
    ok(headOf(r).indexOf("INJECTEDBODY") < 0 && bodyOf(r).indexOf("INJECTEDBODY") < 0,
       "H01: a handler-side double CRLF gives the attacker neither head nor body [" +
       headOf(r) + "]");
    ok(is500(r), "H01: a handler-side double CRLF is a clean 500 [" + statusOf(r) + "]");
    r = rawGet("/ctq?ct=" + pct("text/plain" + CR + LF + CR + LF + "INJECTEDBODY"));
    ok(headOf(r).indexOf("INJECTEDBODY") < 0 && bodyOf(r).indexOf("INJECTEDBODY") < 0,
       "H01: a percent-encoded double CRLF injects no body [" + bodyOf(r).slice(0, 90) + "]");
    ok(is500(r), "H01: a percent-encoded double CRLF is a clean 500 [" + statusOf(r) + "]");

    const CTL_NAMES = ["NUL", "SOH", "BEL", "BS", "TAB", "LF", "VT", "FF", "CR",
                       "SO", "SI", "DLE", "ESC", "US", "DEL"];
    const CTL_CODE = { NUL: 0x00, SOH: 0x01, BEL: 0x07, BS: 0x08, TAB: 0x09, LF: 0x0a,
                       VT: 0x0b, FF: 0x0c, CR: 0x0d, SO: 0x0e, SI: 0x0f, DLE: 0x10,
                       ESC: 0x1b, US: 0x1f, DEL: 0x7f };
    for (const name of CTL_NAMES) {
        const ch = CH(CTL_CODE[name]);
        const rr = rawGet("/ctl?name=" + name);
        ok(ctLine(rr).indexOf(ch) < 0,
           "H01: " + name + " (0x" + CTL_CODE[name].toString(16).padStart(2, "0") +
           ") never reaches the Content-Type line [" + ctLine(rr) + "]");
        ok(is500(rr), "H01: " + name + " is refused with a 500 [" + statusOf(rr) + "]");
        ok(ctValue(rr) === "application/json",
           "H01: " + name + " serves the error's own type, not a garbled echo [" +
           ctValue(rr) + "]");
        ok(bodyOf(rr) === "hi" || /contentType must not contain control/.test(bodyOf(rr)),
           "H01: " + name + " sends either the body or a named refusal, never a splice [" +
           bodyOf(rr).slice(0, 60) + "]");
    }

    r = rawRpc("crlf");
    ok(!hasHeader(r, "Set-Cookie"),
       "H01: the STREAM door injects no Set-Cookie [" + headOf(r) + "]");
    ok(is500(r), "H01: the STREAM door refuses a CRLF contentType with a 500 [" + statusOf(r) + "]");
    ok(/stream contentType must not contain control/.test(bodyOf(r)),
       "H01: the stream refusal NAMES the reason [" + bodyOf(r).slice(0, 90) + "]");
    ok(bodyOf(r).indexOf("2" + CR + LF + "hi" + CR + LF) < 0,
       "H01: the stream refusal sends no chunked body [" + bodyOf(r).slice(0, 60) + "]");

    r = rawRpc("lf");
    ok(!hasHeader(r, "Set-Cookie") && is500(r),
       "H01: the STREAM door refuses a bare LF too [" + headOf(r) + "]");

    r = rawRpc("nul");
    ok(!hasHeader(r, "Set-Cookie") && is500(r),
       "H01: the STREAM door refuses a NUL too [" + headOf(r) + "]");

    r = rawRpc("del");
    ok(!hasHeader(r, "Set-Cookie") && is500(r),
       "H01: the STREAM door refuses DEL too [" + headOf(r) + "]");

    r = rawRpc("long500");
    ok(is500(r) && /too long/.test(bodyOf(r)),
       "H01: an over-length STREAM contentType is a clean, named 500, not an " +
       "empty response [" + statusOf(r) + " " + bodyOf(r).slice(0, 60) + "]");

    r = rawRpc("legal");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && /text\/plain; charset=utf-8/.test(headOf(r)),
       "H01 control: a legal STREAM contentType still streams [" + statusOf(r) + " " +
       headOf(r).slice(0, 80) + "]");

    const staticThrows = (ct) => {
        try {
            new HTTPServer({ port: 0, routes: { "/x": { contentType: ct, body: "b" } } });
            return "";
        } catch (e) { return String(e && e.message || e); }
    };
    for (const [label, bad] of [
        ["CRLF", "text/plain" + CR + LF + "Set-Cookie: admin=1"],
        ["bare CR", "text/plain" + CR + "Set-Cookie: admin=1"],
        ["bare LF", "text/plain" + LF + "Set-Cookie: admin=1"],
        ["NUL", "text/plain" + NUL],
        ["DEL", "text/plain" + DEL],
    ]) {
        ok(/control characters/.test(staticThrows(bad)),
           "H01 agreement: the STATIC door refuses " + label + " at registration");
    }

    for (const [label, bad, url] of [
        ["CRLF", "text/plain" + CR + LF + "Set-Cookie: admin=1", null],
        ["bare CR", "text/plain" + CR + "Set-Cookie: admin=1", null],
        ["bare LF", "text/plain" + LF + "Set-Cookie: admin=1", null],
        ["NUL", "", "/ctl?name=NUL"],
        ["DEL", "", "/ctl?name=DEL"],
    ]) {
        const rr = rawGet(url || ("/ctq?ct=" + pct(bad)));
        ok(is500(rr) && !hasHeader(rr, "Set-Cookie"),
           "H01 agreement: the DYNAMIC door refuses " + label +
           " like the static door [" + statusOf(rr) + "]");
    }

    r = rawGet("/ctq?ct=" + pct("text/plain" + NUL + "Set-Cookie: admin=1"));
    ok(!hasHeader(r, "Set-Cookie") && headOf(r).indexOf("admin=1") < 0,
       "H01: a percent-encoded NUL injects nothing on the query path [" + headOf(r) + "]");
    ok(!lines(r).some(l => l.indexOf(NUL) >= 0),
       "H01: a percent-encoded NUL puts no NUL in the head [" + headOf(r) + "]");

    const x = (n) => new Array(n + 1).join("x");
    r = rawGet("/ctfixed?n=5");
    ok(ctValue(r) === x(255),
       "H01: a 255-byte contentType is served whole (len " + (ctValue(r) || "").length + ")");
    r = rawGet("/ctfixed?n=6");
    ok(ctValue(r) === x(256),
       "H01: a 256-byte contentType is served whole (len " + (ctValue(r) || "").length + ")");
    r = rawGet("/ctfixed?n=7");
    ok(ctValue(r) === null || ctValue(r).length < 257,
       "H01: a 257-byte contentType is NOT served truncated (len " + (ctValue(r) || "").length + ")");
    ok(is500(r) && /too long/.test(bodyOf(r)),
       "H01: a 257-byte contentType is a clean, named 500 [" + statusOf(r) + " " +
       bodyOf(r).slice(0, 60) + "]");
    r = rawGet("/ctfixed?n=8");
    ok(ctValue(r) === null || ctValue(r).length < 500,
       "H01: a 500-byte contentType is NOT served truncated to 127 (len " +
       (ctValue(r) || "").length + ")");
    ok(is500(r) && /too long/.test(bodyOf(r)),
       "H01: a 500-byte contentType is a clean, named 500");

    ok(/at most 256/.test(staticThrows(x(257))),
       "H01 agreement: the STATIC door also refuses over-length contentType");
    ok(staticThrows(x(256)) === "",
       "H01 agreement: the STATIC door still accepts exactly 256 bytes");

    r = rawGet("/ctq?ct=text/plain");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && ctValue(r) === "text/plain" && bodyOf(r) === "hi",
       "H01 control: text/plain still serves 200 with the right type and body");

    r = rawGet("/ctq?ct=" + pct("text/plain; charset=utf-8"));
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && ctValue(r) === "text/plain; charset=utf-8",
       "H01 control: a charset parameter still serves intact [" + ctValue(r) + "]");

    r = rawGet("/ctq?ct=");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && ctValue(r) === "",
       "H01 control: an EMPTY contentType still serves (it is not an attack)");

    r = rawGet("/ctfixed?n=3");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && ctValue(r) === "text/html; charset=ISO-8859-1",
       "H01 control: a quoted-charset media type survives [" + ctValue(r) + "]");

    r = rawGet("/ctfixed?n=4");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)),
       "H01 control: an obs-text (high-byte) contentType still serves 200");

    r = rawGet("/plain");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && bodyOf(r) === "hi",
       "H01 control: a handler with no contentType at all still serves");
    r = rawGet("/env");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && ctValue(r) === "text/plain",
       "H01 control: a string body with no contentType keeps the text/plain default [" +
       ctValue(r) + "]");
    r = rawGet("/envobj");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && ctValue(r) === "application/json",
       "H01 control: a JSON-encoded body with no contentType defaults to application/json [" +
       ctValue(r) + "]");
    r = rawGet("/ctfixed?n=1");
    ok(/^HTTP\/1\.1 200 /.test(statusOf(r)) && ctValue(r) === "",
       "H01 control: an explicit empty contentType is served as given");

    try {
        const c = new HTTPClient();
        const resp = c.get(`http://127.0.0.1:${P}/ctq?ct=text%2Fplain`);
        ok(resp.status === 200 && resp.body === "hi",
           "H01 control: the engine's own client still gets a clean 200");
    } catch (e) {
        ok(false, "H01 control: the engine's own client threw [" + String(e && e.message || e) + "]");
    }

    reap();
}

print("test_http_hdr_injection: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_hdr_injection: " + fail + " failures");
