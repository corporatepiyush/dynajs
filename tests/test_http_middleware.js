/* test_http_middleware.js --: App.use(fn) middleware chain.
 *
 * The chain contract under test: middleware runs in registration order
 * before the matched dynamic handler, sees (and may annotate) the SAME
 * request context the handler gets, and can stop the chain by returning
 * { response }. A throw mid-chain is a 500 whose JSON is WELL-FORMED even
 * when the message carries quotes and newlines (the message is data, not
 * body bytes), and it is the END of the chain. Middleware runs for matched
 * DYNAMIC routes only -- not for typed routes (rpc/static/upload/ws/sse)
 * and not for misses -- so it gates handlers, never the server plumbing.
 * In-process, async client: see test_http_routes.js for why.
 */
import { App, HTTPClient } from "dyna:net";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));
const throws = (fn, m) => {
  let t = false, msg = "";
  try { fn(); } catch (e) { t = true; msg = String(e.message || e); }
  check(t, m);
  return msg;
};

const app = new App({ port: 0 });

/* registration contract */
{
  const m1 = throws(() => app.use(), "use() without fn refused");
  const m2 = throws(() => app.use(42), "use(42) refused");
  check(/function/.test(m2), "error names the function requirement");
  check(typeof app.use(() => undefined) === "object", "use returns this");
  /* the throwaway middleware registered above runs on every dynamic route;
     it returns undefined and changes nothing (pinned below) */
}

const log = [];
app.use((ctx) => { log.push("one:" + ctx.path); });
app.use((ctx) => {
  log.push("two:" + ctx.path);
  if (ctx.path === "/stop") return { response: { status: 401, body: "denied" } };
  if (ctx.path === "/stopstr") return { response: "stopped" };
  if (ctx.path === "/stopasync") return { response: Promise.resolve("async-stop") };
  if (ctx.path === "/throw")
    throw new Error('mw "quoted"\nnewline');
  if (ctx.path === "/asyncmw") return Promise.resolve({ response: "no" });
  ctx.user = "annotated";   /* handler must see this */
});
app.use((ctx) => { log.push("three:" + ctx.path); });

app.get("/stop", () => { log.push("handler:/stop"); return "should-not-run"; });
app.get("/stopstr", () => { log.push("handler:/stopstr"); return "should-not-run"; });
app.get("/stopasync", () => { log.push("handler:/stopasync"); return "should-not-run"; });
app.get("/throw", () => { log.push("handler:/throw"); return "should-not-run"; });
app.get("/asyncmw", () => { log.push("handler:/asyncmw"); return "should-not-run"; });
app.get("/who", (ctx) => "user=" + ctx.user);
app.get("/plain", () => "fine");
app.get("/stopacc", () => "should-not-run");
app.get("/mwmpty", () => "HANDLER-RAN");
{
  /* own `response` accessor on a middleware's return: still read; a plain
     {} return is the shape whose `response` lookup must be OWN-only */
  const o = {};
  Object.defineProperty(o, "response", {
    get() { return { status: 402, body: "OWN-ACCESSOR-RESPONSE" }; },
    enumerable: true });
  app.use((ctx) => {
    if (ctx.path === "/stopacc") return o;
    if (ctx.path === "/mwmpty") return {};
  });
}
app.rpc("/rpc", { ping: () => "pong" });

const base = () => "http://127.0.0.1:" + app.port;
app.start();
const c = new HTTPClient();

