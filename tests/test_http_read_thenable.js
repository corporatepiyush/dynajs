// flags: --std
/* test_http_read_thenable.js -- read() results whose `then` is hostile or
 * lazy: the pump must consume the promise UNDERNEATH, never the trap.
 *
 * The attach rule every consumption site follows (the park, both abandon
 * shapes, the drop pair): a GENUINE promise (Promise subclass instances
 * included) is attached through Promise.prototype.then.call -- its own
 * `then` is never consulted, so a subclass whose then() throws can neither
 * block the attach nor surface its rejection as an unhandled rejection
 * (the engine's loop-boundary policy exit(1)s the process over one). A
 * non-promise thenable has no internal reaction list and genuinely needs
 * its own `then` invoked -- that fallback is kept for it (and is the last
 * resort when every intrinsic route threw), and a `then` that throws
 * synchronously is swallowed: the pair it is handed is once-only (a call
 * before the throw stays the winner), and a thenable carries no
 * unhandled-rejection tracking.
 *
 * Rows: Promise subclass with a throwing own `then` in every shape --
 * already-rejected (abandon), dropped mid-call (abandon), parked then
 * rejected later, parked then resolved later, resolved inline, dropped
 * then resolved late, parked then aborted by app.close() and rejected
 * after; poisoned `constructor` (the intrinsic route's only failure) with
 * and without a hostile own `then`; plain resolved/rejected promises;
 * async-function returns (resolve + throw); non-promise thenables with a
 * throwing `then`, with a `then` that calls the pair twice (inert after
 * the first -- on the drop pair AND on the park's capability pair, where
 * the once-only rule is observable) and with a `then` that never calls
 * back (on the drop path it is consulted and simply ignored -- nothing
 * waits on it; on the park the documented HOLD is the underlying
 * promise's settlement, never the own `then`).
 *
 * Documented boundaries (not rows): a read promise that NEVER settles
 * holds the pump for the stream's lifetime (that IS the hold design) and
 * pins the pump state until process teardown -- the pre-existing
 * teardown-pin defect, unchanged here. A promise that defeats both
 * Promise.prototype.then (poisoned `constructor`) AND a shadow define
 * (sealed) AND carries a hostile `then` has no attachable reaction at all
 * if it rejects -- the same is true of the engine's own `await` -- and is
 * exercised here only in shapes that cannot reject.
 *
 * Every row asserts the documented contract -- the chunked head out, the
 * right chunks (or none), and NO terminal chunk where a read failed (a
 * chunked body without its terminal IS the failure signal) -- and the
 * process reaching the summary is the exit-clean assertion for every
 * shape: rc 0, never an unhandled-rejection kill.
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
 * no data chunk followed, no terminal chunk, and the server closed. */
function trunc(r, label) {
  check(/HTTP\/1\.1 200 OK/.test(r.buf), label + ": status line went out");
  check(/Transfer-Encoding: chunked/.test(r.buf), label + ": chunked head out");
  check(r.buf.indexOf("0\r\n\r\n") < 0, label + ": no terminal chunk (truncated)");
  check(r.closed >= 1, label + ": server closed the truncated conn");
}

/* A hostile own `then`: a REAL promise (Promise subclass) whose then()
 * throws. Attaching through it must never be attempted. */
class HP extends Promise {
  then() { throw new Error("hostile own then"); }
}

