// flags: --std
/* test_http_stream.js -- (HTTPClient.getStream) + (an App route
 * handler may return a dyna:stream ByteSource).
 *
 * The client half: getStream(url) performs ONE exchange (a 3xx is the
 * response it is, exactly like get()) and returns the response head and
 * the body in one object whose read()/close() ARE dyna:stream's ByteSource
 * shape -- dyna:stream's pipe/lines consume it directly, no import. The
 * client's max_body is enforced ON the stream (a declared Content-Length
 * over the cap refuses at open; received bytes crossing the cap reject the
 * read), and a connection ending before a declared Content-Length is a
 * named TRUNCATION -- the read rejects, never a silent 0.
 *
 * The server half: an rpc handler returning a ByteSource (or
 * {stream, contentType, status}) opts that response out of the JSON-RPC
 * envelope -- the App answers chunked and pumps the source. Pinned here:
 * the framing header, contentType/status passthrough, the source's
 * close() firing at the end of body, and an out-of-contract read (a count
 * outside [0, buf.length]) or a mid-stream throw truncating the connection.
 *
 * The framing rule those cuts ride on is pinned from both ends: a chunked
 * body that ends before its terminal chunk (EOF at the head, mid
 * chunk-size line, mid chunk, between chunks, inside the trailer section)
 * is REFUSED by the buffered client AND by getStream, with the truncation
 * named -- while a legal body (full, empty, extension+trailer) stays a
 * clean 200, a short Content-Length body is THE SAME REFUSAL on both paths
 * (t3 review F2, decided: the framing rule is not advisory -- read-to-close
 * is the framing ONLY where no length was declared), and responses that
 * cannot carry a body (HEAD, 204, 304) are not judged against the chunked
 * rule. A raw socket reads the App's own bytes
 * to prove the wire shape the refusals are reacting to. JSON results are
 * untouched.
 *
 * TWO PROCESSES, ALWAYS (the test_http_params rule): App handlers run on
 * the JS thread, so the client half runs in THIS process against a dynajs
 * child serving both sides' fixtures.
 *
 * Needs python3? No -- needs nothing but the built binary. Skips are loud
 * anyway if the child cannot start.
 */
import * as std from "std";
import { HTTPClient } from "dyna:http";
import { TCPServer } from "dyna:net";
import * as stream from "dyna:stream";
import { Exec } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const T = makeTempDir("http_stream");
const P = (n) => T + "/" + n;

/* Fixtures: a python upstream serves the wire shapes for the client half;
   the App under test runs in a dynajs child for the server half. */

const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

/* Fixture lifecycle. Every fixture is a background child of a short-lived
   shell, so it OUTLIVES the script unless it is killed: the App child holds
   its loop open with an interval and the python servers serve forever. A row
   that fails exits the script before any cleanup at the end would run, and in
   a parallel gate the survivors accumulate across runs (ports, fds). So each
   child's pid is recorded HERE, at spawn, and the teardown at the tail runs
   from a `finally` -- on the passing path and on the throwing one. Pids, not
   name patterns: a pattern could match another worker's children. */
const FIXTURES = [];
function startFixture(cmd, needle, log) {
    const pidfile = P(log + ".pid");
    sh(`rm -f ${pidfile}; ${cmd} >${P(log + ".log")} 2>&1 & echo $! > ${pidfile}`);
    let pid = 0;
    for (let i = 0; i < 50 && !pid; i++) {
        sh("sleep 0.05");
        try { pid = parseInt(readFile(new Path(pidfile)), 10) || 0; } catch (e) {}
    }
    FIXTURES.push({ pid, needle });
    return pid;
}
const fixtureAlive = (f) =>
    f.pid > 0 && sh(`ps -p ${f.pid} -o command= 2>/dev/null | grep -qF '${f.needle}'`) === 0;
let reaped = false;
function reapFixtures() {
    if (reaped) return 0;
    reaped = true;
    let stopped = 0, left = 0;
    for (const f of FIXTURES) {
        if (!fixtureAlive(f)) continue;
        sh(`kill ${f.pid} 2>/dev/null; sleep 0.2; kill -9 ${f.pid} 2>/dev/null`);
        if (!fixtureAlive(f)) stopped++; else left++;
    }
    print("  fixtures reaped: " + stopped + " child process(es) stopped, " + left + " still alive");
    if (left === 0 && fail === 0) sh(`rm -rf ${T}`);
    return left;
}

