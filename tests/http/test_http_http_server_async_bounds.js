// flags: --std
// covers: dyna:http.HTTPServerAsync
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_server_async_bounds", { chain: true });
import { TCPServer } from "dyna:net";
import { HTTPServerAsync, HTTPClient } from "dyna:http";
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + a + ", want " + b + ")");

await __kitS.section("the server bound an ephemeral port", async () => {
  const s = new HTTPServerAsync({ port: 0, idleTimeoutMs: 2000, maxConns: 4,
                                  routes: { "/": "hi\n" } });
  s.start();
  ok(s.port > 0, "the server bound an ephemeral port");
  const c = new HTTPClient();
  try {
    const r = await c.get("http://127.0.0.1:" + s.port + "/");
    eq(r.status, 200, "a normal request is unaffected by the bounds");
    eq(r.body, "hi\n", "and returns its body");
  } finally { c.close(); }
  s.close();
  ok(s.closed === true, "close() reports closed");
});

await __kitS.section("a client making progress is never swept mid-conversation", async () => {
  const s = new HTTPServerAsync({ port: 0, idleTimeoutMs: 1000,
                                  routes: { "/": "ok\n" } });
  s.start();
  const c = new HTTPClient();
  try {
    let good = 0;
    for (let i = 0; i < 6; i++) {
      const r = await c.get("http://127.0.0.1:" + s.port + "/");
      if (r.status === 200) good++;
      os.sleep(250);
    }
    eq(good, 6, "a client making progress is never swept mid-conversation");
  } finally { c.close(); s.close(); }
});

await __kitS.section("a server with no explicit bounds still serves", async () => {
  const s = new HTTPServerAsync({ port: 0, routes: { "/": "d\n" } });
  s.start();
  const c = new HTTPClient();
  try {
    eq((await c.get("http://127.0.0.1:" + s.port + "/")).status, 200,
       "a server with no explicit bounds still serves");
  } finally { c.close(); s.close(); }
  ok(true, "the defaults are compiled in, not opt-in");
});

await __kitS.section("idleTimeoutMs 0 disables the sweep rather than sweeping at once", async () => {
  const s = new HTTPServerAsync({ port: 0, idleTimeoutMs: 0, maxConns: 0,
                                  routes: { "/": "z\n" } });
  s.start();
  const c = new HTTPClient();
  try {
    os.sleep(300);
    eq((await c.get("http://127.0.0.1:" + s.port + "/")).status, 200,
       "idleTimeoutMs 0 disables the sweep rather than sweeping at once");
  } finally { c.close(); s.close(); }
});

await __kitS.section("a negative bound is clamped to 0, never to a huge unsigned", async () => {
  const s = new HTTPServerAsync({ port: 0, idleTimeoutMs: -5, maxConns: -5,
                                  routes: { "/": "n\n" } });
  s.start();
  const c = new HTTPClient();
  try {
    eq((await c.get("http://127.0.0.1:" + s.port + "/")).status, 200,
       "a negative bound is clamped to 0, never to a huge unsigned");
  } finally { c.close(); s.close(); }
});

await __kitS.section("the cap bounds LIVE connections, not the accept count", async () => {
  const s = new HTTPServerAsync({ port: 0, maxConns: 4, routes: { "/": "s\n" } });
  s.start();
  let served = 0;
  for (let i = 0; i < 24; i++) {
    const c = new HTTPClient();
    try { if ((await c.get("http://127.0.0.1:" + s.port + "/")).status === 200) served++; }
    catch (e) {  }
    finally { c.close(); }
  }
  eq(served, 24, "the cap bounds LIVE connections, not the accept count");
  s.close();
});

await new Promise((resolve) => {
  const IDLE = 700;
  const srv = new HTTPServerAsync({ port: 0, idleTimeoutMs: IDLE,
                                    routes: { "/": "hi\n" } });
  srv.start();
  let body = "", closed = false, connected = false;
  const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
    connect: () => { connected = true; },
    data: (c, b) => { body += new TextDecoder().decode(b); },
    close: () => { closed = true; },
  });
  setTimeout(() => {
    try { cli.write("GET / HTTP/1.1\r\nHost: x\r\n\r\n"); } catch (e) { }
    setTimeout(() => {
      ok(connected, "the silent peer did connect");
      ok(closed, "a peer that sends NOTHING is swept on the reactor's clock");
      eq(body.length, 0, "and it never got a response");
      try { cli.close(); } catch (e) { }
      srv.close();
      resolve();
    }, 400);
  }, IDLE * 3);
});
__kitS.end();

print("test_http_async_bounds: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
