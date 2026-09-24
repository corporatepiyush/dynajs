// flags: --std
/* test_http_stream_pump.js -- the streamed-ByteSource response pump
 * (App.rpc handler returning a ByteSource / {stream,...}), asserted on the
 * WIRE.
 *
 * The pump's lifetime rule under test: the send of a chunk or the terminal
 * chunk may complete INLINE (inside the very dyn_aio_send that started it),
 * and a read() runs user JS that can close the app out from under the
 * loop. In both shapes the pump state must survive until the owning frame
 * has taken its decision -- the state is freed once, from one owner path,
 * after the completion returns -- so steering decisions never read freed
 * memory and no path frees twice. Rows cover: read() resolving 0 at the
 * first/middle/last read; inline and deferred completions interleaved;
 * close and dispose in every mid-stream position (client gone, app.close()
 * from read(), app.close() from the source's close(), app.close() with a
 * read parked, dispose over a live stream); GC pressure with all JS-side
 * refs dropped while pumping; errors thrown and rejected mid-stream after
 * a partial send; contract-violating read counts; two concurrent streamed
 * responses; frame sizes across the window; the {stream, contentType,
 * status} envelope; Connection: close; and conn reuse after a stream. Raw
 * sockets: every assertion is on bytes seen by the peer plus the survival
 * of the process.
 */
import * as std from "std";
import { App, TCPServer } from "dyna:net";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));

const enc = new TextEncoder(), dec = new TextDecoder();
const rpcReq = (method, id, extra) => {
  const body = JSON.stringify({ jsonrpc: "2.0", id: id, method: method });
  return "POST /rpc HTTP/1.1\r\nHost: x\r\n" + (extra || "") +
         "Content-Type: application/json\r\nContent-Length: " + body.length +
         "\r\n\r\n" + body;
};

/* One raw request per conn: accumulate every byte, resolve on close or
   timeout. onBytes(buf, conn, finish) may close the conn early. */
function raw(port, reqStr, waitMs, onBytes) {
  return new Promise((resolve) => {
    let buf = "", done = false;
    const finish = () => { if (!done) { done = true; resolve(buf); } };
    const conn = TCPServer.connect({ host: "127.0.0.1", port: port }, {
      connect: (s) => { s.write(enc.encode(reqStr)); },
      data: (s, b) => {
        buf += dec.decode(b);
        if (onBytes) onBytes(buf, conn, finish);
      },
      close: finish,
    });
    setTimeout(() => { try { conn.close(); } catch (e) {} finish(); }, waitMs);
  });
}

/* A keep-alive session: sequential requests on ONE conn. A response is
   bounded by its chunked terminal or its Content-Length body. */
function session(port) {
  let buf = "", sock = null, waiters = [];
  const pump = () => {
    while (waiters.length) {
      const w = waiters[0];
      let end = -1;
      if (w.kind === "chunked") {
        const t = buf.indexOf("0\r\n\r\n", w.from);
        if (t >= 0) end = t + 5;
      } else {
        const sp = buf.indexOf("\r\n\r\n", w.from);
        if (sp >= 0) {
          const head = buf.slice(w.from, sp);
          const m = head.match(/content-length:\s*(\d+)/i);
          if (m) {
            const need = sp + 4 + +m[1];
            if (buf.length >= need) end = need;
          }
        }
      }
      if (end < 0) return;
      waiters.shift();
      w.resolve(buf.slice(w.from, end));
    }
  };
  /* The returned handle anchors the connection: dropping it lets the
     collector close the socket out from under the session. */
  const handle = TCPServer.connect({ host: "127.0.0.1", port: port }, {
    connect: (s) => { sock = s; },
    data: (s, b) => { buf += dec.decode(b); pump(); },
    close: () => {
      const ws = waiters; waiters = [];
      for (const w of ws) w.resolve(buf.slice(w.from));
    },
  });
  return {
    handle,
    async request(reqStr, kind, waitMs) {
      for (let i = 0; i < 500 && !sock; i++) await sleep(2);
      return new Promise((resolve) => {
        const from = buf.length;
        const w = { from, kind, resolve };
        waiters.push(w);
        sock.write(enc.encode(reqStr));
        setTimeout(() => {
          const i = waiters.indexOf(w);
          if (i >= 0) { waiters.splice(i, 1); resolve(buf.slice(from)); }
        }, waitMs);
      });
    },
  };
}