(async () => {
  /* H1: hostile own `then`, ALREADY REJECTED at read() time (the
   * already-rejected abandon shape). Reactions land via
   * Promise.prototype.then.call; the rejection is swallowed. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { return HP.reject(new Error("h1")); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "H1 hostile-then already-rejected read");
    app.close();
  }

  /* H2: hostile own `then`, DROPPED mid-call (read() closes the app) and
   * already rejected: the drop pair must still land underneath. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          const p = HP.reject(new Error("h2 dropped"));
          app.close();
          return p;
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 400);
    check(r.buf.indexOf("0\r\n\r\n") < 0, "H2: no terminal chunk");
    await sleep(80);
    check(true, "H2: dropped hostile-then rejection survived");
    try { app.close(); } catch (e) {}
  }

  /* H3: hostile own `then`, dropped mid-call, PENDING then rejecting LATE.
   * The late rejection must be swallowed by the landed pair. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return new HP((res, rej) => setTimeout(() => rej(new Error("h3 late")), 10));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    check(r.buf.indexOf("0\r\n\r\n") < 0, "H3: no terminal chunk");
    await sleep(120);
    check(true, "H3: late rejection of a dropped hostile-then read survived");
    try { app.close(); } catch (e) {}
  }

  /* H4: hostile own `then`, dropped mid-call, resolving LATE: the late
   * VALUE is dropped by the noop, the process survives. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return new HP((res) => setTimeout(() => res(4), 10));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    check(r.buf.indexOf("0\r\n\r\n") < 0, "H4: no terminal chunk");
    await sleep(120);
    check(true, "H4: late value of a dropped hostile-then read survived");
    try { app.close(); } catch (e) {}
  }

  /* H5: hostile own `then`, PARKED then rejected later (the t3 shape).
   * The park attaches underneath; the late rejection truncates and is
   * swallowed. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          return new HP((res, rej) => setTimeout(() => rej(new Error("h5 late")), 20));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    trunc(r, "H5 parked hostile-then read rejecting later");
    app.close();
  }

  /* H6: hostile own `then`, PARKED then RESOLVED later: the count rides
   * the underlying promise and the body completes. Under an own-`then`
   * attach this row dies (the attach throws -> truncation). */
  {
    const app = new App({ port: 0 });
    let calls = 0;
    app.rpc("/rpc", {
      s: () => ({ read(buf) {
          calls++;
          if (calls > 1) return Promise.resolve(0);
          return new HP((res) => setTimeout(() => {
            buf.set(enc.encode("ONE-"), 0);
            res(4);
          }, 20));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    check(/4\r\nONE-\r\n/.test(r.buf), "H6: chunk landed (own then bypassed)");
    check(/0\r\n\r\n/.test(r.buf), "H6: terminal chunk (body completed)");
    eq(calls, 2, "H6: pump read exactly twice");
    app.close();
  }

  /* H7: hostile own `then`, fulfilled INLINE (already settled at read()):
   * the count rides the promise result; no attach is even needed. */
  {
    const app = new App({ port: 0 });
    let calls = 0;
    app.rpc("/rpc", {
      s: () => ({ read(buf) {
          calls++;
          if (calls > 1) return HP.resolve(0);
          buf.set(enc.encode("FOUR"), 0);
          return HP.resolve(4);
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    check(/4\r\nFOUR\r\n/.test(r.buf), "H7: inline fulfilled hostile-then chunk");
    check(/0\r\n\r\n/.test(r.buf), "H7: terminal chunk");
    app.close();
  }

  /* H8: hostile own `then`, PARKED, app.close() while in flight, rejection
   * lands afterwards: swallowed, never a kill. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          return new HP((res, rej) => setTimeout(() => rej(new Error("h8 late")), 40));
        }, close() {} }),
    });
    app.start();
    await shot(app.port, 250, () => setTimeout(() => app.close(), 20));
    await sleep(150);
    check(true, "H8: parked hostile-then read rejecting after app.close survived");
    try { app.close(); } catch (e) {}
  }

  /* H9: poisoned `constructor` (the intrinsic route's ONLY failure) on an
   * already-rejected read with a hostile own `then`: the attach shadows
   * the poison with the intrinsic and still lands underneath. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() {
          const p = HP.reject(new Error("h9 poisoned"));
          Object.defineProperty(p, "constructor",
            { get() { throw new Error("poisoned"); }, configurable: true });
          return p;
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "H9 poisoned-constructor already-rejected read");
    app.close();
  }

  /* H10: poisoned `constructor` + hostile own `then`, PARKED then
   * resolved: same rescue, the body completes. */
  {
    const app = new App({ port: 0 });
    let calls = 0;
    app.rpc("/rpc", {
      s: () => ({ read(buf) {
          calls++;
          if (calls > 1) return Promise.resolve(0);
          const p = new HP((res) => setTimeout(() => {
            buf.set(enc.encode("TEN-"), 0);
            res(4);
          }, 20));
          Object.defineProperty(p, "constructor",
            { get() { throw new Error("poisoned"); }, configurable: true });
          return p;
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    check(/4\r\nTEN-\r\n/.test(r.buf), "H10: poisoned-constructor park resolved");
    check(/0\r\n\r\n/.test(r.buf), "H10: terminal chunk");
    app.close();
  }

  /* P1: plain resolved promise (the workaday shape). */
  {
    const app = new App({ port: 0 });
    let calls = 0;
    app.rpc("/rpc", {
      s: () => ({ read(buf) {
          calls++;
          if (calls > 1) return Promise.resolve(0);
          buf.set(enc.encode("P1--"), 0);
          return Promise.resolve(4);
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    check(/4\r\nP1--\r\n/.test(r.buf), "P1: resolved promise chunk");
    check(/0\r\n\r\n/.test(r.buf), "P1: terminal chunk");
    app.close();
  }

  /* P2: plain rejected promise truncates and survives. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { return Promise.reject(new Error("p2")); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "P2 rejected promise read");
    app.close();
  }

  /* P3: async-function returns: resolve shape completes the body, throw
   * shape truncates -- both survive. */
  {
    const app = new App({ port: 0 });
    let calls = 0;
    app.rpc("/rpc", {
      s: () => ({ read: async function (buf) {
          calls++;
          if (calls > 1) return 0;
          buf.set(enc.encode("P3--"), 0);
          return 4;
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    check(/4\r\nP3--\r\n/.test(r.buf), "P3: async resolve chunk");
    check(/0\r\n\r\n/.test(r.buf), "P3: terminal chunk");
    app.close();
  }
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read: async function () { throw new Error("p3 throw"); },
                  close() {} }),
    });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "P3 async-throw read");
    app.close();
  }

  /* T1: NON-PROMISE thenable whose `then` throws. On the steady path a
   * non-promise read is a contract violation: truncated WITHOUT ever
   * consulting its `then`. On the drop path the fallback does consult it
   * (once) and the throw is swallowed. */
  {
    const app = new App({ port: 0 });
    let thenCalls = 0;
    app.rpc("/rpc", {
      s: () => ({ read() {
          return { then() { thenCalls++; throw new Error("t1 hostile"); } };
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "T1 thenable with throwing then (steady)");
    eq(thenCalls, 0, "T1: steady path never consulted the thenable's then");
    app.close();
  }
  {
    const app = new App({ port: 0 });
    let thenCalls = 0;
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return { then() { thenCalls++; throw new Error("t1 drop hostile"); } };
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    check(r.buf.indexOf("0\r\n\r\n") < 0, "T1: no terminal chunk (drop)");
    eq(thenCalls, 1, "T1: drop path consulted the thenable's then exactly once");
    await sleep(80);
    check(true, "T1: throwing thenable on the drop path survived");
    try { app.close(); } catch (e) {}
  }

  /* T2: thenable whose `then` calls the pair TWICE (plus a rejection and a
   * throw): inert after the first. On the drop pair every call is a noop
   * (observed: consulted once, no effect); on the park's capability pair
   * the once-only rule is observable -- and there the intrinsic routes are
   * ALL closed (poisoned `constructor`, sealed object) so the fallback's
   * own `then` receives the pair: the FIRST call wins, the rest are
   * inert. */
  {
    const app = new App({ port: 0 });
    let thenCalls = 0;
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return { then(res, rej) {
            thenCalls++;
            res(3); res(99); rej(new Error("t2 second")); res(7);
            throw new Error("t2 then threw");
          } };
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    check(r.buf.indexOf("0\r\n\r\n") < 0, "T2: no terminal chunk (drop)");
    eq(thenCalls, 1, "T2: drop path consulted the double-calling then once");
    await sleep(80);
    check(true, "T2: double-calling thenable on the drop path survived");
    try { app.close(); } catch (e) {}
  }
  {
    const app = new App({ port: 0 });
    let calls = 0;
    class DP extends Promise {
      then(res, rej) { res(3); res(99); rej(new Error("t2 second")); res(7); }
    }
    app.rpc("/rpc", {
      s: () => ({ read(buf) {
          calls++;
          if (calls > 1) return Promise.resolve(0);
          const p = new DP((res) => setTimeout(() => res(4), 30));
          Object.defineProperty(p, "constructor",
            { get() { throw new Error("poisoned"); }, configurable: false });
          Object.preventExtensions(p);   /* even the shadow is refused */
          return p;
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 500);
    eq(calls, 2, "T2: pump read exactly twice (first callback won)");
    check(/3\r\n/.test(r.buf), "T2: the FIRST callback's count was processed");
    check(!/99\r\n/.test(r.buf), "T2: second resolve inert");
    check(!/7\r\n/.test(r.buf), "T2: third resolve inert");
    check(/0\r\n\r\n/.test(r.buf), "T2: terminal chunk (no double settle)");
    await sleep(80);        /* the deferred reject lands on nothing */
    check(true, "T2: inert rejection after the first callback survived");
    app.close();
  }

  /* T3: thenable whose `then` NEVER calls back. On the drop path it is
   * consulted once and simply ignored -- nothing waits on it (the drop
   * happened before the consult). The documented HOLD is the underlying
   * promise's own settlement: a hostile `then` that never calls back
   * neither holds nor kills (it is bypassed), and the pump completes when
   * the underlying promise settles. */
  {
    const app = new App({ port: 0 });
    let thenCalls = 0;
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return { then() { thenCalls++; /* never calls back */ } };
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 300);
    check(r.buf.indexOf("0\r\n\r\n") < 0, "T3: no terminal chunk (drop)");
    eq(thenCalls, 1, "T3: drop path consulted the silent then once");
    await sleep(80);
    check(true, "T3: silent thenable on the drop path held nothing");
    try { app.close(); } catch (e) {}
  }
  {
    const app = new App({ port: 0 });
    let calls = 0;
    class NP extends Promise {
      then() { /* never calls back */ }
    }
    app.rpc("/rpc", {
      s: () => ({ read(buf) {
          calls++;
          if (calls > 1) return Promise.resolve(0);
          return new NP((res) => setTimeout(() => {
            buf.set(enc.encode("HOLD"), 0);
            res(4);
          }, 120));
        }, close() {} }),
    });
    app.start();
    const r = await shot(app.port, 600);
    eq(calls, 2, "T3: pump held until the underlying promise settled");
    check(/4\r\nHOLD\r\n/.test(r.buf), "T3: hold ended in the count (then bypassed)");
    check(/0\r\n\r\n/.test(r.buf), "T3: terminal chunk");
    app.close();
  }

  print("test_http_read_thenable: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
  std.gc();
})();