writeFile(new Path(P("up.py")), [
    "import http.server, sys",
    "import threading as _th, os as _os",
    "_th.Timer(120, lambda: _os._exit(0)).start()",
    "class H(http.server.BaseHTTPRequestHandler):",
    "    protocol_version='HTTP/1.1'",
    "    def log_message(self,*a): pass",
    "    def do_GET(self):",
    "        p=self.path",
    "        if p=='/cl':",
    "            b=bytes(i%256 for i in range(8192))",
    "            self.send_response(200); self.send_header('Content-Type','application/x-bin'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)",
    "        elif p=='/chunked':",
    "            self.send_response(200); self.send_header('Transfer-Encoding','chunked'); self.end_headers()",
    "            for i in range(3):",
    "                self.wfile.write(b'5\\r\\nhello\\r\\n'); self.wfile.flush()",
    "            self.wfile.write(b'0\\r\\n\\r\\n')",
    "        elif p=='/truncated':",
    "            self.send_response(200); self.send_header('Content-Length','500'); self.end_headers(); self.wfile.write(b'x'*200); self.wfile.flush(); self.close_connection=True",
    "        elif p=='/huge':",
    "            self.send_response(200); self.send_header('Content-Length','99999999'); self.end_headers()",
    "        else:",
    "            self.send_response(404); self.send_header('Content-Length','0'); self.end_headers()",
    "srv=http.server.ThreadingHTTPServer(('127.0.0.1',0), H)",
    "open(sys.argv[1],'w').write(str(srv.server_address[1]))",
    "srv.serve_forever()",
].join("\n"));
const UP_PID = startFixture(`python3 ${P("up.py")} ${P("upport")}`, P("up.py"), "up");
let UP = 0;
for (let i = 0; i < 60 && !UP; i++) {
    sh("sleep 0.1");
    try { UP = parseInt(readFile(new Path(P("upport"))), 10) || 0; } catch (e) {}
}

/* A second upstream, byte-exact: http.server cannot send a body that stops
   before its terminal chunk, and that cut IS the shape under test. Each route
   is the literal octets the peer writes before closing. */
writeFile(new Path(P("raw.py")), [
    "import socket, sys, os, threading as _th",
    "_th.Timer(120, lambda: os._exit(0)).start()",
    "HEAD = b'HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n'",
    "R = {",
    "  b'/eof-empty':  [HEAD],",
    "  b'/eof-size':   [HEAD, b'5'],",
    "  b'/eof-chunk':  [HEAD, b'5\\r\\nhel'],",
    "  b'/eof-term':   [HEAD, b'5\\r\\nhello\\r\\n'],",
    "  b'/complete':   [HEAD, b'5\\r\\nhello\\r\\n', b'5\\r\\nhello\\r\\n', b'5\\r\\nhello\\r\\n', b'0\\r\\n\\r\\n'],",
    "  b'/empty-ok':   [HEAD, b'0\\r\\n\\r\\n'],",
    "  b'/trailer':    [HEAD, b'5;ext=1\\r\\nhello\\r\\n', b'0\\r\\nX-T: v\\r\\n\\r\\n'],",
    "  b'/malformed':  [HEAD, b'5x\\r\\nhello\\r\\n0\\r\\n\\r\\n'],",
    "  b'/eof-trailer':[HEAD, b'0\\r\\nX-T: v\\r\\n'],",
    "  b'/head-te':    [HEAD],",
    "  b'/no-content': [b'HTTP/1.1 204 No Content\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n'],",
    "  b'/not-modified':[b'HTTP/1.1 304 Not Modified\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n'],",
    "  b'/cl-short':   [b'HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\nConnection: close\\r\\n\\r\\nab'],",
    "}",
    "s = socket.socket()",
    "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)",
    "s.bind(('127.0.0.1', 0)); s.listen(16)",
    "open(sys.argv[1], 'w').write(str(s.getsockname()[1]))",
    "while True:",
    "    c, _ = s.accept()",
    "    try:",
    "        d = b''",
    "        while b'\\r\\n\\r\\n' not in d:",
    "            x = c.recv(4096)",
    "            if not x: break",
    "            d += x",
    "        parts = d.split(b'\\r\\n', 1)[0].split(b' ')",
    "        path = parts[1] if len(parts) > 1 else b''",
    "        c.sendall(b''.join(R.get(path) or [b'HTTP/1.1 404 Not Found\\r\\nContent-Length: 0\\r\\nConnection: close\\r\\n\\r\\n']))",
    "    except OSError:",
    "        pass",
    "    finally:",
    "        try: c.shutdown(socket.SHUT_WR)",
    "        except OSError: pass",
    "        c.close()",
].join("\n"));
const RAW_PID = startFixture(`python3 ${P("raw.py")} ${P("rawport")}`, P("raw.py"), "raw");
let RAW = 0;
for (let i = 0; i < 60 && !RAW; i++) {
    sh("sleep 0.1");
    try { RAW = parseInt(readFile(new Path(P("rawport"))), 10) || 0; } catch (e) {}
}

