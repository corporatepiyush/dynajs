// covers: integration c:src/modules/libc/libc-fetch.c c:src/modules/http/http-client.c -- body handling of the fetch global over the http module client against a raw dyna:net TCPServer peer
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_fetch_body");
import { TCPServer } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

function until(cond, msg, ms = 5000) {
    return new Promise((res, rej) => {
        const t0 = Date.now();
        (function poll() {
            if (cond()) return res();
            if (Date.now() - t0 > ms) return rej(new Error("timeout waiting for " + msg));
            setTimeout(poll, 5);
        })();
    });
}

function captureServer(statusText) {
    const srv = new TCPServer({ port: 0 });
    let captured = "";
    const raw = [];
    srv.start({
        data: (c, bytes) => {
            captured += new TextDecoder().decode(bytes);
            raw.push(bytes);
            c.write("HTTP/1.1 200 " + statusText + "\r\n" +
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
        },
    });
    const rawBytes = () => {
        let total = 0;
        for (const b of raw) total += b.length;
        const out = new Uint8Array(total);
        let off = 0;
        for (const b of raw) { out.set(b, off); off += b.length; }
        return out;
    };
    return { srv, get: () => captured, rawBytes };
}

print("=== 1. fetch POST sends the string body on the wire ===");
await __kitS.section("request line is POST", async () => {
    const cap = captureServer("OK");
    const r = await fetch("http://127.0.0.1:" + cap.srv.port + "/t", {
        method: "POST", body: "hello",
    });
    eq(r.status, 200, "status");
    const wire = cap.get();
    ok(/^POST \/t HTTP\/1\.1\r\n/.test(wire), "request line is POST");
    ok(/Content-Length: 5/.test(wire), "Content-Length: 5 present (" +
       (wire.match(/Content-Length: [0-9]+/) || ["<none>"])[0] + ")");
    ok(wire.indexOf("\r\n\r\nhello") >= 0, "the body follows the headers");
    cap.srv.close();
});

print("=== 2. fetch POST sends a Uint8Array body ===");
await __kitS.section("Content-Length: 9 present", async () => {
    const cap = captureServer("OK");
    const payload = new Uint8Array([0, 1, 2, 250, 251, 252, 253, 254, 255]);
    const r = await fetch("http://127.0.0.1:" + cap.srv.port + "/t", {
        method: "POST", body: payload,
    });
    eq(r.status, 200, "status");
    const wire = cap.get();
    ok(/Content-Length: 9/.test(wire), "Content-Length: 9 present");
    const raw = cap.rawBytes();
    const headEnd = raw.lastIndexOf(0x0a);
    const body = raw.subarray(headEnd + 1);
    ok(body.length === 9 && body[0] === 0 && body[1] === 1 && body[2] === 2 &&
       body[3] === 250 && body[4] === 251 && body[5] === 252 &&
       body[6] === 253 && body[7] === 254 && body[8] === 255,
       "the raw bytes follow the headers verbatim");
    cap.srv.close();
});

print("=== 3. Request.body is a ReadableStream (Fetch spec; BufferSource bytes are copied at construction) ===");
await __kitS.section("string body", async () => {
    const r1 = new Request("http://x.test/", { method: "POST", body: "abc" });
    ok(r1.body instanceof ReadableStream, "string body is a ReadableStream");
    eq(await r1.text(), "abc", "string body");
    const u8 = new Uint8Array([1, 2, 3]);
    const r2 = new Request("http://x.test/", { method: "POST", body: u8 });
    u8[0] = 9;
    eq(Array.from(await r2.bytes()).join(","), "1,2,3", "Uint8Array body copied at construction");
    const r3 = new Request("http://x.test/");
    eq(r3.body, null, "no body is null");
});

print("=== 4. FormData POST still sends a multipart body (regression) ===");
await __kitS.section("multipart content type with boundary", async () => {
    const cap = captureServer("OK");
    const fd = new FormData();
    fd.append("a", "1");
    const r = await fetch("http://127.0.0.1:" + cap.srv.port + "/t", {
        method: "POST", body: fd,
    });
    eq(r.status, 200, "status");
    const wire = cap.get();
    ok(/content-type: multipart\/form-data; boundary=/i.test(wire),
       "multipart content type with boundary");
    ok(/Content-Length: [0-9]+/.test(wire), "multipart Content-Length present");
    ok(wire.indexOf('name="a"') >= 0, "the part name is on the wire");
    cap.srv.close();
});

print("=== 5. Response statusText defaults to '' (Fetch spec; owner rule 12 reverses the old 'OK' default) ===");
__kitS.section("default statusText is ''", () => {
    const r = new Response(null, { status: 404 });
    eq(r.statusText, "", "default statusText is ''");
    eq(new Response(null, { status: 200 }).statusText, "", "200 default is '' too");
    const custom = new Response(null, { status: 404, statusText: "Nope" });
    eq(custom.statusText, "Nope", "explicit statusText is honored");
    const rs = new Response(null, { status: 204 });
    eq(rs.statusText, "", "204 default is ''");
});

print("=== 6. fetch passes the peer's reason phrase through ===");
await __kitS.section("HTTP/1.1 404 Not Foundr", async () => {
    const srv = new TCPServer({ port: 0 });
    srv.start({
        data: (c) => c.write("HTTP/1.1 404 Not Found\r\n" +
                             "Content-Length: 7\r\nConnection: close\r\n\r\n" +
                             "missing"),
    });
    const r = await fetch("http://127.0.0.1:" + srv.port + "/t");
    eq(r.status, 404, "wire status");
    eq(r.statusText, "Not Found", "wire reason phrase, not the ctor default");
    eq(await r.text(), "missing", "body");
    srv.close();
});
__kitS.end();

if (fails) {
    print("test_http_fetch_body: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_http_fetch_body failed");
}
print("test_http_fetch_body: " + n + " assertions, 0 failures");
