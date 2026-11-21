// flags: --std
// covers: integration dyna:http.App dyna:http.HTTPServer dyna:http.HTTPServerAsync -- one Date writer shared by all three servers
// RFC 9110 6.6.1: an origin server with a clock MUST send Date on 2xx/3xx/4xx (MAY on 1xx/5xx).
// Every final response head from App, HTTPServer and HTTPServerAsync carries exactly one
// "Date: <IMF-fixdate>" line straight after the status line, read from a per-thread line
// rebuilt once per second (httpk_date_line; the caching itself is pinned in test_httpk_core.c)
// from the wall second each reactor reads once per wakeup (dyn_wall_tick, pinned in
// test_timer.c), so a response sent after a timer wait still carries a fresh Date.
// A Date the App handler sets wins and is the only one sent. 101 Switching Protocols gets none.
import { sections as __kitSections } from "../lib/kit.js";
import * as std from "std";
import { TCPServer } from "dyna:net";
import { App, HTTPServer, HTTPServerAsync } from "dyna:http";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
const __kitS = __kitSections("test_http_date_header", { chain: true });
let enc, dec, root, app, AP, srv, asrv;

let n = 0, fails = 0;
const ok = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const FIX = /^(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{4} \d{2}:\d{2}:\d{2} GMT$/;
const WD = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

function raw(port, req, settleMs) {
    return new Promise((resolve) => {
        let buf = "", done = false, settle = null;
        const finish = () => { if (!done) { done = true; resolve(buf); } };
        const h = TCPServer.connect({ host: "127.0.0.1", port }, {
            connect: (s) => s.write(enc.encode(req)),
            data: (s, b) => {
                buf += dec.decode(b);
                if (settleMs && buf.indexOf("\r\n\r\n") >= 0 && !settle)
                    settle = setTimeout(() => { try { h.close(); } catch (e) {} finish(); }, settleMs);
            },
            close: finish,
        });
        setTimeout(() => { try { h.close(); } catch (e) {} finish(); }, 4000);
    });
}

function headLines(resp) {
    const e = resp.indexOf("\r\n\r\n");
    return (e < 0 ? resp : resp.slice(0, e)).split("\r\n");
}

function checkDated(tag, resp, status) {
    const lines = headLines(resp);
    ok(lines[0].startsWith("HTTP/1.1 " + status + " "), tag + ": status " + status + " [" + lines[0] + "]");
    const dates = lines.filter((l) => /^date:/i.test(l));
    ok(dates.length === 1, tag + ": exactly one Date line (" + dates.length + ")");
    ok(lines[1] && lines[1].startsWith("Date: "), tag + ": Date follows the status line [" + lines[1] + "]");
    const v = dates.length ? dates[0].slice(dates[0].indexOf(":") + 1).trim() : "";
    ok(FIX.test(v), tag + ": IMF-fixdate [" + v + "]");
    const t = Date.parse(v);
    ok(Math.abs(t - Date.now()) <= 5000, tag + ": Date is now (" + v + ")");
    ok(WD[new Date(t).getUTCDay()] === v.slice(0, 3), tag + ": weekday agrees with the date [" + v + "]");
    return v;
}

const G = (p, extra) => "GET " + p + " HTTP/1.1\r\nHost: x\r\n" + (extra || "") + "Connection: close\r\n\r\n";
await __kitS.section("fixture: create the text encoder and decoder", async () => {
enc = new TextEncoder(), dec = new TextDecoder();

root = makeTempDir("datehdr");
writeFile(root.join("f.txt"), "0123456789");
app = new App({ port: 0 });
app.static("/files", root, {});
app.get("/json", () => ({ a: 1 }));
app.get("/text", () => "hi");
app.get("/created", () => ({ status: 201, body: { a: 1 }, headers: { "X-Kind": "json" } }));
app.get("/nobody", () => ({ status: 204 }));
app.get("/redir", () => ({ status: 302, headers: { Location: "/text" } }));
app.get("/userdate", () => ({ body: "u", headers: { Date: "Mon, 01 Jan 2001 00:00:00 GMT" } }));
app.get("/userdate-lc", () => ({ status: 204, headers: { "x-a": "1", date: "Tue, 02 Jan 2001 00:00:00 GMT" } }));
app.get("/xdate", () => ({ body: "x", headers: { "X-Date": "not-a-date" } }));
app.get("/fail", () => { throw new Error("boom"); });
app.get("/slow", async () => { await new Promise((r) => setTimeout(r, 1500)); return "late"; });
app.sse("/sse", { open: (c) => { c.send("x"); }, close: () => {} });
app.ws("/ws", { open: () => {}, message: () => {}, close: () => {} });
app.start();
AP = app.port;

srv = new HTTPServer({ port: 0, routes: {
    "/": "hello",
    "/json": { status: 200, contentType: "application/json", body: "{\"a\":1}" },
    "/empty": { status: 204, contentType: "text/plain", body: "" },
} });
srv.start();
asrv = new HTTPServerAsync({ port: 0, routes: { "/": "async" } });
asrv.start();
});

try {
await __kitS.section("App json fast path", async () => {
    checkDated("App json fast path", await raw(AP, G("/json")), 200);
    checkDated("App text", await raw(AP, G("/text")), 200);
    checkDated("App 201 with extra headers", await raw(AP, G("/created")), 201);
    checkDated("App 204", await raw(AP, G("/nobody")), 204);
    checkDated("App 302", await raw(AP, G("/redir")), 302);
    checkDated("App 404", await raw(AP, G("/missing")), 404);
    checkDated("App 500", await raw(AP, G("/fail")), 500);
    checkDated("App static HEAD", await raw(AP, "HEAD /files/f.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), 200);
    checkDated("App X-Date is not Date", await raw(AP, G("/xdate")), 200);
    checkDated("App static 200", await raw(AP, G("/files/f.txt")), 200);
    checkDated("App static 206", await raw(AP, G("/files/f.txt", "Range: bytes=2-4\r\n")), 206);
    checkDated("App static 416", await raw(AP, G("/files/f.txt", "Range: bytes=50-60\r\n")), 416);
    checkDated("App SSE", await raw(AP, "GET /sse HTTP/1.1\r\nHost: x\r\n\r\n", 150), 200);
});
    {
let lines, d;
await __kitS.section("App: a handler-set Date wins, sent once", async () => {
        lines = headLines(await raw(AP, G("/userdate")));
        d = lines.filter((l) => /^date:/i.test(l));
        ok(d.length === 1 && d[0] === "Date: Mon, 01 Jan 2001 00:00:00 GMT", "App: a handler-set Date wins, sent once " + JSON.stringify(d));
});
    }
    {
let lines, d;
await __kitS.section("App: a lower-case date on a 204 wins, sent once", async () => {
        lines = headLines(await raw(AP, G("/userdate-lc")));
        d = lines.filter((l) => /^date:/i.test(l));
        ok(d.length === 1 && /Tue, 02 Jan 2001 00:00:00 GMT$/.test(d[0]), "App: a lower-case date on a 204 wins, sent once " + JSON.stringify(d));
});
    }
    {
let a, b;
await __kitS.section("App same second A", async () => {
        a = checkDated("App same second A", await raw(AP, G("/json")), 200);
        b = checkDated("App same second B", await raw(AP, G("/text")), 200);
        ok(Math.abs(Date.parse(b) - Date.parse(a)) <= 1000, "App: back-to-back responses are within a second [" + a + " / " + b + "]");
});
    }
    {
let first, later;
await __kitS.section("App before tick", async () => {
        first = checkDated("App before tick", await raw(AP, G("/json")), 200);
});
await __kitS.section("App after tick", async () => {
        await new Promise((r) => setTimeout(r, 1100));
        later = checkDated("App after tick", await raw(AP, G("/json")), 200);
        ok(Date.parse(later) > Date.parse(first), "App: the cached line advances with the clock [" + first + " -> " + later + "]");
});
    }
await __kitS.section("App: a response sent after a timer wait carries a fresh Date", async () => {
    {
        const v = checkDated("App after a 1.5 s async handler", await raw(AP, G("/slow")), 200);
        ok(Math.abs(Date.parse(v) - Date.now()) <= 1000, "App: a response sent after a timer wait carries a fresh Date [" + v + "]");
    }
});
await __kitS.section("App: each pipelined response carries its own Date", async () => {
    {
        const r = await raw(AP, "GET /json HTTP/1.1\r\nHost: x\r\n\r\nGET /text HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        ok((r.match(/\r\nDate: /g) || []).length === 2, "App: each pipelined response carries its own Date");
    }
});
    {
let r;
await __kitS.section("ws upgrade answers 101", async () => {
        r = await raw(AP, "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n" +
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 150);
        ok(r.startsWith("HTTP/1.1 101"), "ws upgrade answers 101 [" + r.slice(0, 40) + "]");
        ok(!/\r\ndate:/i.test(r.slice(0, r.indexOf("\r\n\r\n"))), "101 carries no Date");
});
    }

await __kitS.section("HTTPServer route", async () => {
    checkDated("HTTPServer route", await raw(srv.port, G("/")), 200);
    checkDated("HTTPServer json", await raw(srv.port, G("/json")), 200);
    checkDated("HTTPServer 204", await raw(srv.port, G("/empty")), 204);
    checkDated("HTTPServer 404", await raw(srv.port, G("/nope")), 404);
    checkDated("HTTPServer 400", await raw(srv.port, "GET / HTTP/1.1\r\n\r\n"), 400);
    checkDated("HTTPServer keep-alive text fast path", await raw(srv.port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 100), 200);
    checkDated("HTTPServerAsync route", await raw(asrv.port, G("/")), 200);
    checkDated("HTTPServerAsync 404", await raw(asrv.port, G("/nope")), 404);
});
} finally { if (!__kitS.listing) {
    app.close();
    srv.stop ? srv.stop() : srv.close();
    asrv.stop ? asrv.stop() : asrv.close();
    removeAll(root);
} }
__kitS.end();
if (!__kitS.listing) {

print("test_http_date_header: " + (n - fails) + " passed, " + fails + " failed");
if (fails) std.exit(1);
}
