// flags: --std
// covers: dyna:http.App
// App responses produced while one reactor event is dispatched are held per connection and
// leave in ONE send at the end of that dispatch. These cases pin what the hold must not change:
// response order, bytes before FIN on close, header-then-sendfile order, one file in flight per
// connection (the reactors refuse a second sendfile on a fd until the first completes), the partial-flush
// remainder (responses bigger than the socket buffer), and WebSocket frame order.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_http_app_coalesce");
import * as os from "os";
import * as std from "std";
import { TCPServer } from "dyna:net";
import { App, WsClient } from "dyna:http";
import { Path } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

function latin1(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return s;
}
function bytes(s) {
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
  return a;
}

function parseAll(buf) {
  const out = [];
  for (;;) {
    const he = buf.indexOf("\r\n\r\n");
    if (he < 0) break;
    const m = /Content-Length: (\d+)/i.exec(buf.slice(0, he));
    if (!m) { out.push({ bad: buf.slice(0, he) }); break; }
    const total = he + 4 + Number(m[1]);
    if (buf.length < total) break;
    out.push({ head: buf.slice(0, he), body: buf.substr(he + 4, Number(m[1])) });
    buf = buf.slice(total);
  }
  return { out, rest: buf };
}

function exchange(port, payload, want, ms = 15000) {
  return new Promise((resolve) => {
    let buf = "", closed = false, got = [], timer;
    const finish = (why) => { clearTimeout(timer); resolve({ got, closed, why, rest: buf }); };
    timer = setTimeout(() => finish("timeout"), ms);
    TCPServer.connect({ host: "127.0.0.1", port }, {
      connect: (c, err) => { if (err) { finish("connect " + err); return; } c.write(bytes(payload)); },
      data: (c, b) => {
        buf += latin1(b);
        const r = parseAll(buf);
        buf = r.rest;
        for (const x of r.out) got.push(x);
        if (want && got.length >= want) { c.close(); finish("done"); }
      },
      close: () => { closed = true; finish("closed"); },
    });
  });
}

const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_coalesce.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
os.mkdir(T, 0o755);
os.mkdir(T + "/static", 0o755);
const FILE = "F".repeat(3000) + "<end>";
{ const f = std.open(T + "/static/f.txt", "w"); f.puts(FILE); f.close(); }

const BIG = 60000;
const app = new App({ port: 0, compress: false });
app.get("/n/:i", (r) => "r" + r.params.i);
app.get("/big/:i", (r) => String(r.params.i).padEnd(BIG, "x"));
const HUGE = 900 * 1024;
app.get("/huge", () => "h".repeat(HUGE));
app.static("/s", new Path(T + "/static"));
app.ws("/ws", { message: (s, d) => { s.send("a:" + d); s.send("b:" + d); s.send("c:" + d); } });
app.start();
const port = app.port;
const get = (p, extra = "") => `GET ${p} HTTP/1.1\r\nHost: x\r\n${extra}\r\n`;

await __kitS.section("and in request order", async () => {
  const N = 32;
  let p = "";
  for (let i = 0; i < N; i++) p += get("/n/" + i);
  const r = await exchange(port, p, N);
  ok(r.got.length === N, `32 pipelined responses arrive (got ${r.got.length}, ${r.why})`);
  ok(r.got.every((x, i) => x.body === "r" + i), "and in request order");
});

await __kitS.section("Connection: closer", async () => {
  let p = "";
  for (let i = 0; i < 7; i++) p += get("/n/" + i);
  p += get("/n/7", "Connection: close\r\n");
  const r = await exchange(port, p, 0);
  ok(r.closed, "Connection: close ends the connection (" + r.why + ")");
  ok(r.got.length === 8, `every held response leaves before the FIN (got ${r.got.length})`);
  ok(r.got.length === 8 && r.got[7].body === "r7",
    "the last one is the close response");
});

