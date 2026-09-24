// flags: --std
/* test_http_read_reject.js -- read() promise failures and drops: a REJECTING
 * read truncates the body and the PROCESS SURVIVES; a read promise the pump
 * abandons (already rejected, or rejected after a close during its call) has
 * its reactions attached before the drop, so its late rejection is swallowed
 * deliberately and can NEVER surface as an unhandled rejection (which the
 * engine's loop-boundary policy punishes with exit(1) for the whole
 * process).
 *
 * Rows: sync-throwing read; already-rejected read (`async read(){throw}` /
 * `Promise.reject`); a parked read rejecting later; a read whose promise
 * rejects after the CLIENT conn closed; after app.close() while parked;
 * close DURING the read call with the promise rejecting afterwards (the
 * abandon shape); the same with the dropped promise garbage-collected under
 * pressure; and back-to-back reads where the first rejects (the pump issues
 * no further read). Every row asserts the documented truncation (the
 * chunked head out, NO terminal chunk, connection closed) where a response
 * started, and the process reaching the summary is the exit-clean
 * assertion for every shape: rc 0, no unhandled-rejection kill.
 */
import * as std from "std";
import { App, TCPServer } from "dyna:net";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + "\n  got  " + JSON.stringify(a) + "\n  want " + JSON.stringify(b));

const enc = new TextEncoder(), dec = new TextDecoder();
const body = JSON.stringify({ jsonrpc: "2.0", id: 1, method: "s" });
const req = "POST /rpc HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n" +
            "Content-Length: " + body.length + "\r\n\r\n" + body;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* One request on a raw conn; resolves {buf, closed} at `waitMs` (the row's
 * survival proof) or earlier on server close. */
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

/* The documented truncation for a read failure: the chunked head is out,
 * nothing of a terminal chunk followed, and the server closed the conn. */
function trunc(r, label) {
  check(/HTTP\/1\.1 200 OK/.test(r.buf), label + ": status line went out");
  check(/Transfer-Encoding: chunked/.test(r.buf), label + ": chunked head out");
  check(r.buf.indexOf("0\r\n\r\n") < 0, label + ": no terminal chunk (truncated)");
  check(r.closed >= 1, label + ": server closed the truncated conn");
}

(async () => {
  /* R1: read() THROWS synchronously. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { throw new Error("sync throw"); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "R1 sync-throwing read");
    app.close();
  }

  /* R2: read() returns an ALREADY-REJECTED promise (`async read(){throw}`
   * is this exact shape). */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { return Promise.reject(new Error("read failed")); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "R2 already-rejected read");
    app.close();
  }

  /* R3: a parked read whose promise rejects LATER (deferred reject). */
  {
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
  }

  /* R4: a partial chunk, THEN the next read rejects: truncate AFTER the
   * chunk (the contract's mid-stream truncation). */
  {
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
  }

  /* R5: reject-then-back-to-back: the FIRST read already rejects -- the
   * pump must issue NO further read, and a second connection with the same
   * shape right behind it (back-to-back streams) truncates the same way. */
  {
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
  }

  /* R6: reject-after-close -- the CLIENT closes while the read is parked;
   * the promise rejects afterwards. Swallowed, never unhandled. */
  {
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
    await sleep(120);            /* the late rejection must land quietly */
    check(true, "R6: reject-after-client-close survived");
    app.close();
  }

  /* R7: reject-after-abandon -- read() closes the app (conn dies under it)
   * and returns a promise that rejects LATER: the pump abandons the read
   * promise; the late rejection must be swallowed. */
  {
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
  }

  /* R8: the abandoned promise is ALREADY rejected at the drop (close during
   * the read call + Promise.reject) and is then DROPPED BY GC. */
  {
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
  }

  /* R9: the abandoned promise is PENDING at the drop, unreferenced, and its
   * rejection fires after GC pressure (dropped-by-GC, pending shape). */
  {
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
  }

  /* R10: app.close() AFTER the read parked (not during the call), promise
   * rejects afterwards. */
  {
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
  }

  print("test_http_read_reject: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
  std.gc();
})();