/* the App child */
writeFile(new Path(P("srv.js")), `
import { App } from "dyna:http";
import * as stream from "dyna:stream";
import { writeFile, Path } from "dyna:file";
const app = new App();
app.rpc("/stream", {
    flat: () => stream.fromBytes("chunk-zero\\nchunk-one\\nchunk-two\\n"),
    shaped: () => ({ stream: stream.fromBytes("A".repeat(70000)),
                     contentType: "text/x-big", status: 201 }),
    slowlines: () => {
        let i = 0;
        return {
            async read(buf) {
                if (i >= 5) return 0;
                const s = "line" + (i++) + "\\n";
                buf.set(new TextEncoder().encode(s));
                await new Promise((res) => setTimeout(res, 5));
                return s.length;
            },
            close() { globalThis.__closed = true; },
        };
    },
    plain: () => ({ still: "json" }),
    boom: () => ({
        async read(buf) {
            if (globalThis.__n) throw new Error("mid-stream explosion");
            globalThis.__n = 1;
            buf[0] = 66;
            return 1;
        },
    }),
    badread: () => ({ read() { return -1; } }),
});
app.start();
writeFile(new Path("${P("appport")}"), String(app.port));
setInterval(() => {}, 1000);
`);
const APP_PID = startFixture(`./dynajs ${P("srv.js")}`, P("srv.js"), "srv");
let APP = 0;
for (let i = 0; i < 100 && !APP; i++) {
    sh("sleep 0.1");
    try { APP = parseInt(readFile(new Path(P("appport"))), 10) || 0; } catch (e) {}
}
/* Everything from the fixture readiness check to the last row, so the
   teardown below runs whether the body returns or throws. */