/* Parse one HTTP response and walk its chunked body strictly. */
function parseResp(buf) {
  const sp = buf.indexOf("\r\n\r\n");
  if (sp < 0)
    return { status: 0, head: buf, chunks: [], sawTerminal: false, text: "" };
  const head = buf.slice(0, sp);
  const body = buf.slice(sp + 4);
  const m = head.match(/^HTTP\/1\.1 (\d+)/);
  const chunks = [];
  let i = 0, sawTerminal = false;
  while (i < body.length) {
    const nl = body.indexOf("\r\n", i);
    if (nl < 0) break;
    const sz = parseInt(body.slice(i, nl), 16);
    if (isNaN(sz)) break;
    i = nl + 2;
    if (sz === 0) { sawTerminal = true; break; }
    if (body.length < i + sz + 2) break;
    chunks.push(body.slice(i, i + sz));
    i += sz + 2;
  }
  return { status: m ? +m[1] : 0, head,
           chunks, sawTerminal, text: chunks.join("") };
}

/* ---- ByteSource shapes ----------------------------------------------- */

/* Every read resolves INLINE (the promise is already settled when the
   pump inspects it) -- the terminal send then completes inline too. */
function inlineSrc(parts) {
  let i = 0;
  return {
    read(buf) {
      if (i >= parts.length) return Promise.resolve(0);
      const e = enc.encode(parts[i++]);
      buf.set(e, 0);
      return Promise.resolve(e.length);
    }
  };
}

/* Every read resolves on a timer (the pump parks on the promise). */
function deferredSrc(parts, ms) {
  let i = 0;
  return {
    read(buf) {
      return new Promise((r) => setTimeout(() => {
        if (i >= parts.length) { r(0); return; }
        const e = enc.encode(parts[i++]);
        buf.set(e, 0);
        r(e.length);
      }, ms));
    }
  };
}

