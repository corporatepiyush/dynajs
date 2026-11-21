// covers: c:src/modules/http/http-app-send.c
// The head of a non-JSON, uncompressed App response used to be built by one snprintf per response
// (about 3% of a text hello-world server's CPU). It is now copied piece by piece into the head buffer,
// with snprintf kept only for a status outside 100..999 or a head that may not fit. These cases pin the exact head bytes:
// status line and reason, Date, handler headers in order (repeated names), Content-Type,
// Content-Length (0, small, past the 1 KiB single-send path), HEAD, a long header block
// (heap head buffer) and the keep-alive/close token.
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { suite } from "../lib/kit.js";

const t = suite("App response head bytes");
const latin1 = (u8) => { let s = ""; for (const b of u8) s += String.fromCharCode(b); return s; };
const bytes = (s) => new Uint8Array([...s].map((ch) => ch.charCodeAt(0)));

const LONGV = "v".repeat(300);
const app = new App({ port: 0, compress: false });
app.get("/t", () => "hello");
app.get("/e", (r) => ({ status: +r.query.s, body: "x".repeat(+r.query.n), contentType: r.query.ct, headers: { "X-A": "1", "X-B": ["p", "q"] } }));
app.get("/long", () => ({ status: 202, body: "ok", contentType: "text/plain", headers: { "X-Long": LONGV } }));
const CT256 = "text/" + "c".repeat(251), V60 = "w".repeat(52);
app.get("/maxhead", () => ({ status: 511, body: "ok", contentType: CT256, headers: { "X-W": V60 } }));
app.start();

const XH = "X-A: 1\r\nX-B: p\r\nX-B: q\r\n";
const head = (st, reason, extra, ct, len, conn) => `HTTP/1.1 ${st} ${reason}\r\nDATE${extra}Content-Type: ${ct}\r\nContent-Length: ${len}\r\nConnection: ${conn}\r\n\r\n`;
const CASES = [
    ["GET /t", head(200, "OK", "", "text/plain", 5, "keep-alive") + "hello"],
    ["GET /e?s=201&n=3&ct=text/csv", head(201, "Created", XH, "text/csv", 3, "keep-alive") + "xxx"],
    ["GET /e?s=299&n=0&ct=a/b", head(299, "OK", XH, "a/b", 0, "keep-alive")],
    ["GET /e?s=404&n=12&ct=text/html;%20charset=utf-8", head(404, "Not Found", XH, "text/html; charset=utf-8", 12, "keep-alive") + "x".repeat(12)],
    ["HEAD /e?s=200&n=5&ct=text/plain", head(405, "Method Not Allowed", "Allow: GET\r\n", "application/json", 30, "keep-alive")],
    ["GET /long", head(202, "Accepted", "X-Long: " + LONGV + "\r\n", "text/plain", 2, "keep-alive") + "ok"],
    ["GET /maxhead", head(511, "Internal Server Error", "X-W: " + V60 + "\r\n", CT256, 2, "keep-alive") + "ok"],
    ["GET /e?s=599&n=2000&ct=text/plain", head(599, "Internal Server Error", XH, "text/plain", 2000, "close") + "x".repeat(2000), "Connection: close\r\n"],
];
const req = CASES.map(([l, , h]) => l + " HTTP/1.1\r\nHost: x\r\n" + (h || "") + "\r\n").join("");

let norm = null;
async function exchange() {
    if (norm !== null) return norm;
    let buf = "";
    await new Promise((resolve) => {
        const timer = setTimeout(resolve, 15000);
        TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
            connect: (c) => c.write(bytes(req)),
            data: (c, b) => { buf += latin1(b); },
            close: () => { clearTimeout(timer); resolve(); },
        });
    });
    app.close();
    norm = buf.replace(/Date: [A-Z][a-z]{2}, \d\d [A-Z][a-z]{2} \d{4} \d\d:\d\d:\d\d GMT\r\n/g, "DATE");
    return norm;
}
let end = 0;
for (const [line, want] of CASES) {
    const at = end;
    end += want.length;
    t.test(line, async (a) => {
        const n = await exchange();
        const got = n.slice(at, at + want.length);
        a.ok(got === want, line + ":\n  want " + JSON.stringify(want.slice(0, 300)) + "\n  got  " + JSON.stringify(got.slice(0, 300)));
    });
}
t.test("no trailing bytes", async (a) => {
    const n = await exchange();
    a.eq(n.length, end, "trailing: " + JSON.stringify(n.slice(end, end + 100)));
});
await t.run();
