// flags: --std
// covers: dyna:http.App
// The Connection header an App response carries must say what the server then does: a request
// with "Connection: close" (or HTTP/1.0 without keep-alive) gets "Connection: close" and the
// connection ends after it; every other response says keep-alive and the connection stays.
// Covered for text, JSON, static file, and the 416 a static range refusal produces.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_http_app_conn_token");
import * as os from "os";
import * as std from "std";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { Path } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function latin1(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
function bytes(s) { const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff; return a; }

function exchange(port, payload, ms = 5000) {
  return new Promise((resolve) => {
    let buf = "", timer;
    const finish = (closed) => { clearTimeout(timer); resolve({ buf, closed }); };
    timer = setTimeout(() => finish(false), ms);
    TCPServer.connect({ host: "127.0.0.1", port }, {
      connect: (c, err) => { if (err) { finish(false); return; } c.write(bytes(payload)); },
      data: (c, b) => { buf += latin1(b); },
      close: () => finish(true),
    });
  });
}
const conn = (head) => { const m = /\r\nConnection: ([^\r]*)\r\n/i.exec(head); return m ? m[1] : null; };

const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_conntok.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
os.mkdir(T, 0o755);
{ const f = std.open(T + "/f.txt", "w"); f.puts("0123456789"); f.close(); }

const app = new App({ port: 0, compress: false });
app.get("/t", () => "text");
app.get("/j", () => ({ a: 1 }));
app.static("/s", new Path(T));
app.start();
const port = app.port;

const cases = [
  ["text", "/t", ""],
  ["json", "/j", ""],
  ["static file", "/s/f.txt", ""],
  ["static 416", "/s/f.txt", "Range: bytes=500-600\r\n"],
];
for (const [name, path, extra] of cases) {
  const c = await exchange(port, `GET ${path} HTTP/1.1\r\nHost: x\r\n${extra}Connection: close\r\n\r\n`);
  ok(c.buf.startsWith("HTTP/1.1 "), `${name}: a response arrives (${JSON.stringify(c.buf.slice(0, 20))})`);
  ok(conn(c.buf) === "close", `${name}: a Connection: close request is answered Connection: close (got ${conn(c.buf)})`);
  ok(c.closed, `${name}: and the connection then ends`);

  const k = await exchange(port, `GET ${path} HTTP/1.1\r\nHost: x\r\n${extra}\r\n`, 400);
  ok(conn(k.buf) === "keep-alive", `${name}: a keep-alive request is answered keep-alive (got ${conn(k.buf)})`);
  ok(!k.closed, `${name}: and the connection stays open`);
}

await __kitS.section("GET /j HTTP/1.0r Host: xr r", async () => {
  const c = await exchange(port, `GET /j HTTP/1.0\r\nHost: x\r\n\r\n`);
  ok(conn(c.buf) === "close" && c.closed, `HTTP/1.0 without keep-alive is answered close and closed (got ${conn(c.buf)}, closed ${c.closed})`);
});
__kitS.end();

app.close();
os.remove(T + "/f.txt");
os.remove(T);
print(`test_http_app_conn_token: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
