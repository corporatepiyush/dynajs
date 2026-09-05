/* test_http_binary_body.js -- M10-05 differential: the response body must be
 * bytes-native end to end.
 *
 * THE GAP: dyn_build_response (src/dyna-http.c) handed the raw body to
 * JS_NewStringLen, so every byte >= 0x80 was mangled into a UTF-8 JS string
 * and fetch's Response.bytes() could only RE-ENCODE the mangled text. Network
 * binary content had no true remedy. This file is the proof that the bytes
 * path now survives the whole trip:
 *
 *   raw socket -> dyn_read_response -> bodyBytes (exact) -> fetch Response
 *   -> bytes()/arrayBuffer() EXACT, text() = on-demand UTF-8 decode.
 *
 * The origin is a TCPServer speaking raw HTTP/1.1 (the reactor's data handler
 * hands us an ArrayBuffer of the exact received bytes), so the test controls
 * every byte on the wire: all-256-values payloads, chunked framing with
 * boundaries at odd offsets, empty bodies, 1 MiB, and an ECHO route that
 * reflects the fetched request body back so client bytes are compared against
 * themselves after a full round trip.
 *
 * Run: dynajs tests/test_http_binary_body.js
 * Prints "test_http_binary_body: all tests passed" on success.
 */

import { TCPServer, HTTPClient, HTTPServer } from "dyna:net";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + String(a) + ", want " + String(b) + ")");

/* ---- payload builders ---------------------------------------------------- */

/* 4096 bytes covering ALL 256 byte values (consecutive i mod 256 cycles),
 * with NULs at every 256th position -- NULs are the classic string-path
 * killer (C truncation, strlen-based framing). */
function payload4k() {
    const b = new Uint8Array(4096);
    for (let i = 0; i < b.length; i++)
        b[i] = (i + ((i / 256) | 0) * 37) & 0xff;
    return b;
}

/* Deterministic 1 MiB from a xorshift64 PRNG: no short period that a framing
 * bug could accidentally satisfy. */
function payload1m() {
    const b = new Uint8Array(1 << 20);
    let s = 0x9e3779b97f4a7c15n;
    for (let i = 0; i < b.length; i++) {
        s ^= s << 13n; s &= 0xffffffffffffffffn;
        s ^= s >> 7n;
        s ^= s << 17n; s &= 0xffffffffffffffffn;
        b[i] = Number((s >> 24n) & 0xffn);
    }
    return b;
}

const UTF8_TEXT = "héllo wörld — кодировка 中文テスト 🚀🎉 ñ\n".repeat(64);
const UTF8_BYTES = new TextEncoder().encode(UTF8_TEXT);
const P4K = payload4k();
const P1M = payload1m();

/* ---- the oracle: WHATWG UTF-8 decoding with replacement ------------------- */
/* Independent re-implementation of the decoder TextDecoder must apply
 * (dyna-libc.c js_utf8_decode_whatwg): each maximally-invalid subpart
 * becomes exactly one U+FFFD. Used to prove text() never CORRUPTS binary. */
function whatwgUtf8Decode(bytes) {
    let out = "";
    let i = 0;
    const n = bytes.length;
    while (i < n) {
        const b0 = bytes[i];
        let cp, need, lower = 0, upper = 0x10ffff;
        if (b0 < 0x80) { cp = b0; need = 0; }
        else if (b0 >= 0xc2 && b0 <= 0xdf) { cp = b0 & 0x1f; need = 1; lower = 0x80; upper = 0x7ff; }
        else if (b0 >= 0xe0 && b0 <= 0xef) { cp = b0 & 0x0f; need = 2; lower = 0x800; upper = 0xffff; }
        else if (b0 >= 0xf0 && b0 <= 0xf4) { cp = b0 & 0x07; need = 3; lower = 0x10000; upper = 0x10ffff; }
        else { out += "\uFFFD"; i++; continue; }
        if (i + need >= n + 1 && i + need > n - 1) { /* fallthrough */ }
        let j = 1, valid = true;
        for (; j <= need; j++) {
            if (i + j >= n || (bytes[i + j] & 0xc0) !== 0x80) { valid = false; break; }
            cp = (cp << 6) | (bytes[i + j] & 0x3f);
        }
        if (!valid || cp < lower || cp > upper ||
            (cp >= 0xd800 && cp <= 0xdfff)) {
            out += "\uFFFD"; i++;          /* one replacement per bad subpart */
        } else {
            out += String.fromCodePoint(cp);
            i += need + 1;
        }
    }
    return out;
}

