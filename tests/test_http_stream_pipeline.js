// flags: --std
/* test_http_stream_pipeline.js -- request-pump ordering against a LIVE
 * streamed (ByteSource) response, asserted on the WIRE.
 *
 * The rule under test: while a stream response is pumping on a connection,
 * the request pump HOLDS the next request until the stream's terminal chunk
 * (or its truncation) -- exactly as it holds for a parked handler promise.
 * A request arriving behind a live stream (pipelined in one write, or
 * written mid-stream) is buffered and dispatched afterwards, so responses
 * leave in request order (RFC 9112) and an ordinary response can never be
 * injected INSIDE a live chunked body. The stream slot is multi-safe on top
 * of that: a second stream can never overwrite a live one.
 *
 * Rows: pipelined stream+normal and stream+stream in one write; the same
 * pairs written mid-stream (after the first chunk); a second request
 * written at several stream chunk positions (before the head, at the first
 * chunk, at the last chunk, just before the terminal); 3-deep pipelines
 * mixing streams and normal responses (both orders); sequential
 * stream -> normal -> stream on one keep-alive conn; the slot-overwrite
 * attack (second stream mid-stream: both sources' close() exactly once,
 * each body framed against its own bytes, no garbage); Connection: close
 * behind a live stream (close rides the stream's terminal; the request
 * behind it is never answered); a stream whose own request asked to close
 * (nothing pipelined behind it is answered); and a parked (async) handler
 * behind a live stream. Every row asserts the byte-EXACT wire, which is
 * the desync detector: one injected byte anywhere inside a chunked body
 * fails the equality. Every shape must also exit clean: the process
 * reaching the summary IS the no-teardown-abort assertion (an orphaned
 * pump asserts at engine teardown and aborts before printing it).
 */
import * as std from "std";
import { App, TCPServer } from "dyna:net";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + "\n  got  " + JSON.stringify(a) + "\n  want " + JSON.stringify(b));

const enc = new TextEncoder(), dec = new TextDecoder();
const rpcReq = (method, id, extra) => {
  const body = JSON.stringify({ jsonrpc: "2.0", id: id, method: method });
  return "POST /rpc HTTP/1.1\r\nHost: x\r\n" + (extra || "") +
         "Content-Type: application/json\r\nContent-Length: " + body.length +
         "\r\n\r\n" + body;
};
const frame = (s) => s.length.toString(16) + "\r\n" + s + "\r\n";
const TERMINAL = "0\r\n\r\n";
const streamHead = (conn) =>
  "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n" +
  "Transfer-Encoding: chunked\r\nConnection: " + (conn || "keep-alive") +
  "\r\n\r\n";
const jsonResp = (result, id, conn) => {
  const body = "{\"jsonrpc\":\"2.0\",\"result\":" + JSON.stringify(result) +
               ",\"id\":" + JSON.stringify(id) + "}";
  return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
         "Content-Length: " + body.length + "\r\nConnection: " +
         (conn || "keep-alive") + "\r\n\r\n" + body;
};
const streamBody = (parts) => parts.map(frame).join("") + TERMINAL;

/* A deferred ByteSource: one part per read, spaced by `ms`; logs its
 * close() position. */