async function main() {
    if (!UP || !APP || !RAW) {
        print("  FAIL: fixtures did not start (up=" + UP + " app=" + APP + " raw=" + RAW +
              " pids=" + UP_PID + "/" + APP_PID + "/" + RAW_PID + ")");
        throw new Error("http_stream: fixtures failed");
    }
    const UB = "http://127.0.0.1:" + UP;
    const AB = "http://127.0.0.1:" + APP;
    const RB = "http://127.0.0.1:" + RAW;

    /* ==== the client half: HTTPClient.getStream ============================== */
    {
        const client = new HTTPClient(1 << 20);

        const r = client.getStream(UB + "/cl");
        ok(r.status === 200 && r.ok === true, "getStream: status/ok surface");
        ok(r.contentType === "application/x-bin", "getStream: contentType from the head");
        ok(typeof r.read === "function" && typeof r.close === "function",
           "getStream: the response IS the ByteSource (read/close)");
        {
            const buf = new Uint8Array(1000);
            let total = 0, seq = true;
            for (;;) {
                const n = await r.read(buf);
                if (n === 0) break;
                for (let i = 0; i < n; i++)
                    if (buf[i] !== (total + i) % 256) { seq = false; break; }
                total += n;
            }
            ok(total === 8192, "getStream: content-length body streamed whole (" + total + ")");
            ok(seq, "getStream: every byte in order");
        }
        r.close();

        const rc = client.getStream(UB + "/chunked");
        let text = "";
        for await (const l of stream.lines(rc)) text += l;
        ok(text === "hellohellohello",
           "getStream: dyna:stream lines() over a chunked body (" + JSON.stringify(text) + ")");
        rc.close();

        const rt = client.getStream(UB + "/truncated");
        const buf = new Uint8Array(150);
        let got = 0, rejected = false, msg = "";
        for (;;) {
            try { const n = await rt.read(buf); if (n === 0) break; got += n; }
            catch (e) { rejected = true; msg = String(e); break; }
        }
        ok(rejected && /truncat/i.test(msg),
           "getStream: short body against Content-Length REJECTS naming it [" +
           msg.slice(0, 60) + "]");
        ok(got <= 300, "getStream: the bytes before the cut are bounded by the reads (" + got + ")");
        rt.close();

        let refused = false;
        const small = new HTTPClient(4096);
        try { small.getStream(UB + "/huge"); }
        catch (e) { refused = /max body|Content-Length/i.test(String(e)); }
        ok(refused, "getStream: declared Content-Length over the client cap refuses at open");
        small.close();

        const rg = client.getStream(UB + "/gone");
        ok(rg.status === 404 && !rg.ok, "getStream: a 3xx-class status is surfaced as-is (no chase, like get())");
        ok((await rg.read(new Uint8Array(8))) === 0, "getStream: empty body reads 0");
        rg.close();
        client.close();
    }

    /* ==== the truncation rule, on the wire ==================================
     *
     * A chunked body has no defined length until its terminal chunk: a peer that
     * closes first is CUT, and the cut is the whole signal (it is how a streamed
     * handler's failure reaches the client once the status line is out). So every
     * one of these exchanges must REFUSE with the truncation named, and the two
     * legal ends -- a full body and an empty one -- must stay clean 200s. The
     * fixtures above are byte-exact for exactly this reason. */
    {
        const client = new HTTPClient(1 << 20);
        const swallow = (fn) => { try { return { r: fn(), threw: false, msg: "" }; }
                                  catch (e) { return { r: null, threw: true, msg: String(e) }; } };

        for (const [path, what] of [["/eof-empty", "no body bytes at all"],
                                    ["/eof-size", "EOF mid chunk-size line"],
                                    ["/eof-chunk", "EOF mid chunk"],
                                    ["/eof-term", "EOF between chunks (no terminal chunk)"],
                                    ["/eof-trailer", "EOF inside the trailer section"]]) {
            const x = swallow(() => client.get(RB + path));
            ok(x.threw && /truncat|incomplete/i.test(x.msg),
               "chunked cut refused: " + what + " [" + (x.msg || JSON.stringify(x.r && x.r.body)).slice(0, 72) + "]");
        }

        {
            const rc = client.get(RB + "/complete");
            ok(rc.status === 200 && rc.body === "hellohellohello",
               "chunked complete body stays a clean 200 (" + JSON.stringify(rc.body) + ")");
            const re = client.get(RB + "/empty-ok");
            ok(re.status === 200 && re.body === "" && re.bodyBytes.byteLength === 0,
               "chunked empty body (legal terminal chunk) stays a clean 200 with no body");
            const rt = client.get(RB + "/trailer");
            ok(rt.status === 200 && rt.body === "hello",
               "chunk extension and trailer fields are consumed, body intact (" +
               JSON.stringify(rt.body) + ")");
            const xm = swallow(() => client.get(RB + "/malformed"));
            ok(xm.threw && /malformed|truncat|incomplete/i.test(xm.msg),
               "a chunked body that does not follow the grammar is refused [" +
               (xm.msg || "").slice(0, 72) + "]");
        }

        /* Responses that cannot carry a body: a legal HEAD answer to a chunked
           resource, 204 and 304 arrive with the head and ZERO body bytes. Having
           no terminal chunk there is not a cut. */
        {
            const rh = swallow(() => client.request("HEAD", RB + "/head-te"));
            ok(!rh.threw && rh.r.status === 200 && rh.r.body === "",
               "HEAD of a chunked resource: 200 with the head and no body [" +
               (rh.threw ? rh.msg.slice(0, 72) : JSON.stringify(rh.r.body)) + "]");
            const rn = swallow(() => client.get(RB + "/no-content"));
            ok(!rn.threw && rn.r.status === 204 && rn.r.body === "",
               "204 with chunked framing and no body is not a cut [" +
               (rn.threw ? rn.msg.slice(0, 72) : JSON.stringify(rn.r.body)) + "]");
            const rm = swallow(() => client.get(RB + "/not-modified"));
            ok(!rm.threw && rm.r.status === 304 && rm.r.body === "",
               "304 with chunked framing and no body is not a cut [" +
               (rm.threw ? rm.msg.slice(0, 72) : JSON.stringify(rm.r.body)) + "]");
        }

        /* A short Content-Length body is a TRUNCATED exchange, not a
           close-framed one: the declared length makes the framing, and the
           close is the peer admitting it never finished. Same refusal as
           the chunked cuts above (t3 review F2). */
        {
            const rs = swallow(() => client.get(RB + "/cl-short"));
            ok(rs.threw && /truncat/i.test(rs.msg),
               "a short Content-Length body refuses naming the cut [" +
               (rs.threw ? rs.msg.slice(0, 88) : JSON.stringify(rs.r && rs.r.body)) + "]");
        }

        client.close();
    }

    /* ==== the same rule through getStream (the streaming path) ===============
     * Both paths must name the same cut: a read past a truncated chunked body
     * REJECTS, it never reports a clean end-of-body. */
    {
        const client = new HTTPClient(1 << 20);
        const readAll = async (url) => {
            const r = client.getStream(url);
            const buf = new Uint8Array(64);
            let total = 0;
            try {
                for (;;) { const n = await r.read(buf); if (n === 0) break; total += n; }
                return { threw: false, total, msg: "" };
            } catch (e) {
                return { threw: true, total, msg: String(e) };
            } finally { r.close(); }
        };

        const cs = await readAll(RB + "/eof-size");
        ok(cs.threw && /truncat|incomplete/i.test(cs.msg),
           "getStream: EOF mid chunk-size line rejects naming the cut [" + cs.msg.slice(0, 72) + "]");
        const cc = await readAll(RB + "/eof-chunk");
        ok(cc.threw && /truncat|incomplete/i.test(cc.msg),
           "getStream: EOF mid chunk rejects naming the cut [" + cc.msg.slice(0, 72) + "]");
        const ct = await readAll(RB + "/eof-term");
        ok(ct.threw && /truncat|incomplete/i.test(ct.msg),
           "getStream: EOF before the terminal chunk rejects naming the cut [" + ct.msg.slice(0, 72) + "]");
        const cf = await readAll(RB + "/complete");
        ok(!cf.threw && cf.total === 15,
           "getStream: a complete chunked body still reads whole (" + cf.total + ")");
        const ce = await readAll(RB + "/empty-ok");
        ok(!ce.threw && ce.total === 0, "getStream: a legal empty chunked body reads 0");
        const cs2 = await readAll(RB + "/cl-short");
        ok(cs2.threw && /truncat/i.test(cs2.msg),
           "getStream: a short Content-Length body rejects naming the cut, the same refusal the buffered client makes [" +
           (cs2.threw ? cs2.msg.slice(0, 88) : "clean end after " + cs2.total) + "]");
        client.close();
    }

    /* ==== what the App actually puts on the wire =============================
     * The client-side refusal is only honest if the bytes deserve it: a raw
     * socket (no client library in between) reads the App's answer to each route
     * and asserts the framing literally -- the clean stream ends with the
     * terminal chunk, the two cut streams send the 200 head and then stop. */
    {
        const rawPost = (method) => new Promise((resolve) => {
            let buf = "";
            let settled = false;
            const done = (v) => { if (!settled) { settled = true; resolve(v); } };
            const t = setTimeout(() => done({ raw: buf, err: "timeout" }), 15000);
            const body = JSON.stringify({ jsonrpc: "2.0", method, id: 1 });
            const conn = TCPServer.connect({ host: "127.0.0.1", port: APP }, {
                connect(c) {
                    c.write("POST /stream HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
                            "Content-Type: application/json\r\nContent-Length: " +
                            body.length + "\r\nConnection: close\r\n\r\n" + body);
                },
                data(c, bytes) { buf += new TextDecoder().decode(bytes); },
                close() { clearTimeout(t); done({ raw: buf }); },
            });
            setTimeout(() => { try { conn.close(); } catch (e) {} }, 16000);
        });

        const wf = await rawPost("flat");
        ok(/^HTTP\/1\.1 200 /.test(wf.raw) && wf.raw.indexOf("Transfer-Encoding: chunked") > 0 &&
           /\r\n0\r\n\r\n$/.test(wf.raw),
           "wire: a clean streamed body ends with the terminal chunk [" +
           wf.raw.slice(wf.raw.length - 24).replace(/\r/g, "\\r").replace(/\n/g, "\\n") + "]");

        const wb = await rawPost("boom");
        ok(/^HTTP\/1\.1 200 /.test(wb.raw) && wb.raw.indexOf("0\r\n\r\n") < 0 &&
           /1\r\nB\r\n$/.test(wb.raw),
           "wire: a mid-stream throw leaves the 200 head out and ends after the last framed byte [" +
           wb.raw.slice(-16).replace(/\r/g, "\\r").replace(/\n/g, "\\n") + "]");

        const wr = await rawPost("badread");
        ok(/^HTTP\/1\.1 200 /.test(wr.raw) && wr.raw.indexOf("0\r\n\r\n") < 0 &&
           wr.raw.indexOf("\r\n\r\n") === wr.raw.length - 4,
           "wire: an out-of-range read count truncates before any chunk [" +
           wr.raw.slice(-16).replace(/\r/g, "\\r").replace(/\n/g, "\\n") + "]");
    }

    /* ==== the server half: App handlers returning ByteSources ================ */
    {
        const client = new HTTPClient(1 << 22);
        const post = (method) => client.post(AB + "/stream",
            JSON.stringify({ jsonrpc: "2.0", method, id: 1 }),
            { "Content-Type": "application/json" });

        const rj = post("plain");
        ok(rj.status === 200 && JSON.parse(rj.body).result.still === "json",
           "HP-4: a JSON result keeps the JSON-RPC envelope");

        const rf = post("flat");
        ok(rf.status === 200 && rf.headers["Transfer-Encoding"] === "chunked",
           "HP-4: a ByteSource result answers chunked");
        ok(rf.headers["Content-Type"] === "application/octet-stream",
           "HP-4: the default content type is application/octet-stream");
        ok(rf.body === "chunk-zero\nchunk-one\nchunk-two\n",
           "HP-4: the body is the source's bytes, verbatim, out of the envelope");

        const rs = post("shaped");
        ok(rs.status === 201, "HP-4: {stream,contentType,status} honors the status");
        ok(rs.headers["Content-Type"] === "text/x-big", "HP-4: it honors the contentType");
        ok(rs.bodyBytes.byteLength === 70000 &&
           new Uint8Array(rs.bodyBytes)[0] === 65 &&
           new Uint8Array(rs.bodyBytes)[69999] === 65,
           "HP-4: 70000 bytes arrive framed and intact (" + rs.bodyBytes.byteLength + ")");

        const ra = post("slowlines");
        ok(ra.body.split("\n").filter(Boolean).length === 5,
           "HP-4: an async source's awaited reads all frame (5 lines)");
        ok(ra.headers["Transfer-Encoding"] === "chunked",
           "HP-4: the async source rides the same framing");

        /* A source that throws mid-stream ends the body early: the head is long
           gone (see the raw-socket rows above), so the buffered client sees a
           chunked body with no terminal chunk and must refuse it, not report the
           bytes that made it as a complete 200. */
        {
            let cut = false, msg = "";
            try { post("boom"); } catch (e) { cut = true; msg = String(e); }
            ok(cut && /truncat|incomplete/i.test(msg),
               "HP-4: a mid-stream throw truncates the body (the client sees the cut) [" +
               msg.slice(0, 72) + "]");
        }

        {
            let cut = false, msg = "";
            try { post("badread"); } catch (e) { cut = true; msg = String(e); }
            ok(cut && /truncat|incomplete/i.test(msg),
               "HP-4: a read resolving outside [0, buf.length] truncates the connection (the client sees the cut)");
        }

        /* The control for both rows above: the same route, same framing, read to
           its terminal chunk -- a clean 200 with the whole body. */
        {
            const rk = post("flat");
            ok(rk.status === 200 && rk.body === "chunk-zero\nchunk-one\nchunk-two\n",
               "HP-4: a complete streamed body is still a 200 with every byte");
        }

        client.close();
    }

}
try {
    await main();
} finally {
    reapFixtures();
}
print("test_http_stream: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_stream: " + fail + " failures");
