// flags: --std
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

class HP extends Promise {
  then() { throw new Error("hostile own then"); }
}

(async () => {
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { return HP.reject(new Error("h1")); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "H1 hostile-then already-rejected read");
    app.close();
  }

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

  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => ({ read() { return Promise.reject(new Error("p2")); },
                                  close() {} }) });
    app.start();
    const r = await shot(app.port, 400);
    trunc(r, "P2 rejected promise read");
    app.close();
  }

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
          Object.preventExtensions(p);
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
    await sleep(80);
    check(true, "T2: inert rejection after the first callback survived");
    app.close();
  }

  {
    const app = new App({ port: 0 });
    let thenCalls = 0;
    app.rpc("/rpc", {
      s: () => ({ read() {
          app.close();
          return { then() { thenCalls++;  } };
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
      then() {  }
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