await __kitS.section("GET /n/3 HTTP/1.1r Host: xr Bad Headerr r", async () => {
  const p = get("/n/0") + get("/n/1") + get("/n/2") + "GET /n/3 HTTP/1.1\r\nHost: x\r\nBad Header\r\n\r\n";
  const r = await exchange(port, p, 0);
  ok(r.closed, "a malformed request closes the connection (" + r.why + ")");
  ok(r.got.length === 4 && r.got[0].body === "r0" && r.got[2].body === "r2" && /^HTTP\/1\.1 400/.test(r.got[3].head),
    `responses held before a close inside the dispatch leave before the FIN (got ${r.got.length})`);
});

await __kitS.section("a held header is flushed before its sendfile body, in order", async () => {
  const p = get("/n/1") + get("/s/f.txt") + get("/n/2") + get("/s/f.txt") + get("/n/3");
  const r = await exchange(port, p, 5);
  ok(r.got.length === 5, `held responses and sendfile bodies interleave (got ${r.got.length}, ${r.why})`);
  ok(r.got.length === 5 && r.got[0].body === "r1" && r.got[1].body === FILE && r.got[2].body === "r2"
    && r.got[3].body === FILE && r.got[4].body === "r3", "a held header is flushed before its sendfile body, in order");
});

await __kitS.section("/s/f.txt", async () => {
  const p = get("/s/f.txt") + get("/s/f.txt") + get("/n/5") + get("/s/f.txt");
  const r = await exchange(port, p, 4);
  ok(r.got.length === 4 && r.got[0].body === FILE && r.got[1].body === FILE && r.got[2].body === "r5" && r.got[3].body === FILE,
    `pipelined static files each get their body, in order (got ${r.got.length}, ${r.why})`);
});

await __kitS.section("/huge", async () => {
  const p = get("/huge") + get("/s/f.txt") + get("/s/f.txt") + get("/n/6");
  const r = await exchange(port, p, 4, 30000);
  ok(r.got.length === 4 && r.got[0].body.length === HUGE && r.got[1].body === FILE && r.got[2].body === FILE && r.got[3].body === "r6",
    `a static file queued behind an unsent response does not refuse the next one (got ${r.got.length}, ${r.why})`);
});

await __kitS.section("/big/", async () => {
  const N = 48;
  let p = "";
  for (let i = 0; i < N; i++) p += get("/big/" + i);
  const r = await exchange(port, p, N, 30000);
  ok(r.got.length === N, `${N} x 60 KB held responses (more than a socket buffer) all arrive (got ${r.got.length}, ${r.why})`);
  let intact = 0;
  for (let i = 0; i < r.got.length; i++)
    if (r.got[i].body.length === BIG && r.got[i].body === String(i).padEnd(BIG, "x")) intact++;
  ok(intact === N, `the partial-flush remainder keeps every byte in order (intact ${intact})`);
});

await __kitS.section("a peer that resets mid-flush leaves the App serving", async () => {
  let p = "";
  for (let i = 0; i < 64; i++) p += get("/big/" + i);
  await new Promise((resolve) => {
    TCPServer.connect({ host: "127.0.0.1", port }, {
      connect: (c, err) => { if (err) { resolve(); return; } c.write(bytes(p)); setTimeout(() => { c.close(); resolve(); }, 5); },
      close: () => resolve(),
    });
  });
  const r = await exchange(port, get("/n/9"), 1);
  ok(r.got.length === 1 && r.got[0].body === "r9", "a peer that resets mid-flush leaves the App serving");
});

await __kitS.section("timeout", async () => {
  const msgs = [];
  const done = new Promise((resolve) => {
    const t = setTimeout(() => resolve("timeout"), 10000);
    const ws = new WsClient(`ws://127.0.0.1:${port}/ws`, {
      open: (w) => { w.send("1"); w.send("2"); },
      message: (w, d) => { msgs.push(String(d)); if (msgs.length === 6) { clearTimeout(t); w.close(); resolve("done"); } },
      close: () => { clearTimeout(t); resolve("closed"); },
    });
  });
  const why = await done;
  ok(msgs.join(",") === "a:1,b:1,c:1,a:2,b:2,c:2", `three sends per message stay in order (${why}: ${msgs.join(",")})`);
});
__kitS.end();

app.close();
os.remove(T + "/static/f.txt");
os.remove(T + "/static");
os.remove(T);
print(`test_http_app_coalesce: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
