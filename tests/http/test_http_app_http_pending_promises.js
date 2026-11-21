// covers: dyna:http.App
// The App starts in the kit's before hook (never under --list); the last case
// closes it on purpose (a settle after close), so after closes it only if open.
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { suite } from "../lib/kit.js";

const t = suite("test_http_pending_promises");
let A = null;
const check = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));

let app = null, closed = false;
let slow1 = null, slow2 = null, late = null;
t.before(() => {
app = new App({ port: 0 });
app.get("/double", () => ({
  then(res) { res("first"); res("second"); }
}));
app.get("/settle-throw", () => ({
  then(res) { res("first"); throw new Error("late throw after settle"); }
}));
app.get("/res-rej", () => ({
  then(res, rej) { res("first"); rej(new Error("rejection after resolution")); }
}));
app.get("/rej-res", () => ({
  then(res, rej) { rej(new Error("first-reject")); res("second"); }
}));
app.get("/abandon", () => ({ then() {  } }));
app.get("/late", () => ({ then(res) { late = res; } }));
app.get("/slow1", () => new Promise((r) => { slow1 = r; }));
app.get("/slow2", () => new Promise((r) => { slow2 = r; }));
app.get("/fast", () => "FAST");
app.get("/nulthrow", () => { throw new Error("before\u0000AFTER-tail\u0000MORE"); });
app.use((ctx) => {
  if (ctx.path === "/nulmw") throw new Error("m1\u0000m2-tail\u0000m3");
});
app.get("/nulmw", () => "should-not-run");
app.rpc("/rpc", {
  evil() { return { then(res) { res("R1"); res("R2"); } }; },
  evilThrow() { return { then(res) { res("R1"); throw new Error("late"); } }; },
  nulthrow() { throw new Error("r1\u0000r2-tail\u0000r3"); },
  nulrej() { return Promise.reject(new Error("p1\u0000p2-tail\u0000p3")); },
  nulthen() {
    return { then(res, rej) { rej(new Error("t1\u0000t2-tail\u0000t3")); } };
  },
});
app.start();
});
t.after(() => { if (app && !closed) app.close(); });
let buf, b, p;
const bodyOf = (s) => {
  const i = s.indexOf("\r\n\r\n");
  return i < 0 ? "" : s.slice(i + 4);
};
const jsonOf = (s) => { try { return JSON.parse(bodyOf(s)); } catch (e) { return null; } };

const raw = (reqBytes, waitMs) => new Promise((resolve) => {
  let buf = "";
  const conn = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
    connect: (s) => { s.write(new TextEncoder().encode(reqBytes)); },
    data: (s, b) => { buf += new TextDecoder().decode(b); },
    close: () => resolve(buf),
  });
  setTimeout(() => { try { conn.close(); } catch (e) {} resolve(buf); }, waitMs);
});
const respCount = (s) => (s.match(/HTTP\/1\.1 /g) || []).length;
const rpcReq = (body) =>
  "POST /rpc HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n" +
  "Content-Length: " + body.length + "\r\n\r\n" + body;