(async () => {
  let r = await c.getAsync(base() + "/who");
  eq([r.status, r.body], [200, "user=annotated"],
     "middleware annotates the ctx the handler receives");
  eq(log, ["one:/who", "two:/who", "three:/who"],
     "chain runs in registration order");

  log.length = 0;
  r = await c.getAsync(base() + "/stop");
  eq([r.status, r.body], [401, "denied"], "{response} short-circuits with it");
  eq(log, ["one:/stop", "two:/stop"],
     "chain STOPS at the short-circuit: later middleware and handler untouched");

  log.length = 0;
  r = await c.getAsync(base() + "/stopstr");
  eq([r.status, r.body], [200, "stopped"], "a plain {response} value sends like a handler's");

  log.length = 0;
  r = await c.getAsync(base() + "/stopasync");
  eq([r.status, r.body], [200, "async-stop"],
     "a thenable {response} is awaited before it is sent");

  log.length = 0;
  r = await c.getAsync(base() + "/throw");
  eq(r.status, 500, "a middleware throw is a 500");
  let parsed = null, parseOk = true;
  try { parsed = JSON.parse(r.body); } catch (e) { parseOk = false; }
  check(parseOk, "and the body is WELL-FORMED JSON despite quotes/newlines in the message");
  check(parsed && /quoted/.test(parsed.error) && /newline/.test(parsed.error),
        "the escaped message survives intact");
  eq(log, ["one:/throw", "two:/throw"],
     "the chain ENDS at the throw: middleware three and the handler never run");

  log.length = 0;
  r = await c.getAsync(base() + "/asyncmw");
  eq(r.status, 500, "an async (thenable) middleware is refused with 500");
  check(/async middleware/.test(r.body), "and says so");
  eq(log, ["one:/asyncmw", "two:/asyncmw"], "the chain ends there too");

  log.length = 0;
  r = await c.getAsync(base() + "/plain");
  eq([r.status, r.body], [200, "fine"], "a non-short-circuiting chain still reaches the handler");
  eq(log, ["one:/plain", "two:/plain", "three:/plain"], "all three ran");

  /* scope contract: middleware gates DYNAMIC handlers only */
  log.length = 0;
  r = await c.getAsync(base() + "/missing");
  eq(r.status, 404, "a miss is still a 404");
  eq(log, [], "middleware does NOT run for an unmatched path");
  log.length = 0;
  const rpc = await c.postAsync(base() + "/rpc",
    JSON.stringify({ jsonrpc: "2.0", id: 1, method: "ping" }),
    { "Content-Type": "application/json" });
  eq(JSON.parse(rpc.body).result, "pong", "typed (rpc) routes still dispatch");
  eq(log, [], "middleware does NOT run for typed routes");

  /* the throwaway registration-time middleware changed nothing */
  r = await c.getAsync(base() + "/plain");
  eq(r.status, 200, "a middleware returning undefined is a no-op");

  /* the short-circuit reads OWN properties only: an inherited `response`
     must not hijack a middleware that returned {} (the lookup runs on the
     middleware's own return value -- a plain {} is the gadget's target) */
  Object.prototype.response = { status: 418, body: "PWNED-INHERITED" };
  r = await c.getAsync(base() + "/mwmpty");
  eq([r.status, r.body], [200, "HANDLER-RAN"],
     "inherited `response` does not short-circuit a middleware that returned {}");
  delete Object.prototype.response;

  /* -- prototype-gadget property reads on the middleware `response`
   *   lookup: inherited, late defineProperty getter, symbol keys ------- */
  Object.defineProperty(Object.prototype, "response", {
    get() { return { status: 418, body: "PWNED-LATE-GETTER" }; },
    configurable: true });
  r = await c.getAsync(base() + "/mwmpty");
  eq([r.status, r.body], [200, "HANDLER-RAN"],
     "late defineProperty getter `response` INERT on the middleware lookup");
  delete Object.prototype.response;

  Object.prototype[Symbol.for("response")] = { status: 418, body: "PWNED-SYM" };
  r = await c.getAsync(base() + "/mwmpty");
  eq([r.status, r.body], [200, "HANDLER-RAN"],
     "symbol-keyed `response` INERT on the middleware lookup");
  delete Object.prototype[Symbol.for("response")];

  /* the response that IS an own property still short-circuits (incl. an
     own accessor: a property read runs getters) */
  r = await c.getAsync(base() + "/stopacc");
  eq([r.status, r.body], [402, "OWN-ACCESSOR-RESPONSE"],
     "own `response` accessor still short-circuits with its value");

  eq((({}).response), undefined, "Object.prototype restored clean");

  c.close();
  app.close();
  print("test_http_middleware: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
})();
