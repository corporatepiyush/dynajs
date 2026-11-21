// flags: --std
// covers: dyna:http.App
import * as std from "std";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { suite } from "../lib/kit.js";

// Each case starts its own App and closes it; nothing listens under --list.
const t = suite("test_http_read_reject");
let A = null;
const check = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + "\n  got  " + JSON.stringify(a) + "\n  want " + JSON.stringify(b));

const enc = new TextEncoder(), dec = new TextDecoder();
const body = JSON.stringify({ jsonrpc: "2.0", id: 1, method: "s" });
const req = "POST /rpc HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n" +
            "Content-Length: " + body.length + "\r\n\r\n" + body;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function shot(port, waitMs, drive) {
  return new Promise((resolve) => {
    let buf = "", closed = 0, done = false;
    const finish = () => { if (!done) { done = true; resolve({ buf, closed }); } };
    const conn = TCPServer.connect({ host: "127.0.0.1", port: port }, {
      connect: (s) => {
        s.write(enc.encode(req));
        if (drive) drive(() => buf, () => { try { conn.close(); } catch (e) {} });
      },
      data: (s, b) => { buf += dec.decode(b); },
      close: () => { closed++; finish(); },
    });
    setTimeout(() => { try { conn.close(); } catch (e) {} finish(); }, waitMs);
  });
}

function trunc(r, label) {
  check(/HTTP\/1\.1 200 OK/.test(r.buf), label + ": status line went out");
  check(/Transfer-Encoding: chunked/.test(r.buf), label + ": chunked head out");
  check(r.buf.indexOf("0\r\n\r\n") < 0, label + ": no terminal chunk (truncated)");
  check(r.closed >= 1, label + ": server closed the truncated conn");
}


test("R1 a sync-throwing read truncates the chunked response and closes", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { throw new Error("sync throw"); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "R1 sync-throwing read");
    app.close();
});

test("R2 an already-rejected read truncates the chunked response and closes", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { return Promise.reject(new Error("read failed")); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "R2 already-rejected read");
    app.close();
});

test("R3 a deferred-rejecting read truncates the chunked response and closes", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          return new Promise((res, rej) =>
            setTimeout(() => rej(new Error("deferred")), 15));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    trunc(r, "R3 deferred-rejecting read");
    app.close();
});

test("R4 a partial chunk lands before the reject and no read follows it", async () => {
    const app = new App({ port: 0 });
    let calls = 0;
    app.rpc("/rpc", {
      s: () => ({ read(buf) {
          calls++;
          if (calls > 1)
            return Promise.reject(new Error("second read"));
          return new Promise((res) => setTimeout(() => {
            buf.set(enc.encode("ONE-"), 0);
            res(4);
          }, 10));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 600);
    check(/4\r\nONE-\r\n/.test(r.buf), "R4: partial chunk landed before the reject");
    check(r.buf.indexOf("0\r\n\r\n") < 0, "R4: truncated after the partial chunk");
    eq(calls, 2, "R4: no read issued after the rejection");
    app.close();
});

test("R5 back-to-back rejecting streams each read exactly once", async () => {
    const app = new App({ port: 0 });
    let calls = 0;
    app.rpc("/rpc", {
      s: () => ({ read() {
          calls++;
          return Promise.reject(new Error("r" + calls));
        }, close() {} }),
    });
    app.start();
    const r1 = await shot(app.port, 400);
    trunc(r1, "R5 first rejecting stream");
    eq(calls, 1, "R5: no back-to-back read after the first rejection");
    const r2 = await shot(app.port, 400);
    trunc(r2, "R5 second rejecting stream (back-to-back)");
    eq(calls, 2, "R5: second stream read exactly once");
    app.close();
});

test("R6 a reject after the client closed is survived", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          return new Promise((res, rej) =>
            setTimeout(() => rej(new Error("after close")), 40));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 200, (getBuf, closeConn) =>
      setTimeout(closeConn, 15));
    await sleep(120);
    check(true, "R6: reject-after-client-close survived");
    app.close();
});

test("R7 a reject after the stream was abandoned is survived", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return new Promise((res, rej) =>
            setTimeout(() => rej(new Error("late")), 5));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    await sleep(120);
    check(true, "R7: reject-after-abandon survived");
});

test("R8 an abandoned and collected rejected read is survived", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          const p = Promise.reject(new Error("dropped"));
          app.close();
          return p;
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    for (let i = 0; i < 5; i++) { std.gc(); await sleep(10); }
    check(true, "R8: abandoned+GC'd rejected read survived");
});

test("R9 a pending abandoned read rejecting after GC is survived", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return new Promise((res, rej) =>
            setTimeout(() => rej(new Error("gc late")), 30));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    for (let i = 0; i < 8; i++) { std.gc(); await sleep(15); }
    check(true, "R9: pending abandoned read rejecting after GC survived");
});

test("R10 a reject after app.close is survived", async () => {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          return new Promise((res, rej) =>
            setTimeout(() => rej(new Error("post close")), 50));
        }, close() {} }),
    });
    app.start();
    await shot(app.port, 250, () => setTimeout(() => app.close(), 20));
    await sleep(150);
    check(true, "R10: reject-after-app.close survived");
});

await t.run();
std.gc();