function deferredSrc(log, tag, parts, ms) {
  let i = 0;
  return {
    read(buf) {
      return new Promise((r) => setTimeout(() => {
        if (i >= parts.length) { r(0); return; }
        const e = enc.encode(parts[i++]);
        buf.set(e, 0);
        r(e.length);
      }, ms));
    },
    close() { log.push("close:" + tag); },
  };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* One keep-alive connection with scripted writes: steps are [when, bytes]
 * -- when === 0 means on connect, a function means "call with (write, getBuf)
 * to schedule yourself". Collects the full buffer and close events. */
function connRun(port, steps, waitMs) {
  return new Promise((resolve) => {
    let buf = "", closed = 0, done = false;
    const finish = () => { if (!done) { done = true; resolve({ buf, closed }); } };
    const conn = TCPServer.connect({ host: "127.0.0.1", port: port }, {
      connect: (s) => {
        const write = (str) => { try { s.write(enc.encode(str)); } catch (e) {} };
        for (const [when, bytes] of steps) {
          if (when === 0) write(bytes);
          else if (typeof when === "function") when(write, () => buf);
          else setTimeout(() => write(bytes), when);
        }
      },
      data: (s, b) => { buf += dec.decode(b); },
      close: () => { closed++; finish(); },
    });
    setTimeout(() => { try { conn.close(); } catch (e) {} finish(); }, waitMs);
  });
}

/* Write `bytes` once the buffer first matches `re` (poll). */
const onSeen = (re, bytes) => (write, getBuf) => {
  const t = setInterval(() => {
    if (re.test(getBuf())) { clearInterval(t); write(bytes); }
  }, 5);
};

(async () => {
  const log = [];

  /* P1: pipelined [stream, normal] in ONE write. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => deferredSrc(log, "p1", ["ONE-", "TWO-"], 15),
      ping: () => "PONG",
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s", 1) + rpcReq("ping", 2)]], 1500);
    eq(r.buf,
       streamHead() + streamBody(["ONE-", "TWO-"]) + jsonResp("PONG", 2),
       "P1: pipelined normal behind a live stream stays behind its terminal");
    app.close();
  }

  /* P2: pipelined [stream, stream] in ONE write (the slot attack, pipelined). */
  {
    log.length = 0;
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s1: () => deferredSrc(log, "s1", ["ONE-", "TWO-"], 15),
      s2: () => deferredSrc(log, "s2", ["AAA-", "BBB-"], 15),
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s1", 1) + rpcReq("s2", 2)]], 2000);
    eq(r.buf,
       streamHead() + streamBody(["ONE-", "TWO-"]) +
       streamHead() + streamBody(["AAA-", "BBB-"]),
       "P2: two pipelined streams land as two complete bodies, in order");
    eq(log, ["close:s1", "close:s2"],
       "P2: both sources' close() ran, in order, exactly once");
    check(r.buf.indexOf("\u0000") < 0, "P2: no NUL-garbage chunks");
    app.close();
  }

  /* P3: ordinary request written MID-STREAM of a live stream. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => deferredSrc(log, "p3", ["ONE-", "TWO-", "3ONE"], 15),
      ping: () => "PONG",
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s", 1)],
       [onSeen(/ONE-/, rpcReq("ping", 2))]], 2000);
    eq(r.buf,
       streamHead() + streamBody(["ONE-", "TWO-", "3ONE"]) + jsonResp("PONG", 2),
       "P3: mid-stream ordinary response is never injected into the body");
    app.close();
  }

  /* P4: a second STREAM written mid-stream (the slot-overwrite attack). */
  {
    log.length = 0;
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s1: () => deferredSrc(log, "s1", ["ONE-", "TWO-", "3ONE"], 15),
      s2: () => deferredSrc(log, "s2", ["AAA-", "BBB-"], 15),
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s1", 1)],
       [onSeen(/ONE-/, rpcReq("s2", 2))]], 2500);
    eq(r.buf,
       streamHead() + streamBody(["ONE-", "TWO-", "3ONE"]) +
       streamHead() + streamBody(["AAA-", "BBB-"]),
       "P4: mid-stream second stream never overwrites the live pump");
    eq(log, ["close:s1", "close:s2"],
       "P4: first source's close() is not skipped; both run once, in order");
    check(r.buf.indexOf("\u0000") < 0, "P4: no wrong-buffer (NUL) chunks");
    check((r.buf.split("HTTP/1.1").length - 1) === 2 &&
          (r.buf.split(TERMINAL).length - 1) === 2,
          "P4: exactly two heads and two terminals for two chunked bodies");
    app.close();
  }

  /* P5: 3-deep pipeline [stream, normal, stream] in one write. */
  {
    log.length = 0;
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s1: () => deferredSrc(log, "s1", ["A1-"], 10),
      ping: () => "PONG",
      s2: () => deferredSrc(log, "s2", ["B1-"], 10),
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s1", 1) + rpcReq("ping", 2) + rpcReq("s2", 3)]], 2500);
    eq(r.buf,
       streamHead() + streamBody(["A1-"]) +
       jsonResp("PONG", 2) +
       streamHead() + streamBody(["B1-"]),
       "P5: [stream, normal, stream] pipeline keeps request order");
    eq(log, ["close:s1", "close:s2"], "P5: both sources closed once, in order");
    app.close();
  }

  /* P6: 3-deep pipeline [normal, stream, normal] in one write. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      ping1: () => "ONE",
      s: () => deferredSrc(log, "p6", ["S1-"], 10),
      ping2: () => "TWO",
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("ping1", 1) + rpcReq("s", 2) + rpcReq("ping2", 3)]], 2500);
    eq(r.buf,
       jsonResp("ONE", 1) +
       streamHead() + streamBody(["S1-"]) +
       jsonResp("TWO", 3),
       "P6: [normal, stream, normal] pipeline keeps request order");
    app.close();
  }

  /* P7: sequential stream -> normal -> stream on ONE keep-alive conn. */
  {
    log.length = 0;
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s1: () => deferredSrc(log, "s1", ["A1-"], 10),
      ping: () => "PONG",
      s2: () => deferredSrc(log, "s2", ["B1-"], 10),
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s1", 1)],
       [onSeen(new RegExp(TERMINAL.replace(/([()])/g, "\\$1")), rpcReq("ping", 2))],
       [onSeen(/"id":2/, rpcReq("s2", 3))]], 3000);
    eq(r.buf,
       streamHead() + streamBody(["A1-"]) +
       jsonResp("PONG", 2) +
       streamHead() + streamBody(["B1-"]),
       "P7: sequential stream/normal/stream on one conn is clean and ordered");
    eq(log, ["close:s1", "close:s2"], "P7: both sources closed once, in order");
    app.close();
  }

  /* P8: the second request arrives at four stream chunk positions; the wire
   * must be IDENTICAL at every position. */
  {
    const want = streamHead() + streamBody(["ONE-", "TWO-", "3ONE"]) +
                 jsonResp("PONG", 2);
    const positions = [
      ["t=1ms with the stream request", 1],
      ["at the head", onSeen(/HTTP\/1\.1/, rpcReq("ping", 2))],
      ["after chunk 1", onSeen(/ONE-/, rpcReq("ping", 2))],
      ["after chunk 3", onSeen(/3ONE/, rpcReq("ping", 2))],
    ];
    for (const [label, trigger] of positions) {
      const app = new App({ port: 0 });
      app.rpc("/rpc", {
        s: () => deferredSrc(log, "p8", ["ONE-", "TWO-", "3ONE"], 15),
        ping: () => "PONG",
      });
      app.start();
      const steps = [[0, rpcReq("s", 1)]];
      steps.push(typeof trigger === "number"
        ? [trigger, rpcReq("ping", 2)]
        : [trigger]);
      const r = await connRun(app.port, steps, 2500);
      eq(r.buf, want, "P8 (" + label + "): identical ordered wire");
      app.close();
      await sleep(20);
    }
  }

  /* P9: slot-overwrite attack hammered: THREE streams pipelined behind one
   * write, disjoint payloads. Each body framed against its OWN bytes. */
  {
    log.length = 0;
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s1: () => deferredSrc(log, "s1", ["1a--", "1b--"], 10),
      s2: () => deferredSrc(log, "s2", ["2a--", "2b--"], 10),
      s3: () => deferredSrc(log, "s3", ["3a--", "3b--"], 10),
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s1", 1) + rpcReq("s2", 2) + rpcReq("s3", 3)]], 3000);
    eq(r.buf,
       streamHead() + streamBody(["1a--", "1b--"]) +
       streamHead() + streamBody(["2a--", "2b--"]) +
       streamHead() + streamBody(["3a--", "3b--"]),
       "P9: three pipelined streams: three complete bodies, correct buffers");
    eq(log, ["close:s1", "close:s2", "close:s3"],
       "P9: all three close() hooks ran exactly once, in order");
    check(r.buf.indexOf("\u0000") < 0, "P9: no garbage chunks");
    app.close();
  }

  /* P10: a Connection: close request behind a live stream: the close rides
   * the stream's terminal and the held request answers on its own response
   * first. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => deferredSrc(log, "p10", ["ONE-"], 10),
      ping: () => "PONG",
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s", 1) + rpcReq("ping", 2, "Connection: close\r\n")]], 2500);
    /* Ordinary responses always spell Connection: keep-alive (the app's
     * constant header); the close still rides this response's send
     * completion, and the ORDER is the rule under test. */
    eq(r.buf,
       streamHead() + streamBody(["ONE-"]) + jsonResp("PONG", 2),
       "P10: close-behind-a-stream answers fully, in order (after the terminal)");
    check(r.buf.indexOf("PONG") > r.buf.indexOf(TERMINAL),
          "P10: the held response lands strictly after the stream terminal");
    check(r.closed >= 1, "P10: connection closed after the close_after response");
    app.close();
  }

  /* P11: the STREAM's own request asked to close: the terminal rides the
   * close and NOTHING pipelined behind it is answered (same rule as every
   * other close_after response). */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => deferredSrc(log, "p11", ["ONE-"], 10),
      ping: () => "PONG",
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s", 1, "Connection: close\r\n") + rpcReq("ping", 2)]], 2500);
    eq(r.buf, streamHead("close") + streamBody(["ONE-"]),
       "P11: close_after stream: full body, then close; request behind is dead");
    eq(r.buf.indexOf("PONG"), -1, "P11: nothing pipelined behind a close is answered");
    app.close();
  }

  /* P12: a parked (async) handler behind a live stream: dispatched after
   * the terminal, settled after that. */
  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => deferredSrc(log, "p12", ["ONE-", "TWO-"], 10),
      slow: () => new Promise((r) => setTimeout(() => r("LATE"), 40)),
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s", 1) + rpcReq("slow", 2)]], 2500);
    eq(r.buf,
       streamHead() + streamBody(["ONE-", "TWO-"]) + jsonResp("LATE", 2),
       "P12: async handler behind a live stream answers after the terminal");
    app.close();
  }

  print("test_http_stream_pipeline: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
  std.gc();
})();
