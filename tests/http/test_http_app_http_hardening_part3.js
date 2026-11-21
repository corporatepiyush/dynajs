// flags: --std
// covers: dyna:http.App
// Part of the former tests/http/test_http_hardening.js, split so the parts run in parallel.
// peer-proof: drop evil out:^$ -- slow_headers: the client gives up at 3 s while the peer waits out its 4 s delay; stop() records that open connection, the proxy in front of the Python peer never logs it
// peer-proof: exempt v6 -- the recording proxy listens on 127.0.0.1 only, and this probe exists to reach ::1; the Python run never records it
// peer-proof: exempt v6a -- the same ::1 probe against the App
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with every Python peer taking its port or port file as an argument, so the recording proxy sits in front of it, and the v6 heredoc written to v6.py, since the shim runs python3 in the background without stdin
// slots: 2
import { spawnHelper, cleanupOnExit } from "../lib/helper.js";
import { App, HTTPClient, HTTPServer, HTTPServerAsync } from "dyna:http";
import { pid } from "dyna:sys";
import * as std from "std";
import * as os from "os";

import { suite } from "../lib/kit.js";
import { SHA256Hex } from "dyna:hash";

const t = suite("test_http_hardening_part3");
let A = null, why = null;
const ok = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why) a.skip(why); return fn(a); }, o);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_hard.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;