test("a double-resolving thenable sends exactly one response, the first", async () => {
  buf = await raw("GET /double HTTP/1.1\r\nHost: x\r\n\r\n", 600);
  eq(respCount(buf), 1,
     "double-resolving thenable: exactly one response on the wire");
  check(buf.indexOf("first") >= 0, "and the first settle's body is the one sent");
  check(buf.indexOf("second") < 0, "and the second resolve changed nothing");

});
test("a settle-then-throw thenable sends the settled response once", async () => {
  buf = await raw("GET /settle-throw HTTP/1.1\r\nHost: x\r\n\r\n", 600);
  eq(respCount(buf), 1, "settle-then-throw thenable: exactly one response");
  check(buf.indexOf("first") >= 0 && buf.indexOf("Server error") < 0,
        "and the settle (not the sync-throw fallback) produced it");

});
test("a resolve-then-reject thenable: the resolution wins", async () => {
  buf = await raw("GET /res-rej HTTP/1.1\r\nHost: x\r\n\r\n", 600);
  eq(respCount(buf), 1, "resolve-then-reject thenable: exactly one response");
  check(buf.indexOf("first") >= 0, "and the resolution won the race");

});
test("a reject-then-resolve thenable: the rejection wins", async () => {
  buf = await raw("GET /rej-res HTTP/1.1\r\nHost: x\r\n\r\n", 600);
  eq(respCount(buf), 1, "reject-then-resolve thenable: exactly one response");
  check(/500/.test(buf) && buf.indexOf("first-reject") >= 0,
        "and the rejection won the race");

});
test("rpc: a double-resolving thenable sends one response, the first", async () => {
  buf = await raw(rpcReq('{"jsonrpc":"2.0","method":"evil","id":1}'), 600);
  eq(respCount(buf), 1, "rpc double-resolving thenable: exactly one response");
  check(buf.indexOf("R1") >= 0 && buf.indexOf("R2") < 0, "first settle won");

});
test("rpc: a settle-then-throw thenable sends the resolved result once", async () => {
  buf = await raw(rpcReq('{"jsonrpc":"2.0","method":"evilThrow","id":2}'), 600);
  eq(respCount(buf), 1, "rpc settle-then-throw thenable: exactly one response");
  check(buf.indexOf("R1") >= 0, "and the resolved result was sent");

});
test("pipelined slow-then-fast: the parked response leaves first", async () => {
  setTimeout(() => { if (slow1) slow1("SLOW"); }, 300);
  buf = await raw("GET /slow1 HTTP/1.1\r\nHost: x\r\n\r\n" +
                  "GET /fast HTTP/1.1\r\nHost: x\r\n\r\n", 1500);
  check(buf.indexOf("SLOW") >= 0,
        "pipelined slow-then-fast: the parked first response is delivered");
  check(buf.indexOf("FAST") < 0 || buf.indexOf("SLOW") < buf.indexOf("FAST"),
        "and it leaves BEFORE the fast second response");

});
test("a pipelined close behind a parked response keeps request order", async () => {
  setTimeout(() => { if (slow2) slow2("SLOW2"); }, 300);
  buf = await raw("GET /slow2 HTTP/1.1\r\nHost: x\r\n\r\n" +
                  "GET /fast HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                  1500);
  check(buf.indexOf("SLOW2") >= 0,
        "pipelined close behind a parked response: the parked response survives");
  check(buf.indexOf("SLOW2") < buf.indexOf("FAST"),
        "and both responses arrive in request order");

});
test("an abandoned response promise fabricates no response", async () => {
  buf = await raw("GET /abandon HTTP/1.1\r\nHost: x\r\n\r\n", 800);
  eq(respCount(buf), 0,
     "abandoned response promise: no fabricated response, connection closed");


});
test("a dynamic throw message with NULs survives byte-exact", async () => {
    b = await raw("GET /nulthrow HTTP/1.1\r\nHost: x\r\n\r\n", 600);
    p = jsonOf(b);
    eq(p && p.error, "Error: before\u0000AFTER-tail\u0000MORE",
       "dynamic throw: message survives BOTH NULs byte-exact");
    check(bodyOf(b).indexOf("AFTER-tail") >= 0 && bodyOf(b).indexOf("MORE") >= 0,
          "dynamic throw: both tails present in the raw body");
    eq((bodyOf(b).split("\\u0000").length - 1), 2,
       "dynamic throw: exactly two \\u0000 escapes (no C-string truncation)");

});
test("a middleware throw message with NULs survives byte-exact", async () => {
    b = await raw("GET /nulmw HTTP/1.1\r\nHost: x\r\n\r\n", 600);
    p = jsonOf(b);
    eq(p && p.error, "Error: m1\u0000m2-tail\u0000m3",
       "middleware throw: message survives BOTH NULs byte-exact");

});
test("rpc: a thrown message with NULs survives byte-exact, with its id", async () => {
    b = await raw(rpcReq('{"jsonrpc":"2.0","method":"nulthrow","id":8}'), 600);
    p = jsonOf(b);
    eq(p && p.error && p.error.message, "Error: r1\u0000r2-tail\u0000r3",
       "rpc throw: error.message survives BOTH NULs byte-exact");
    eq(p && p.id, 8, "rpc throw: the id rides along");

});
test("rpc: a rejected promise message with NULs survives byte-exact", async () => {
    b = await raw(rpcReq('{"jsonrpc":"2.0","method":"nulrej","id":9}'), 600);
    p = jsonOf(b);
    eq(p && p.error && p.error.message, "Error: p1\u0000p2-tail\u0000p3",
       "rpc rejected promise: message survives BOTH NULs byte-exact");

});
test("rpc: a rejecting thenable message with NULs survives byte-exact", async () => {
    b = await raw(rpcReq('{"jsonrpc":"2.0","method":"nulthen","id":10}'), 600);
    p = jsonOf(b);
    eq(p && p.error && p.error.message, "Error: t1\u0000t2-tail\u0000t3",
       "rpc rejecting thenable: message survives BOTH NULs byte-exact");
});
test("a settle after close writes nothing", async () => {
  buf = raw("GET /late HTTP/1.1\r\nHost: x\r\n\r\n", 1500);
  for (let i = 0; i < 100 && !late; i++)
    await new Promise((r) => setTimeout(r, 10));
  check(late !== null, "the parked handler exposed its settle");
  app.close();
  closed = true;
  if (late) late("too-late");
  buf = await buf;
  eq(respCount(buf), 0, "settle after close writes nothing");

}, { timeoutMs: 10000 });

await t.run();