/* ---- byte-exact comparison ----------------------------------------------- */
function bytesEqual(a, b) {
    if (!(a instanceof Uint8Array) || !(b instanceof Uint8Array)) return false;
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (a[i] !== b[i]) return false;
    return true;
}

/* ---- the raw HTTP/1.1 origin (TCPServer on the reactor) -------------------- */
const routes = {
    "/bin4k":    { ct: "application/octet-stream", body: P4K },
    "/utf8":     { ct: "text/plain; charset=utf-8", body: UTF8_BYTES },
    "/empty":    { ct: "application/octet-stream", body: new Uint8Array(0) },
    "/big":      { ct: "application/octet-stream", body: P1M },
    "/nul-heavy":{ ct: "application/octet-stream",
                   body: new Uint8Array(1024).fill(0).map((_, i) => (i % 3 === 0 ? 0 : i & 0xff)) },
};

/* chunk boundaries at deliberately ODD offsets: a dechunker that resyncs on
 * the wrong byte cannot reassemble this exactly */
const CHUNK_CUTS = [7, 1, 1001, 255, 3, 4095, 64];
function chunkedFrame(bytes) {
    const parts = [];
    let off = 0, k = 0;
    while (off < bytes.length) {
        const cut = Math.min(bytes.length - off, CHUNK_CUTS[k++ % CHUNK_CUTS.length]);
        parts.push(new TextEncoder().encode(cut.toString(16) + "\r\n"));
        parts.push(bytes.subarray(off, off + cut));
        parts.push(new TextEncoder().encode("\r\n"));
        off += cut;
    }
    parts.push(new TextEncoder().encode("0\r\n\r\n"));
    /* ONE write: the chunk boundaries live in the byte stream, so the client
     * dechunker still sees every odd cut. */
    return concatBytes2(parts);
}
function concatBytes2(list) {
    let n = 0;
    for (const p of list) n += p.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of list) { out.set(p, o); o += p.length; }
    return out;
}

/* close() drops QUEUED sends (tcp_conn_close -> dyn_aio_close), so give the
 * reactor a turn to drain the socket buffer before closing. Every response
 * declares Connection: close, and close-framed clients read to EOF. */
function closeLater(c) {
    setTimeout(() => { try { c.close(); } catch (e) {} }, 25);
}

function httpHead(status, ct, len, extra) {
    return "HTTP/1.1 " + status + " X\r\nContent-Type: " + ct +
           "\r\nContent-Length: " + len + "\r\n" + (extra || "") +
           "Connection: close\r\n\r\n";
}

