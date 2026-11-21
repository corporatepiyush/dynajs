// flags: --std
// timeout: 60
// covers: dyna:http.App
// App.static sends a body of up to 16 KiB (the whole file, or the requested range) in the same write
// as the response head, read with one pread; larger bodies keep the sendfile path (splice through a
// pipe on io_uring). Both must be byte-identical: every size around the cut (0, 1, 16383, 16384,
// 16385, 64 KiB), ranges that end inside, at and past the cut, HEAD (no body), and pipelined
// keep-alive requests mixing both paths on one connection, ending with Connection: close.
// HEAD with Connection: close must close the connection after the head: the head-only branch sent
// it with the keep-alive completion (no close_after check), so the client waited forever.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_http_static_inline");
import * as std from "std";
import * as os from "os";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { makeTempDir, writeFile, removeAll, realPath } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };

const dir = realPath(makeTempDir("stinline"));
const sizes = [0, 1, 16383, 16384, 16385, 65536];
const bodies = {};
for (const n of sizes) {
  let s = "";
  for (let i = 0; i < n; i++) s += String.fromCharCode(97 + ((i * 7 + (i >> 9)) % 26));
  bodies[n] = s;
  writeFile(dir.join("f" + n + ".txt"), s);
}

const app = new App({ port: 0, compress: false });
app.static("/s", dir, {});
app.start();

function raw(payload) {
  return new Promise((resolve) => {
    const parts = [];
    const timer = os.setTimeout(() => resolve(parts.join("") + "\u0000timeout"), 3000);
    TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
      connect: (c) => { if (c) c.write(payload); else resolve(""); },
      data: (c, b) => { for (let i = 0; i < b.length; i += 8192) parts.push(String.fromCharCode(...b.subarray(i, i + 8192))); },
      close: () => { os.clearTimeout(timer); resolve(parts.join("")); },
    });
  });
}

function responses(buf, heads) {
  const out = [];
  let p = 0;
  for (let k = 0; k < heads.length; k++) {
    const e = buf.indexOf("\r\n\r\n", p);
    if (e < 0) break;
    const head = buf.slice(p, e);
    const m = /content-length:\s*(\d+)/i.exec(head);
    const len = heads[k] ? 0 : (m ? +m[1] : 0);
    out.push({ status: +head.slice(9, 12), head, body: buf.slice(e + 4, e + 4 + len) });
    p = e + 4 + len;
  }
  out.rest = buf.slice(p);
  return out;
}

const req = (path, extra = "", method = "GET", close = true) =>
  `${method} ${path} HTTP/1.1\r\nHost: x\r\n${extra}${close ? "Connection: close\r\n" : ""}\r\n`;

for (const n of sizes) {
  const [r] = responses(await raw(req("/s/f" + n + ".txt")), [false]);
  ok(r && r.status === 200 && r.body === bodies[n], "GET " + n + " bytes: status " + (r && r.status) + ", body " + (r && r.body.length));
  ok(r && new RegExp("content-length: " + n + "\\r?$", "im").test(r.head), "GET " + n + ": Content-Length " + n);
  const hb = await raw(req("/s/f" + n + ".txt", "", "HEAD"));
  const [h] = responses(hb, [true]);
  ok(!hb.endsWith("\u0000timeout") && responses(hb, [true]).rest === "", "HEAD " + n + ": no body, connection closed after the head (Connection: close)");
  ok(h && h.status === 200 && new RegExp("content-length: " + n + "\\r?$", "im").test(h.head), "HEAD " + n + ": head only, Content-Length " + n);
}

const big = bodies[65536];
for (const [a, b] of [[0, 0], [5, 16388], [100, 16483], [100, 16484], [16380, 16390], [65000, 65535], [0, 65535], [40000, 56383]]) {
  const [r] = responses(await raw(req("/s/f65536.txt", `Range: bytes=${a}-${b}\r\n`)), [false]);
  ok(r && r.status === 206 && r.body === big.slice(a, b + 1), `range ${a}-${b} (${b - a + 1} bytes): got ${r && r.status} ${r && r.body.length}`);
  ok(r && r.head.includes(`Content-Range: bytes ${a}-${b}/65536`), `range ${a}-${b}: Content-Range`);
}
await __kitS.section("Range: bytes=-3r", async () => {
  const [r] = responses(await raw(req("/s/f16385.txt", "Range: bytes=-3\r\n")), [false]);
  ok(r && r.status === 206 && r.body === bodies[16385].slice(-3), "suffix range on 16385");
});

await __kitS.section("pipelined:", async () => {
  const order = [1, 65536, 16384, 0, 16385, 16383, 1];
  let payload = "";
  order.forEach((n, i) => { payload += req("/s/f" + n + ".txt", "", "GET", i === order.length - 1); });
  const rs = responses(await raw(payload), order.map(() => false));
  ok(rs.length === order.length, "pipelined: " + rs.length + " of " + order.length + " responses");
  order.forEach((n, i) => ok(rs[i] && rs[i].status === 200 && rs[i].body === bodies[n], "pipelined #" + i + " (" + n + " bytes) intact and in order"));
  ok(rs.rest === "", "pipelined: nothing after the last response (" + rs.rest.length + " bytes)");
});
__kitS.end();

app.close();
removeAll(dir);
print(`test_http_static_inline: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
