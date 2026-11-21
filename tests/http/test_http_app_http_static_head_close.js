// flags: --std
// timeout: 60
// covers: dyna:http.App
// App.static answers HEAD with the head only and must honour "Connection: close" afterwards. The
// head-only branch completed its write with the keep-alive callback (no close_after check), so a
// HEAD ... Connection: close left the socket open and a client reading to EOF waited forever.
// Each request here gets 3 s to see EOF; GET is the control (it always closed).
// A keep-alive response completing while the NEXT pipelined response (Connection: close) is still in
// sendfile must not close the connection: dyn_app_out_last closed on close_after alone, cutting the
// file body short (a keep-alive route GET then a 200 kB static GET lost the whole body).
import * as std from "std";
import * as os from "os";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { makeTempDir, writeFile, removeAll, realPath } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_http_static_head_close");
let A = null;
const ok = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);

let dir = null, app = null;
t.before(() => {
  dir = realPath(makeTempDir("sthead"));
  writeFile(dir.join("small.txt"), "x".repeat(100));
  writeFile(dir.join("big.txt"), "y".repeat(200000));
  app = new App({ port: 0, compress: false });
  app.static("/s", dir, {});
  app.get("/r", () => "route-body");
  app.start();
});
t.after(() => {
  if (app) app.close();
  if (dir) removeAll(dir);
});

function raw(payload) {
  return new Promise((resolve) => {
    const parts = [];
    let closed = false;
    const timer = os.setTimeout(() => resolve({ text: parts.join(""), closed }), 3000);
    TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
      connect: (c) => { if (c) c.write(payload); else resolve({ text: "", closed: false }); },
      data: (c, b) => { for (let i = 0; i < b.length; i += 8192) parts.push(String.fromCharCode(...b.subarray(i, i + 8192))); },
      close: () => { closed = true; os.clearTimeout(timer); resolve({ text: parts.join(""), closed }); },
    });
  });
}

for (const f of ["small.txt", "big.txt"]) {
  const n = f === "small.txt" ? 100 : 200000;
  for (const m of ["HEAD", "GET"]) test(`${m} ${f} with Connection: close gets its head, its body and EOF`, async () => {
    const r = await raw(`${m} /s/${f} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
    const e = r.text.indexOf("\r\n\r\n");
    ok(r.closed, `${m} ${f} with Connection: close: server closed the connection`);
    ok(/^HTTP\/1\.1 200 /.test(r.text) && new RegExp("content-length: " + n + "\\r?$", "im").test(r.text.slice(0, e)), `${m} ${f}: 200 with Content-Length ${n}`);
    ok(e >= 0 && r.text.length - e - 4 === (m === "HEAD" ? 0 : n), `${m} ${f}: body length ${e >= 0 ? r.text.length - e - 4 : "?"}`);
  });
  test(`keep-alive route GET then static GET ${f} (Connection: close) keeps the whole body`, async () => {
  const g = await raw(`GET /r HTTP/1.1\r\nHost: x\r\n\r\nGET /s/${f} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
  ok(g.closed && g.text.includes("route-body") && g.text.endsWith(f === "small.txt" ? "x".repeat(100) : "y".repeat(200000)), `keep-alive route GET then static GET ${f} (Connection: close) on one connection: ${g.text.length} bytes`);
  });
  test(`keep-alive HEAD then GET ${f} on one connection`, async () => {
  const k = await raw(`HEAD /s/${f} HTTP/1.1\r\nHost: x\r\n\r\nGET /s/${f} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
  ok(k.closed && k.text.split("HTTP/1.1 200").length === 3 && k.text.endsWith(f === "small.txt" ? "x".repeat(100) : "y".repeat(200000)), `keep-alive HEAD then GET ${f} on one connection`);
  });
}

await t.run();