const srv = new TCPServer({ port: 0 });
const pending = new Map();   /* conn -> accumulated request bytes */
srv.start({
    data(c, buf) {
        const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
        let acc = pending.get(c);
        acc = acc ? concatBytes(acc, u8) : u8;
        pending.set(c, acc);

        const headEnd = findCRLFCRLF(acc);
        if (headEnd < 0) return;                    /* headers incomplete */
        const head = new TextDecoder().decode(acc.subarray(0, headEnd));
        const clMatch = /content-length:\s*(\d+)/i.exec(head);
        const want = clMatch ? parseInt(clMatch[1], 10) : 0;
        const bodyStart = headEnd + 4;
        if (acc.length - bodyStart < want) return;  /* body incomplete */
        const body = acc.subarray(bodyStart, bodyStart + want);
        pending.delete(c);

        const path = head.split(" ")[1] || "/";
        /* every response below declares Connection: close, so the origin
           MUST close: the client reads to EOF for close-framed bodies */
        if (path === "/echo") {
            c.write(new TextEncoder().encode(httpHead(200, "application/octet-stream", body.length)));
            if (body.length) c.write(body);
            closeLater(c);
            return;
        }
        if (path === "/chunked-bin") {
            const headBuf = new TextEncoder().encode(
                "HTTP/1.1 200 X\r\nContent-Type: application/octet-stream\r\n" +
                "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
            c.write(concatBytes(headBuf, chunkedFrame(P4K)));
            closeLater(c);
            return;
        }
        const r = routes[path];
        if (!r) {
            c.write(new TextEncoder().encode(httpHead(404, "text/plain", 9)));
            closeLater(c);
            return;
        }
        c.write(new TextEncoder().encode(httpHead(200, r.ct, r.body.length)));
        if (r.body.length) c.write(r.body);
        closeLater(c);
    },
    close(c) { pending.delete(c); },
});
const BASE = "http://127.0.0.1:" + srv.port;

function concatBytes(a, b) {
    const out = new Uint8Array(a.length + b.length);
    out.set(a); out.set(b, a.length);
    return out;
}
function findCRLFCRLF(b) {
    outer: for (let i = 0; i + 4 <= b.length; i++) {
        if (b[i] === 13 && b[i+1] === 10 && b[i+2] === 13 && b[i+3] === 10)
            return i;
        continue outer;
    }
    return -1;
}

/* Run each section independently: one crash must not hide the others. */
const SECTIONS = [];
function section(name, fn) { SECTIONS.push([name, fn]); }
async function runSections() {
    for (const [name, fn] of SECTIONS) {
        try {
            await fn();
        } catch (e) {
            fail++;
            print("  FAIL: section '" + name + "' threw: " +
                  (e && e.message !== undefined ? e.message : String(e)));
        }
    }
}

section("fetch bytes differential", async () => {
    /* ---- 0. chunked FIRST: does ordering matter? ---- */
    {
        const r = await fetch(BASE + "/chunked-bin");
        const got = await r.bytes();
        ok(bytesEqual(got, P4K), "chunked-first reassembles EXACTLY (" + got.length + ")");
    }
    /* ---- 1. THE differential: fetch binary, bytes() must be EXACT ---- */
    {
        const r = await fetch(BASE + "/bin4k");
        eq(r.status, 200, "fetch /bin4k status");
        const got = await r.bytes();
        ok(got instanceof Uint8Array, "bytes() returns a Uint8Array");
        ok(bytesEqual(got, P4K), "4 KiB all-256-values payload survives EXACTLY (" +
           (got ? got.length : "null") + " bytes)");
        const r2 = await fetch(BASE + "/bin4k");
        const ab = await r2.arrayBuffer();
        ok(ab instanceof ArrayBuffer && bytesEqual(new Uint8Array(ab), P4K),
           "arrayBuffer() hands out the exact bytes");
        /* text() = the WHATWG replacement decode of the same bytes */
        const r3 = await fetch(BASE + "/bin4k");
        eq(await r3.text(), whatwgUtf8Decode(P4K),
           "text() of binary is the WHATWG replacement decode (no corruption)");
    }

    /* ---- 2. valid UTF-8: text() byte-compatible with the source text ---- */
    {
        const r = await fetch(BASE + "/utf8");
        const got = await r.bytes();
        ok(bytesEqual(got, UTF8_BYTES), "UTF-8 text bytes exact (" +
           got.length + " bytes)");
        const r2 = await fetch(BASE + "/utf8");
        eq(await r2.text(), UTF8_TEXT, "text() equals the UTF-8 source string");
        const r3 = await fetch(BASE + "/utf8");
        /* json() on a JSON body built from the same bytes */
        const obj = { "ключ": "値", n: 42, arr: [1, 2, 3] };
        const jbytes = new TextEncoder().encode(JSON.stringify(obj));
        const r4 = await fetch(BASE + "/echo", { method: "POST", body: jbytes });
        eq(JSON.stringify(await r4.json()), JSON.stringify(obj),
           "json() decodes fetched JSON exactly");
    }

    /* ---- 3. empty body ---- */
    {
        const r = await fetch(BASE + "/empty");
        const got = await r.bytes();
        ok(got instanceof Uint8Array && got.length === 0, "empty body -> 0 bytes");
        const r2 = await fetch(BASE + "/empty");
        eq(await r2.text(), "", "empty body -> empty text");
    }

    /* ---- 4. 1 MiB ---- */
    {
        const r = await fetch(BASE + "/big");
        const got = await r.bytes();
        ok(bytesEqual(got, P1M), "1 MiB PRNG payload survives EXACTLY");
    }

    /* ---- 5. chunked framing, odd cut points ---- */
    {
        const r = await fetch(BASE + "/chunked-bin");
        const got = await r.bytes();
        ok(bytesEqual(got, P4K), "chunked binary reassembles EXACTLY");
    }

    /* ---- 6. ECHO: fetched bytes round-trip the wire a second time ---- */
    {
        const r = await fetch(BASE + "/echo", { method: "POST", body: P4K });
        eq(r.status, 200, "echo status");
        ok(bytesEqual(await r.bytes(), P4K), "ECHO loop closed: POSTed bytes return EXACTLY");
        const r2 = await fetch(BASE + "/echo", { method: "POST", body: UTF8_BYTES });
        ok(bytesEqual(await r2.bytes(), UTF8_BYTES), "ECHO of UTF-8 bytes exact");
        const r3 = await fetch(BASE + "/echo",
                               { method: "POST", body: new Uint8Array([0, 1, 2, 0, 255, 0]) });
        ok(bytesEqual(await r3.bytes(), new Uint8Array([0, 1, 2, 0, 255, 0])),
           "ECHO with interior NULs exact");
    }

    /* ---- 6b. NUL-dense payload: interior NULs mid-stream ---- */
    {
        const r = await fetch(BASE + "/nul-heavy");
        const got = await r.bytes();
        ok(bytesEqual(got, routes["/nul-heavy"].body),
           "NUL-dense payload survives EXACTLY (1024 bytes, 1/3 NULs)");
        eq(got[0], 0, "first byte is NUL and present");
        const r2 = await fetch(BASE + "/nul-heavy");
        const t = await r2.text();
        eq(t.length, whatwgUtf8Decode(routes["/nul-heavy"].body).length,
           "text() of NUL-dense payload matches oracle length");
        eq(t, whatwgUtf8Decode(routes["/nul-heavy"].body),
           "text() of NUL-dense payload matches oracle exactly");
    }

    /* ---- 6c. maxBody cap on the raw bytes path: refusal, not truncation.
       Sync clients block the loop, so this runs against the THREADED
       HTTPServer (served from worker threads), like section 7. ---- */
    {
        const hs = new HTTPServer({
            port: 0,
            routes: {
                "/bin": { status: 200, contentType: "application/octet-stream", body: P4K },
                "/empty": { status: 200, contentType: "application/octet-stream", body: new Uint8Array(0) },
            },
        });
        hs.start();
        const hbase = "http://127.0.0.1:" + hs.port;
        const c = new HTTPClient(1024);          /* 1 KiB cap vs 4 KiB body */
        try {
            let threw = null;
            try { c.get(hbase + "/bin"); }
            catch (e) { threw = e; }
            ok(!!threw, "sync get over maxBody REFUSES");
            ok(!!threw && /large|size|exceed/i.test(threw.message),
              "refusal says the body was too big (" + (threw && threw.message) + ")");
        } finally { c.close(); }
        const c2 = new HTTPClient(1024);
        try {
            let rejected = null;
            try { await c2.getAsync(hbase + "/bin"); }
            catch (e) { rejected = e; }
            ok(!!rejected && /large|size|exceed/i.test(rejected.message),
               "async get over maxBody rejects the same way (" +
               (rejected && rejected.message) + ")");
        } finally { c2.close(); }
        /* the cap must not clip honest small bodies */
        const c3 = new HTTPClient(1024);
        try {
            const r = c3.get(hbase + "/empty");
            eq(r.status, 200, "maxBody client still serves small bodies");
            eq(new Uint8Array(r.bodyBytes).length, 0, "and its bodyBytes stay exact");
        } finally { c3.close(); }
        hs.close();
    }

    /* ---- 6d. Response clone/consume semantics on a bytes body ---- */
    {
        const r = await fetch(BASE + "/bin4k");
        const clone = r.clone();
        const a = await r.bytes();
        const b = await clone.bytes();
        ok(bytesEqual(a, P4K) && bytesEqual(b, P4K),
           "clone before consume: BOTH copies get the exact bytes");
        let threw = false;
        try { await r.text(); } catch (e) { threw = true; }
        ok(threw, "consuming twice still throws (bodyUsed intact)");
        /* user-constructed bytes body keeps working */
        const u = new Response(P4K, { status: 200 });
        ok(bytesEqual(await u.bytes(), P4K),
           "Response constructed with Uint8Array bytes() exact");
        const u2 = new Response(P4K, { status: 200 });
        eq(await u2.text(), whatwgUtf8Decode(P4K),
           "Response constructed with Uint8Array text() = WHATWG decode");
    }

    /* ---- 7+8. threaded HTTPServer: HTTPClient parity (sync client blocks
       the loop, so it must be served by worker threads, not by the
       TCPServer-on-the-loop origin) and byte-view static routes ---- */
    {
        const hs = new HTTPServer({
            port: 0,
            routes: {
                "/bin": { status: 200, contentType: "application/octet-stream", body: P4K },
                "/utf8": { status: 200, contentType: "text/plain; charset=utf-8", body: UTF8_BYTES },
                "/empty": { status: 200, contentType: "application/octet-stream", body: new Uint8Array(0) },
                "/txt": "hello world",
            },
        });
        hs.start();
        const hbase = "http://127.0.0.1:" + hs.port;
        const c = new HTTPClient();
        try {
            const r = c.get(hbase + "/bin");
            eq(r.status, 200, "HTTPClient status");
            ok(r.bodyBytes instanceof ArrayBuffer, "HTTPClient bodyBytes is an ArrayBuffer");
            ok(bytesEqual(new Uint8Array(r.bodyBytes), P4K),
               "HTTPClient bodyBytes EXACT for all-256-values payload");
            const rt = c.get(hbase + "/utf8");
            eq(rt.body, UTF8_TEXT, "HTTPClient .body string view unchanged for UTF-8");
            ok(bytesEqual(new Uint8Array(rt.bodyBytes), UTF8_BYTES),
               "HTTPClient bodyBytes exact for UTF-8");
            const re = c.get(hbase + "/empty");
            ok(re.bodyBytes instanceof ArrayBuffer && re.bodyBytes.byteLength === 0,
               "HTTPClient bodyBytes empty body");
            const r2 = c.get(hbase + "/txt");
            eq(r2.body, "hello world", "HTTPServer string route unchanged");
            const f = await fetch(hbase + "/bin");
            ok(bytesEqual(await f.bytes(), P4K),
               "fetch of HTTPServer binary route EXACT");
        } finally { c.close(); hs.close(); }
    }
});

async function main() {
    await runSections();
    srv.close();
    if (fail === 0)
        print("test_http_binary_body: all tests passed (" + pass + " assertions)");
    else
        print("test_http_binary_body: " + fail + " FAILURES (" + pass + " passed)");
    return fail === 0 ? 0 : 1;
}

main().then((rc) => { if (rc) throw new Error("differential failed"); },
            (e) => { print("FATAL: " + (e && e.stack || e)); std_exit(1); });

function std_exit(code) { import("std").then((std) => std.exit(code)); }
