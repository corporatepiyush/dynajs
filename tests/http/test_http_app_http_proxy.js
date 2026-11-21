// covers: dyna:http.App
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { suite } from "../lib/kit.js";

// The backend and the proxying App start, and both raw clients are answered,
// in the kit's before hook (never under --list); each case checks one side.
const t = suite("test_http_proxy");
let A = null;
const ok = (c, what) => A.ok(c, what);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
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

let seen = "", back = null, app = null, reply = "", badReply = "";
const live = [];
t.before(async () => {
back = new TCPServer({ port: 0 });
back.start({ data: (c, b) => {
    seen += str(b);
    if (seen.indexOf("\r\n\r\n") >= 0) {
        const body = "hi from upstream";
        c.write(bytes("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" +
                      "Connection: keep-alive\r\nX-Backend: yes\r\n" +
                      "Content-Length: " + body.length + "\r\n\r\n" + body));
    }
} });

app = new App({ port: 0 });
app.proxy("/api", { host: "127.0.0.1", port: back.port });
app.start();

const raw =
    "GET /api/thing HTTP/1.1\r\n" +
    "Host: victim.example\r\n" +
    "X-Forwarded-For: 1.2.3.4\r\n" +
    "Connection: keep-alive, X-Secret\r\n" +
    "X-Secret: leak\r\n" +
    "Keep-Alive: timeout=5\r\n" +
    "Proxy-Authorization: Basic abc\r\n" +
    "Upgrade: h2c\r\n" +
    "X-Keep: kept\r\n" +
    "Content-Length: 0\r\n" +
    "\r\n";

live.push(TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
    connect: (c, e) => { if (!e) c.write(bytes(raw)); },
    data: (c, b) => { reply += str(b); },
}));

live.push(TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
    connect: (c, e) => {
        if (!e) c.write(bytes("GET /api/thing HTTP/1.1\r\n" +
                              "Host: x\r\n" +
                              "X(Bad): 1\r\n" +
                              "Content-Length: 0\r\n\r\n"));
    },
    data: (c, b) => { badReply += str(b); },
}));

await new Promise((r) => setTimeout(r, 1200));
});
t.after(() => {
    for (const c of live) { try { c.close(); } catch (e) {} }
    if (app) app.close();
    if (back) back.close();
});
const upstreamHead = () => seen.split("\r\n\r\n")[0] || "";

test("the App resolves a port and reaches the backend", () => {
    ok(app.port > 0, "App resolved a listen port");
    ok(seen.length > 0, "the backend was reached at all");
});

test("the request line and Host are rewritten for the upstream", () => {
    const head = upstreamHead();
    const low = head.toLowerCase();
    ok(head.indexOf("GET /thing HTTP/1.1") === 0,
       "request line re-emitted with the prefix stripped: " +
       head.split("\r\n")[0]);
    ok(low.indexOf("host: 127.0.0.1:" + back.port) >= 0,
       "Host is the UPSTREAM, not the client's victim.example");
    ok(low.indexOf("victim.example") < 0, "the client's Host did not survive");
});

test("hop-by-hop headers are stripped and Connection replaced", () => {
    const head = upstreamHead();
    const low = head.toLowerCase();
    for (const h of ["keep-alive:", "proxy-authorization:", "upgrade:"])
        ok(low.indexOf(h) < 0, "hop-by-hop stripped: " + h);
    ok(low.indexOf("connection: close") >= 0 &&
       low.indexOf("keep-alive,") < 0,
       "the client's Connection was replaced, not forwarded");
});

test("X-Forwarded-For is rebuilt, Via added, ordinary headers kept", () => {
    const head = upstreamHead();
    const low = head.toLowerCase();
    const xffs = (low.match(/x-forwarded-for:/g) || []).length;
    ok(xffs === 1, "exactly one X-Forwarded-For, got " + xffs);
    ok(low.indexOf("1.2.3.4") < 0, "the client's forged X-Forwarded-For is gone");
    ok(low.indexOf("via: 1.1 dynajs") >= 0, "a Via record was added");
    ok(head.indexOf("X-Keep: kept") >= 0, "an ordinary header survives");
});

test("a non-token header name is a 400 and never reaches the backend", () => {
    ok(badReply.indexOf("HTTP/1.1 400") === 0,
       "a header name that is not a token is a 400 (got " +
       JSON.stringify(badReply.slice(0, 40)) + ")");
    ok(seen.indexOf("X(Bad)") < 0,
       "the non-token name never reached the backend");
});

test("the client gets the upstream response without its hop-by-hop headers", () => {
    ok(reply.indexOf("200 OK") > 0, "the client got the upstream status");
    ok(reply.indexOf("hi from upstream") > 0, "the client got the body");
    ok(reply.indexOf("X-Backend: yes") > 0, "an upstream header reached the client");
    ok(reply.toLowerCase().indexOf("connection: keep-alive") < 0,
       "the upstream's hop-by-hop header was stripped on the way back");
});

await t.run();