/* Reads alternate: deferred, inline, deferred, ... */
function mixedSrc(parts) {
  let i = 0;
  return {
    read(buf) {
      const deliver = (r) => {
        if (i >= parts.length) { r(0); return; }
        const e = enc.encode(parts[i++]);
        buf.set(e, 0);
        r(e.length);
      };
      return (i % 2 === 0) ? new Promise((r) => setTimeout(() => deliver(r), 5))
                           : new Promise((r) => deliver(r));
    }
  };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const churn = (rounds) => {
  for (let i = 0; i < rounds; i++) {
    const junk = [];
    for (let j = 0; j < 1000; j++) junk.push({ a: i, b: "" + j });
    std.gc();
  }
};

(async () => {
  /* ============ read() resolving 0: first / middle / last read ======== */

  /* FIRST read resolves 0: the terminal chunk's send can complete INLINE
     and the pump must not steer itself from state that completion retired.
     The response is head + the bare terminal chunk, and the conn stays
     usable afterwards. */
  {
    const app = new App({ port: 0 });
    let closed = 0;
    app.rpc("/rpc", {
      s: () => ({ read() { return Promise.resolve(0); },
                  close() { closed++; } }),
      ping: () => "pong",
    });
    app.start();
    const s = session(app.port);
    const resp = parseResp(await s.request(rpcReq("s", 1), "chunked", 3000));
    eq(resp.status, 200, "first-read 0: 200 with the stream head");
    check(/transfer-encoding:\s*chunked/i.test(resp.head),
          "first-read 0: chunked framing");
    eq(resp.chunks, [], "first-read 0: no data chunks");
    eq(resp.sawTerminal, true, "first-read 0: terminal chunk sent");
    eq(closed, 1, "first-read 0: the source's close() ran exactly once");
    const pong = await s.request(rpcReq("ping", 2), "len", 3000);
    check(/200/.test(pong) && pong.indexOf("pong") >= 0,
          "first-read 0: the conn is reusable after the stream (" +
          JSON.stringify(pong.slice(0, 40)) + ")");
    app.close();
  }

  /* MIDDLE: two data reads, then 0 -- all inline. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => inlineSrc(["A", "BB"]) });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.chunks, ["A", "BB"], "middle 0 (inline): data chunks in order");
    eq(resp.text, "ABB", "middle 0 (inline): body bytes exact");
    eq(resp.sawTerminal, true, "middle 0 (inline): terminal chunk");
    app.close();
  }

  /* LAST read of a longer stream: 8 mixed-size chunks then 0, including a
     full-window (64 KiB) read whose frame crosses 16-bit hex sizes. */
  {
    const app = new App({ port: 0 });
    const big = new Array(65536 + 1).join("x");   /* the window exactly */
    const parts = ["c0-", "c1-", big, "c3-", "c4-", "c5-", "c6-", "c7!"];
    app.rpc("/rpc", { s: () => inlineSrc(parts) });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 5000));
    eq(resp.chunks.length, parts.length, "last 0 (inline): 8 chunks");
    eq(resp.text, parts.join(""), "last 0 (inline): every byte in order");
    eq(resp.chunks[2].length, 65536,
       "last 0 (inline): full-window chunk framed exactly");
    eq(resp.sawTerminal, true, "last 0 (inline): terminal after the last read");
    app.close();
  }

  /* MIDDLE/LAST with every read DEFERRED (the settle path + inline
     terminal send). */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => deferredSrc(["D1-", "D2-", "D3-"], 5) });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 4000));
    eq(resp.chunks, ["D1-", "D2-", "D3-"], "0 after deferred chunks: exact");
    eq(resp.sawTerminal, true, "0 after deferred chunks: terminal");
    app.close();
  }

  /* Deferred FIRST read resolving 0: the settle runs the terminal send
     inline from the settle frame. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => deferredSrc([], 5) });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.chunks, [], "deferred first-read 0: no data");
    eq(resp.sawTerminal, true, "deferred first-read 0: terminal");
    app.close();
  }

  /* Deferred data read, then an INLINE read resolving 0: the settle sends
     the data frame inline, resumes the loop, and the loop's terminal send
     completes inline -- nested completions must not free under the loop. */
  {
    const app = new App({ port: 0 });
    let call = 0;
    app.rpc("/rpc", {
      s: () => ({
        read(buf) {
          call++;
          if (call === 1)
            return new Promise((r) => setTimeout(() => {
              const e = enc.encode("MID");
              buf.set(e, 0);
              r(e.length);
            }, 5));
          return Promise.resolve(0);
        }
      }),
    });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.chunks, ["MID"],
       "deferred chunk + inline 0: data frame delivered exactly once");
    eq(resp.sawTerminal, true, "deferred chunk + inline 0: terminal");
    app.close();
  }

  /* ============ inline vs deferred completions interleaved ============ */

  /* One stream alternating inline and deferred settlements, several
     rounds to shake the completion order. */
  for (let round = 0; round < 3; round++) {
    const app = new App({ port: 0 });
    const parts = ["i" + round + "-", "d" + round + "-", "i" + round + "=",
                   "d" + round + "="];
    app.rpc("/rpc", { s: () => mixedSrc(parts) });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 4000));
    eq(resp.text, parts.join(""),
       "mixed inline/deferred round " + round + ": bytes exact");
    eq(resp.sawTerminal, true,
       "mixed inline/deferred round " + round + ": terminal");
    app.close();
    await sleep(1);
  }

  /* Two concurrent streams on ONE app: one all-inline, one all-deferred,
     their completions interleaved on the single reactor. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      fast: () => inlineSrc(["F1-", "F2-", "F3-", "F4-", "F5!"]),
      slow: () => deferredSrc(["S1-", "S2-", "S3-", "S4-", "S5!"], 7),
    });
    app.start();
    const [rb, rs] = await Promise.all([
      raw(app.port, rpcReq("fast", 1), 5000),
      raw(app.port, rpcReq("slow", 2), 5000),
    ]);
    const rf = parseResp(rb), rslow = parseResp(rs);
    eq(rf.text, "F1-F2-F3-F4-F5!",
       "concurrent streams: the inline stream's bytes are exact");
    eq(rslow.text, "S1-S2-S3-S4-S5!",
       "concurrent streams: the deferred stream's bytes are exact");
    check(rf.sawTerminal && rslow.sawTerminal,
          "concurrent streams: both terminated");
    app.close();
  }

  /* ============ close / dispose mid-stream ============================ */

  /* Client walks away mid-stream: the pump runs on to its end or dies on
     the abort, but the PROCESS must survive and serve others. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => deferredSrc(["X1-", "X2-", "X3-"], 10) });
    app.start();
    let got = "";
    await raw(app.port, rpcReq("s", 1), 2500, (buf, conn, finish) => {
      got = buf;
      if (buf.indexOf("X1-") >= 0) { try { conn.close(); } catch (e) {} }
    });
    check(got.indexOf("HTTP/1.1 200") >= 0,
          "client close mid-stream: head was delivered");
    await sleep(80);                     /* let the pump finish/abort */
    const pong = parseResp(await raw(app.port,
      "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 3000));
    check(pong.status === 404,
          "client close mid-stream: the server still answers (" +
          pong.status + ")");
    app.close();
  }

  /* app.close() FROM INSIDE read() after a partial send: the abort lands
     in the middle of the pump loop and the state must not be freed under
     it. (Un-fixed: the loop steers freed state -- crash/ASan.) */
  {
    const app = new App({ port: 0 });
    let call = 0;
    app.rpc("/rpc", {
      s: () => ({
        read(buf) {
          call++;
          if (call === 1) {
            const e = enc.encode("GO");
            buf.set(e, 0);
            return Promise.resolve(e.length);
          }
          app.close();                   /* dispose mid-pump */
          return Promise.resolve(0);
        }
      }),
    });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.chunks, ["GO"], "app.close() in read(): the partial send landed");
    eq(resp.sawTerminal, false,
       "app.close() in read(): no terminal after the abort (truncated)");
    check(resp.status === 200, "app.close() in read(): head was 200");
    await sleep(30);
  }

  /* The process survived the abort above: a fresh app answers. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { ping: () => "pong-after-abort" });
    app.start();
    const buf = await raw(app.port, rpcReq("ping", 9), 3000);
    check(buf.indexOf("pong-after-abort") >= 0,
          "process alive after an in-read dispose");
    app.close();
  }

  /* app.close() FROM the source's close() -- the terminal path's close()
     hook runs user JS just before the terminal send, under the same
     frame. (Un-fixed: the abort frees the state and on_count then writes
     it -- ASan.) */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ read() { return Promise.resolve(0); },
                  close() { app.close(); } }),
    });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.status, 200, "app.close() in close(): head was 200");
    eq(resp.sawTerminal, false,
       "app.close() in close(): the aborted pump sends no terminal");
    await sleep(30);
  }

  /* app.close() WHILE a read is parked, then the parked read settles:
     the settle is the last holder and must do the free exactly once. */
  {
    const app = new App({ port: 0 });
    let parked = null, reads = 0;
    app.rpc("/rpc", {
      s: () => ({
        read(buf) {
          reads++;
          if (reads === 1) {
            const e = enc.encode("P");
            buf.set(e, 0);
            return Promise.resolve(e.length);
          }
          return new Promise((r) => { parked = r; });
        }
      }),
    });
    app.start();
    const p = raw(app.port, rpcReq("s", 1), 3000);
    for (let i = 0; i < 200 && !parked; i++) await sleep(5);
    check(parked !== null, "parked-read dispose: the read parked");
    app.close();                          /* dispose under the park */
    await sleep(20);
    if (parked) parked(0);                /* the settle fires after it */
    const resp = parseResp(await p);
    eq(resp.chunks, ["P"], "parked-read dispose: the parked frame landed");
    await sleep(30);
  }

  /* Dispose while one stream is mid-flight AND another conn is idle. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => deferredSrc(["K1-", "K2-", "K3-"], 15),
      kill: () => { app.close(); return "dead"; },
    });
    app.start();
    const a = raw(app.port, rpcReq("s", 1), 3000);
    await sleep(25);
    await raw(app.port, rpcReq("kill", 2), 3000);
    parseResp(await a);
    await sleep(30);
    check(true, "dispose over a live stream + an idle conn: process alive");
  }

  /* ============ GC pressure mid-stream =============================== */

  /* The handler's source is created inline and NEVER captured by the
     test: after the handler returns, only the pump's own C-held refs keep
     src/readfn/chunk alive. GC rounds while the stream is mid-flight must
     not disturb a byte. */
  {
    const app = new App({ port: 0 });
    const parts = ["GA-", "RA-", "GE-", "RA-2-", "GE-2!"];
    app.rpc("/rpc", {
      s: () => {
        let i = 0;
        return {
          read(buf) {
            return new Promise((r) => setTimeout(() => {
              if (i >= parts.length) { r(0); return; }
              const e = enc.encode(parts[i++]);
              buf.set(e, 0);
              r(e.length);
            }, 10));
          }
        };
      },
    });
    app.start();
    const p = raw(app.port, rpcReq("s", 1), 5000,
                  () => { churn(1); });    /* GC between every chunk */
    churn(3);
    const resp = parseResp(await p);
    churn(3);
    eq(resp.text, parts.join(""),
       "GC pressure mid-stream (all JS refs dropped): bytes exact");
    eq(resp.sawTerminal, true, "GC pressure mid-stream: terminal");
    const pong = await raw(app.port, rpcReq("nope", 5), 3000);
    check(pong.indexOf("404") >= 0, "GC pressure mid-stream: server alive");
    app.close();
  }

  /* ============ errors mid-stream after a partial send ================ */

  /* read() THROWS after two chunks went out: past the status line the
     only honest signal is truncation -- chunked body, no terminal, conn
     closed. */
  {
    const app = new App({ port: 0 });
    let call = 0;
    app.rpc("/rpc", {
      s: () => ({
        read(buf) {
          call++;
          if (call <= 2) {
            const e = enc.encode(call === 1 ? "PAR-" : "TIAL");
            buf.set(e, 0);
            return Promise.resolve(e.length);
          }
          throw new Error("mid-stream throw");
        }
      }),
    });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.chunks, ["PAR-", "TIAL"],
       "throw after partial send: the delivered chunks are exact");
    eq(resp.sawTerminal, false,
       "throw after partial send: NO terminal (honest truncation)");
    check(resp.status === 200, "throw after partial send: head was 200");
  }

  /* read() REJECTS after a partial send (deferred rejection: the settle
     error path). */
  {
    const app = new App({ port: 0 });
    let call = 0;
    app.rpc("/rpc", {
      s: () => ({
        read(buf) {
          call++;
          if (call === 1) {
            const e = enc.encode("RE-");
            buf.set(e, 0);
            return Promise.resolve(e.length);
          }
          return new Promise((_, rej) =>
            setTimeout(() => rej(new Error("mid-stream reject")), 5));
        }
      }),
    });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.chunks, ["RE-"], "reject after partial send: chunk exact");
    eq(resp.sawTerminal, false,
       "reject after partial send: NO terminal (honest truncation)");
  }

  /* ============ contract-violating counts truncate cleanly =========== */

  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      big: () => {
        let d = 0;
        return { read(buf) {
          d++;
          if (d === 1) {
            const e = enc.encode("ok-");
            buf.set(e, 0);
            return Promise.resolve(e.length);
          }
          return Promise.resolve(65537);   /* one past the window */
        } };
      },
      weird: () => {
        let d = 0;
        return { read(buf) {
          d++;
          if (d === 1) {
            const e = enc.encode("ok-");
            buf.set(e, 0);
            return Promise.resolve(e.length);
          }
          return Promise.resolve("not-a-count");
        } };
      },
    });
    app.start();
    const r1 = parseResp(await raw(app.port, rpcReq("big", 1), 3000));
    eq(r1.chunks, ["ok-"], "count past the window: prefix delivered");
    eq(r1.sawTerminal, false, "count past the window: truncated, no terminal");
    const r2 = parseResp(await raw(app.port, rpcReq("weird", 2), 3000));
    eq(r2.chunks, ["ok-"], "non-number count: prefix delivered");
    eq(r2.sawTerminal, false, "non-number count: truncated, no terminal");
    app.close();
  }

  /* ============ the {stream, contentType, status} envelope ============ */

  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => ({ stream: deferredSrc(["E1-", "E2!"], 5),
                  contentType: "text/plain", status: 201 }),
    });
    app.start();
    const resp = parseResp(await raw(app.port, rpcReq("s", 1), 3000));
    eq(resp.status, 201, "envelope form: handler status honored");
    check(/content-type:\s*text\/plain/i.test(resp.head),
          "envelope form: handler content type honored");
    eq(resp.text, "E1-E2!", "envelope form: bytes exact");
    eq(resp.sawTerminal, true, "envelope form: terminal");
    app.close();
  }

  /* Connection: close + stream: the close rides the TERMINAL completion
     (never before the body is out). */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", { s: () => inlineSrc(["Z1-", "Z2!"]) });
    app.start();
    const resp = parseResp(await raw(app.port,
      rpcReq("s", 1, "Connection: close\r\n"), 3000));
    check(/connection:\s*close/i.test(resp.head),
          "close_after stream: head says close");
    eq(resp.text, "Z1-Z2!", "close_after stream: full body before the close");
    eq(resp.sawTerminal, true, "close_after stream: terminal landed first");
    app.close();
  }

  print("test_http_stream_pump: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
  std.gc();
})();