const startBg = (cmd, tag) => {
    sh(`rm -f ${T}/${tag}.pid`); return spawnHelper(`${cmd} >${T}/${tag}.log 2>&1`, { pidFile: `${T}/${tag}.pid` });
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
cleanupOnExit(`rm -rf ${T}`);
t.before(() => { if (!why) sh(`rm -rf ${T}; mkdir -p ${T}`); });
t.after(() => { if (!why) { reapFixtures(); sh(`rm -rf ${T}`); } });

test("client: CR or LF in header strings and invalid methods are refused before anything is sent", async () => {
    {
        const s = new HTTPServerAsync({ port: 0, routes: { "/": "ok\n" } });
        s.start();
        const c = new HTTPClient();
        try {
            const url = "http://127.0.0.1:" + s.port + "/";
            let threw = null;
            try { await c.get(url, { "X-Good": "1" }); }
            catch (e) { threw = e; }
            ok(!threw, "object headers are still accepted (control)");
            ok((await c.get(url, "X-Good: 1\r\n")).status === 200,
               "a clean pre-formatted header string is still accepted (control)");

            threw = null;
            try { await c.get(url, "X-Evil: a\r\nEvil: 1"); }
            catch (e) { threw = e; }
            ok(threw && /CR or LF/.test(threw.message),
               "a CR/LF in a pre-formatted header string is refused (CWE-93), got: " +
               (threw ? threw.message.slice(0, 60) : "no throw"));

            threw = null;
            try { await c.request("GET\r\nX-Evil: 1", url); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "a method carrying CRLF is refused (request splitting), got: " +
               (threw ? threw.message.slice(0, 60) : "no throw"));

            threw = null;
            try { await c.request("GET X", url); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "a method with a space is refused (tchar rule)");

            threw = null;
            try { await c.request("", url); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "an empty method is refused");

            threw = null;
            try { await c.request("POST\r\n", url, "x"); }
            catch (e) { threw = e; }
            ok(threw && /invalid HTTP method/.test(threw.message),
               "requestAsync validates the method too");
        } finally { c.close(); s.close(); }
    }
});

test("client: methods, headers, binary bodies, queries and fragments reach the wire verbatim", async () => {
    {
        write(`${T}/capture.js`, `import { listen, latin1, cat } from ${KIT};
import { SHA256Hex } from "dyna:hash";
import * as std from "std";
listen(async (c) => {
    const h = await c.readUntil("\\r\\n\\r\\n");
    let d = h.bytes;
    if (h.found) {
        let need = 0;
        for (const hl of latin1(h.bytes).split("\\r\\n")) if (hl.toLowerCase().startsWith("content-length:")) need = parseInt(hl.split(":")[1].trim() || "0", 10);

        if (need > 0) {
            const b = await c.readBytes(need), body = b.bytes.subarray(0, need);
            d = cat(d, body);
            const f = std.open("${T}/cap.bodies", "a"); f.puts(body.length + " " + SHA256Hex(body) + "\\n"); f.close();
        }
    }
    const raw = latin1(d).replace(/\\r\\n/g, "\\n");
    await c.write(new TextEncoder().encode("HTTP/1.1 200 OK\\r\\nContent-Length: " + raw.length + "\\r\\n\\r\\n" + raw));
    c.close();
}, { portFile: scriptArgs[1] });
`);
        startBg(`${EXE} --std ${T}/capture.js ${T}/cap.port`, "cap");
        const CAP = waitPort("cap");
        const c = new HTTPClient();
        try {
            const r = await c.request("PATCH", `http://127.0.0.1:${CAP}/x`);
            ok(r.status === 200 && r.body.indexOf("PATCH /x") === 0,
               "a valid custom method (PATCH) reaches the wire verbatim (got '" +
               r.body.slice(0, 30) + "')");
        } catch (e) {
            ok(false, "PATCH request threw: " + e.message.slice(0, 60));
        }
        const r2 = await c.get(`http://127.0.0.1:${CAP}/w`, { "X-One": "1", "X-Two": "b" });
        ok(r2.body.indexOf("X-One: 1") >= 0 && r2.body.indexOf("X-Two: b") >= 0,
           "client: object headers arrive on the wire verbatim (got '" +
           r2.body.replace(/\n/g, "|").slice(0, 90) + "')");

        const bytes = new Uint8Array(257);
        for (let i = 0; i < 257; i++) bytes[i] = i & 0xff;
        const r3 = await c.post(`http://127.0.0.1:${CAP}/bin`, bytes);
        ok(r3.body.indexOf("Content-Length: 257") >= 0,
           "client: a 257-byte binary body declares CL 257 (got '" +
           r3.body.slice(-60).replace(/\n/g, "|") + "')");

        const capB = cat(`${T}/cap.bodies`);
        ok(capB.indexOf("257 " + SHA256Hex(bytes)) >= 0,
           "client: the peer received the exact 257-byte binary body, by length and sha256 (got '" + capB.trim().slice(0, 80) + "')");

        const r4 = await c.get(`http://127.0.0.1:${CAP}/q?a=1&b=two`);
        ok(r4.body.indexOf("GET /q?a=1&b=two ") === 0,
           "client: query strings are preserved verbatim");
        const r5 = await c.get(`http://127.0.0.1:${CAP}/p#frag`);
        ok(r5.body.indexOf("GET /p ") === 0,
           "client: fragments are stripped from the request-target, never " +
           "sent on the wire (dynajs.d.ts: a request-url fragment is stripped " +
           "before anything else)");

        let arrThrew = null;
        try { await c.get(`http://127.0.0.1:${CAP}/x`, ["X-Evil: 1"]); }
        catch (e) { arrThrew = e; }
        ok(arrThrew && /not an array/.test(arrThrew.message),
           "client: an Array as headers is refused (numeric-named junk), got: " +
           (arrThrew ? arrThrew.message.slice(0, 50) : "no throw"));

        c.close();
        sh(`kill $(cat ${T}/cap.pid) 2>/dev/null`);
    }
});

test("server: out-of-range statuses and control bytes in route content types and paths are refused", async () => {
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
            const r = await c.get("http://127.0.0.1:" + s.port + "/");
            good = r.status === 404 &&
                   (r.headers["Content-Type"] === "application/xml" ||
                    r.headers["content-type"] === "application/xml");
            c.close(); s.close();
        } catch (e) { good = false; }
        ok(good, "control: a valid non-200 route with a real content type serves");
    }
});

