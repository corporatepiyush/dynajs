/* test_http_proxy.js -- App.proxy, the L7 reverse proxy.
 *
 * The point of an L7 proxy is that it re-serialises from its OWN parse. So the
 * assertions are about what the BACKEND received, not about what the client
 * got back: a proxy that forwarded raw bytes would still return 200.
 *
 * The backend here is a raw TCPServer, deliberately NOT an App -- a real
 * server would normalise the very thing under test, so a strict backend cannot
 * distinguish "the proxy cleaned it" from "the backend cleaned it".
 */
import { App, TCPServer } from "dyna:net";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function bytes(s) {
    const a = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
    return a;
}
function str(b) {
    let s = "";
    for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    return s;
}

let seen = "";          /* exactly what the backend was sent */
const back = new TCPServer({ port: 0 });
back.start({ data: (c, b) => {
    seen += str(b);
    if (seen.indexOf("\r\n\r\n") >= 0) {
        const body = "hi from upstream";
        c.write(bytes("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" +
                      "Connection: keep-alive\r\nX-Backend: yes\r\n" +
                      "Content-Length: " + body.length + "\r\n\r\n" + body));
    }
} });

const app = new App({ port: 0 });
app.proxy("/api", { host: "127.0.0.1", port: back.port });
app.start();
ok(app.port > 0, "App resolved a listen port");

/* A client that sends hop-by-hop headers and a forged X-Forwarded-For. A proxy
   that forwards raw bytes passes all of them straight to the backend. */
const raw =
    "GET /api/thing HTTP/1.1\r\n" +
    "Host: victim.example\r\n" +
    "X-Forwarded-For: 1.2.3.4\r\n" +      /* forged: must not survive */
    "Connection: keep-alive, X-Secret\r\n" +
    "X-Secret: leak\r\n" +                /* named by Connection: hop-by-hop */
    "Keep-Alive: timeout=5\r\n" +
    "Proxy-Authorization: Basic abc\r\n" +
    "Upgrade: h2c\r\n" +
    "X-Keep: kept\r\n" +                  /* an ordinary header MUST survive */
    "Content-Length: 0\r\n" +
    "\r\n";

let reply = "";
const live = [];
live.push(TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
    connect: (c, e) => { if (!e) c.write(bytes(raw)); },
    data: (c, b) => { reply += str(b); },
}));

setTimeout(() => {
    const head = seen.split("\r\n\r\n")[0] || "";
    const low = head.toLowerCase();

    ok(seen.length > 0, "the backend was reached at all");
    ok(head.indexOf("GET /thing HTTP/1.1") === 0,
       "request line re-emitted with the prefix stripped: " +
       head.split("\r\n")[0]);
    ok(low.indexOf("host: 127.0.0.1:" + back.port) >= 0,
       "Host is the UPSTREAM, not the client's victim.example");
    ok(low.indexOf("victim.example") < 0, "the client's Host did not survive");

    /* hop-by-hop must not reach the backend */
    for (const h of ["keep-alive:", "proxy-authorization:", "upgrade:"])
        ok(low.indexOf(h) < 0, "hop-by-hop stripped: " + h);
    ok(low.indexOf("connection: close") >= 0 &&
       low.indexOf("keep-alive,") < 0,
       "the client's Connection was replaced, not forwarded");

    /* the forged forwarding header must be replaced, not appended to */
    const xffs = (low.match(/x-forwarded-for:/g) || []).length;
    ok(xffs === 1, "exactly one X-Forwarded-For, got " + xffs);
    ok(low.indexOf("1.2.3.4") < 0, "the client's forged X-Forwarded-For is gone");
    ok(low.indexOf("via: 1.1 dynajs") >= 0, "a Via record was added");

    /* an ordinary header must still be forwarded, or the filter is too broad */
    ok(head.indexOf("X-Keep: kept") >= 0, "an ordinary header survives");

    /* the response leg: backend headers reach the client, hop-by-hop do not */
    ok(reply.indexOf("200 OK") > 0, "the client got the upstream status");
    ok(reply.indexOf("hi from upstream") > 0, "the client got the body");
    ok(reply.indexOf("X-Backend: yes") > 0, "an upstream header reached the client");
    ok(reply.toLowerCase().indexOf("connection: keep-alive") < 0,
       "the upstream's hop-by-hop header was stripped on the way back");

    for (const c of live) { try { c.close(); } catch (e) {} }
    app.close();
    back.close();
    print("test_http_proxy: " + n + " assertions, " + bad + " failures");
    if (bad) throw new Error(bad + " failures");
}, 1200);
