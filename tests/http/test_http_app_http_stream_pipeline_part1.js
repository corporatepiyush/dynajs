// flags: --std
// covers: dyna:http.App
// Part of the former tests/http/test_http_stream_pipeline.js, split so the parts run in parallel.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_http_stream_pipeline_part1", { chain: true });
import * as std from "std";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + "\n  got  " + JSON.stringify(a) + "\n  want " + JSON.stringify(b));

const enc = new TextEncoder(), dec = new TextDecoder();
// Every response head carries exactly one IMF-fixdate Date line right after its status line
// (RFC 9110 6.6.1); undate() checks that and strips it so the byte pins below stay date-free.
const DATE_LINE = /(HTTP\/1\.1 \d{3} [^\r]*\r\n)Date: (?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{4} \d{2}:\d{2}:\d{2} GMT\r\n/g;
const undate = (w) => {
  const heads = (w.match(/HTTP\/1\.1 \d{3} /g) || []).length;
  const dated = (w.match(DATE_LINE) || []).length;
  check(heads === dated, "every response head has one Date line (" + dated + "/" + heads + ")");
  return w.replace(DATE_LINE, "$1");
};
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
    const finish = () => { if (!done) { done = true; resolve({ buf: undate(buf), closed }); } };
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

await __kitS.section("P1: pipelined normal behind a live stream stays behind its terminal", async () => {
(async () => {
  const log = [];

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

  print("test_http_stream_pipeline_part1: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
  std.gc();
})();
});
__kitS.end();