test("client: malformed status lines and framing from a hostile server are refused; legal edge cases parse", async () => {
    {
        const runEvil = (mode, resp, delay) => {
            write(`${T}/evil.js`, `import { listen } from ${KIT};
const DELAY_MS = Number(scriptArgs[1] || 0) * 1000;
const RESP = "${resp}";
listen(async (c) => {
    await c.read();
    if (DELAY_MS) await new Promise((r) => setTimeout(r, DELAY_MS));
    await c.write(RESP);
    c.close();
}, { portFile: scriptArgs[2] });
`);
            const evilH = startBg(`${EXE} --std ${T}/evil.js ${delay} ${T}/evil.${mode}.port`, `evil.${mode}`);
            const EP = waitPort(`evil.${mode}`);
            write(`${T}/cli.js`, `import { HTTPClient } from "dyna:http";
import * as std from "std";
const c = new HTTPClient();
c.setTimeout(3000);
let out = "";
try { const r = await c.get("http://127.0.0.1:${EP}/");
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
            evilH.stop();
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
});

test("client: caller Content-Length, Host and Transfer-Encoding never duplicate the client's own framing", async () => {
    {
        write(`${T}/capture2.js`, `import { listen, latin1, cat } from ${KIT};
listen(async (c) => {
    const h = await c.readUntil("\\r\\n\\r\\n");
    if (!h.found) { c.close(); return; }
    let need = 0;
    for (const hl of latin1(h.bytes).split("\\r\\n")) if (hl.toLowerCase().startsWith("content-length:")) { const v = (hl.split(":")[1] || "").trim(); need = /^[0-9]+$/.test(v) ? parseInt(v, 10) : 0; }
    let d = h.bytes;
    if (need > 0) { const b = await c.readBytes(need); d = cat(d, b.bytes.subarray(0, need)); }
    const raw = latin1(d);
    await c.write(new TextEncoder().encode("HTTP/1.1 200 OK\\r\\nContent-Length: " + raw.length + "\\r\\n\\r\\n" + raw));
    c.close();
}, { portFile: scriptArgs[1] });
`);
        startBg(`${EXE} --std ${T}/capture2.js ${T}/cap2.port`, "cap2");
        const CAP2 = waitPort("cap2");
        const cc = new HTTPClient();
        const curl2 = `http://127.0.0.1:${CAP2}/`;
        const rD = await cc.post(curl2, "abc", { "Content-Length": "3", "X-Keep": "1" });
        const dupCL = (rD.body.match(/Content-Length:/gi) || []).length;
        ok(rD.status === 200 && dupCL === 1 && rD.body.indexOf("X-Keep: 1") >= 0,
           "wire: caller Content-Length is dropped; ours is the only framing (got " +
           dupCL + " CL lines)");
        const rH = await cc.get(curl2, { "Host": "evil.example" });
        ok(rH.status === 200 &&
           (rH.body.match(/Host:/gi) || []).length === 1 &&
           rH.body.indexOf("evil.example") < 0,
           "wire: caller Host is dropped; ours is the only one");
        const rT = await cc.post(curl2, "abc", { "Transfer-Encoding": "chunked" });
        ok(rT.status === 200 && rT.body.indexOf("Transfer-Encoding:") < 0 &&
           (rT.body.match(/Content-Length:/gi) || []).length === 1,
           "wire: caller Transfer-Encoding is dropped; framing stays by length");
        const rN = await cc.get(curl2, "X-Foo: bar");
        ok(rN.status === 200 && /X-Foo: bar\r\nConnection: (close|keep-alive)\r\n/.test(rN.body),
           "wire: a string header block without a trailing CRLF is terminated, " +
           "not glued onto Connection (got '" +
           rN.body.replace(/\r\n/g, "|").slice(-40) + "')");
        cc.close();
        sh(`kill $(cat ${T}/cap2.pid) 2>/dev/null`);
    }
});

test("App: malformed request targets, Host lines and header bytes are 400 and the proxy never forwards them", async () => {
    {
        write(`${T}/up2.js`, `import { listen, enc, cat } from ${KIT};
import * as std from "std";
listen(async (c) => {
    const h = await c.readUntil("\\r\\n\\r\\n", { timeoutMs: 2000 });
    if (h.timedOut) { c.close(); return; }
    const d = h.bytes;
    const rec = cat(enc("====\\n"), d);
    const f = std.open("${T}/up2.last.bin", "ab"); f.write(rec.buffer, rec.byteOffset, rec.length); f.close();
    await c.write("HTTP/1.1 200 OK\\r\\nContent-Length: 8\\r\\nConnection: close\\r\\n\\r\\nCAPTURED");
    c.close();
}, { portFile: scriptArgs[1] });
`);
        write(`${T}/app2.js`, `import { App } from "dyna:http";
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
        const up2H = startBg(`${EXE} --std ${T}/up2.js ${T}/up2.port`, "up2");
        const UP2 = waitPort("up2");
        startBg(`${EXE} --std ${T}/app2.js ${UP2}`, "app2sh");
        const AP2 = waitPort("app2");
        sh(`rm -f ${T}/up2.last.bin`);
        write(`${T}/atk2.js`, `import { connect, latin1, cat } from ${KIT};
const P = Number(scriptArgs[1]);
const R = [];
async function hit(req, tag, timeout = 4.0) {
    let s;
    try { s = await connect(P, "127.0.0.1", { connectTimeoutMs: timeout * 1000 }); }
    catch (e) { R.push({ tag, code: null, err: String(e.message).slice(0, 50) }); return; }
    await s.write(req);
    let buf = new Uint8Array(0);
    for (;;) { const r = await s.read({ timeoutMs: timeout * 1000 }); if (r.timedOut || !r.bytes) break; buf = cat(buf, r.bytes); }
    s.abort();
    const b = latin1(buf);
    R.push({ tag, code: b.startsWith("HTTP") ? b.slice(9, 12) : null });
}
const BODY = '{"method":"ping","id":1}';
await hit("GET /rp\\nX-Evil: 1 HTTP/1.1\\r\\nHost: h\\r\\n\\r\\n", "lf_target");
await hit("GET /rp\\x01x HTTP/1.1\\r\\nHost: h\\r\\n\\r\\n", "ctl_target");
await hit("POST /rpc HTTP/1.1\\r\\nContent-Length: " + BODY.length + "\\r\\n\\r\\n" + BODY, "no_host");
await hit("POST /rpc HTTP/1.1\\r\\nHost: a\\r\\nHost: b\\r\\nContent-Length: " + BODY.length + "\\r\\n\\r\\n" + BODY, "dup_host");
await hit("POST /rpc HTTP/1.1\\r\\nHost: h\\r\\nX-Nul: a\\x00b\\r\\nContent-Length: " + BODY.length + "\\r\\n\\r\\n" + BODY, "ctl_value");
await hit("POST /rpc HTTP/1.1\\r\\nHost: h\\r\\nContent-Length: " + BODY.length + "\\r\\n\\r\\n" + BODY, "control_ok");
await hit("GET /p/lf\\nX: 1 HTTP/1.1\\r\\nHost: h\\r\\nContent-Length: 0\\r\\n\\r\\n", "proxy_lf");
await hit("GET /p/clean HTTP/1.1\\r\\nHost: h\\r\\n\\r\\n", "proxy_clean");
print(JSON.stringify(R));
`);
        sh(`${EXE} --std ${T}/atk2.js ${AP2} > ${T}/atk2.json 2>${T}/atk2.err`);
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
            write(`${T}/wsprobe.js`, `import { connect, cat } from ${KIT};
import * as std from "std";
const s = await connect(Number(scriptArgs[1]), "127.0.0.1", { connectTimeoutMs: 3000 });
await s.write("GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\nSec-WebSocket-Key: ${WSKEY}\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n");
await s.readUntil("\\r\\n\\r\\n", { timeoutMs: 3000 });
const payload = [0xff, 0xfe, 0xfe, 0x62, 0x61, 0x64], mask = [1, 2, 3, 4];
const frame = new Uint8Array([0x81, 0x80 | payload.length, ...mask, ...payload.map((b, i) => b ^ mask[i & 3])]);
let out = new Uint8Array(0);
if (await s.write(frame)) for (;;) { const r = await s.read({ timeoutMs: 3000 }); if (r.timedOut || !r.bytes) break; out = cat(out, r.bytes); }
s.abort();
const f = std.open("${T}/ws.out", "w"); f.puts(out.toHex()); f.close();
`);
            sh(`${EXE} --std ${T}/wsprobe.js ${AP2} >${T}/ws.log 2>&1`);
            const wsRaw = cat(`${T}/ws.out`);
            ok(wsRaw.indexOf("880203ef") >= 0,
               "W: an invalid-UTF-8 text frame draws a close frame with code 1007 (got '" +
               wsRaw.slice(0, 40) + "')");
        }
        sh(`pkill -f '${T}/app2.js' 2>/dev/null`); up2H.stop();
    }
});

test("dual stack: host \"::\" serves both ::1 and v4-mapped 127.0.0.1 for HTTPServerAsync and App", async () => {
    {
        const s = new HTTPServerAsync({ port: 0, host: "::",
                                        routes: { "/": "dual\n" } });
        s.start();
        sh(`rm -f ${T}/v6.out`);
        write(`${T}/v6.js`, `import { connect, latin1 } from ${KIT};
const P = Number(scriptArgs[1]), R = {};
const APP = "0" === "1";
const body = '{"method":"ok","id":1}';
const req = APP ? "POST /h HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: " + body.length + "\\r\\nConnection: close\\r\\n\\r\\n" + body : "GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n";
const want = APP ? "dual-app" : "dual";
for (const [tag, host] of [["v6", "::1"], ["v4map", "127.0.0.1"]]) {
    try {
        const sk = await connect(P, host, { connectTimeoutMs: 2000 });
        await sk.write(req);
        const r = await sk.readAll({ timeoutMs: 3000 });
        sk.abort();
        const out = latin1(r.bytes);
        R[tag] = out.includes(want) ? "ok" : "badbody:" + JSON.stringify(out.slice(0, 48));
    } catch (e) { R[tag] = "err:" + String(e.message).slice(0, 40); }
}
print(JSON.stringify(R));
`);
        sh(`${EXE} --std ${T}/v6.js ${s.port} > ${T}/v6.out 2>&1`);
        let V = {};
        try { V = JSON.parse(cat(`${T}/v6.out`).trim()); }
        catch (e) { ok(false, "v6 probe failed: " + cat(`${T}/v6.out`).slice(0, 60)); }
        ok(V.v6 === "ok", "⑤: host '::' serves ::1 natively (got " + JSON.stringify(V.v6) + ")");
        ok(V.v4map === "ok", "⑤: the same socket serves 127.0.0.1 v4-mapped (got " +
           JSON.stringify(V.v4map) + ")");
        s.close();

        write(`${T}/app6.js`, `import { App } from "dyna:http";
import { pid } from "dyna:sys";
import * as std from "std";
const f = std.open("${T}/app6.pid", "w"); f.puts(String(pid())); f.close();
const app = new App({ port: 0, host: "::" });
app.rpc("/h", { ok: () => "dual-app" });
app.start();
const g = std.open("${T}/app6.port", "w"); g.puts(String(app.port)); g.close();
`);
        spawnHelper(`${EXE} --std ${T}/app6.js >${T}/app6.log 2>&1`);
        let app6Port = 0;
        for (let i = 0; i < 100 && !(app6Port > 0); i++) {
            app6Port = parseInt(cat(`${T}/app6.port`).trim(), 10) || 0;
            if (!(app6Port > 0)) sh("sleep 0.05");
        }
        ok(app6Port > 0, "App v6 child started and bound");
        write(`${T}/v6a.js`, `import { connect, latin1 } from ${KIT};
const P = Number(scriptArgs[1]), R = {};
const APP = "1" === "1";
const body = '{"method":"ok","id":1}';
const req = APP ? "POST /h HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: " + body.length + "\\r\\nConnection: close\\r\\n\\r\\n" + body : "GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n";
const want = APP ? "dual-app" : "dual";
for (const [tag, host] of [["v6", "::1"], ["v4map", "127.0.0.1"]]) {
    try {
        const sk = await connect(P, host, { connectTimeoutMs: 2000 });
        await sk.write(req);
        const r = await sk.readAll({ timeoutMs: 3000 });
        sk.abort();
        const out = latin1(r.bytes);
        R[tag] = out.includes(want) ? "ok" : "badbody:" + JSON.stringify(out.slice(0, 48));
    } catch (e) { R[tag] = "err:" + String(e.message).slice(0, 40); }
}
print(JSON.stringify(R));
`);
        sh(`rm -f ${T}/v6a.out`);
        sh(`${EXE} --std ${T}/v6a.js ${app6Port} > ${T}/v6a.out 2>&1`);
        let VA = {};
        try { VA = JSON.parse(cat(`${T}/v6a.out`).trim()); }
        catch (e) { ok(false, "App v6 probe failed: " + cat(`${T}/v6a.out`).slice(0, 60)); }
        ok(VA.v6 === "ok", "⑤: App host '::' serves ::1 through dyn_aio_listen (got " +
           JSON.stringify(VA.v6) + ")");
        ok(VA.v4map === "ok", "⑤: App dual-stack serves 127.0.0.1 too (got " +
           JSON.stringify(VA.v4map) + ")");
        sh(`pkill -f '${T}/app6.js' 2>/dev/null`);
    }
});


await t.run();
