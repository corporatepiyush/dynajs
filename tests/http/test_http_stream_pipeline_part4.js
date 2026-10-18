// flags: --std
// Part of the former tests/http/test_http_stream_pipeline.js, split so the parts run in parallel.
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

const onSeen = (re, bytes) => (write, getBuf) => {
  const t = setInterval(() => {
    if (re.test(getBuf())) { clearInterval(t); write(bytes); }
  }, 5);
};

(async () => {
  const log = [];

  

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

  {
    const app = new App({ port: 0 });
    app.rpc("/rpc", {
      s: () => deferredSrc(log, "p10", ["ONE-"], 10),
      ping: () => "PONG",
    });
    app.start();
    const r = await connRun(app.port,
      [[0, rpcReq("s", 1) + rpcReq("ping", 2, "Connection: close\r\n")]], 2500);
    eq(r.buf,
       streamHead() + streamBody(["ONE-"]) + jsonResp("PONG", 2),
       "P10: close-behind-a-stream answers fully, in order (after the terminal)");
    check(r.buf.indexOf("PONG") > r.buf.indexOf(TERMINAL),
          "P10: the held response lands strictly after the stream terminal");
    check(r.closed >= 1, "P10: connection closed after the close_after response");
    app.close();
  }

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

  print("test_http_stream_pipeline_part4: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
  std.gc();
})();
