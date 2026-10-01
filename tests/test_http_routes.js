import { App, HTTPClient, TCPServer } from "dyna:net";

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

{
  let m = throws(() => app.get("", () => ""), "empty pattern refused");
  check(/non-empty/.test(m), "error says non-empty");
  m = throws(() => app.get("u/:id", () => ""), "pattern without / refused");
  check(/'\/'/.test(m) || /start/.test(m), "error names the leading /");
  m = throws(() => app.get("/u?x=1", () => ""), "query in pattern refused");
  m = throws(() => app.get("/u#f", () => ""), "fragment in pattern refused");
  m = throws(() => app.get("/a//b", () => ""), "empty segment refused");
  m = throws(() => app.get("/:", () => ""), "':param' without name refused");
  m = throws(() => app.get("/*", () => ""), "'*rest' without name refused");
  m = throws(() => app.get("/a:b", () => ""), "':' mid-segment refused");
  m = throws(() => app.get("/a*b", () => ""), "'*' mid-segment refused");
  m = throws(() => app.get("/:a-b", () => ""), "bad param char refused");
  m = throws(() => app.get("/:a/:a", () => ""), "duplicate param name refused");
  {
    let p = "/";
    for (let i = 0; i < 17; i++) p += ":n" + i + "/";
    m = throws(() => app.get(p.slice(0, -1), () => ""), "more than 16 named captures refused");
    check(/16/.test(m), "error names the 16-capture limit");
  }
  m = throws(() => app.get("/*a/*b", () => ""), "two wildcards refused");
  m = throws(() => app.get("/*a/b", () => ""), "wildcard must be last");
  m = throws(() => app.get("/x/:" + "z".repeat(70), () => ""),
             "over-long param name refused");
  m = throws(() => app.get("/" + "s".repeat(1100), () => ""),
             "pattern over 1024 bytes refused");
  m = throws(() => app.get("/ok"), "missing handler refused");
  m = throws(() => app.get("/ok", 42), "non-function handler refused");
  check(typeof app.get("/ok1", () => "ok") === "object", "register returns this");
}

app.get("/u/:id", (ctx) => ({ id: ctx.params.id }));
app.get("/u/:id/friends/:fid", (ctx) => ctx.params);
app.get("/files/*rest", (ctx) => "rest=" + JSON.stringify(ctx.params.rest));
app.get("/fixed", (ctx) => "params=" + JSON.stringify(ctx.params));
app.get("/v/", () => "trailing-ok");
app.get("/me/first", () => "first-registered");
app.get("/me/:who", () => "second-registered");
app.post("/u/:id", (ctx) => "posted:" + ctx.params.id);
app.get("/q", (ctx) => "q=" + JSON.stringify(ctx.query));
app.get("/hd", (ctx) => "h=" + JSON.stringify(ctx.headers["x-custom"]) +
                        "/" + JSON.stringify(ctx.headers["x-dup"]) +
                        "/" + ctx.method);
app.get("/hh", (ctx) => JSON.stringify([ctx.headers.accept, ctx.headers.cookie,
                                        ctx.headers["set-cookie"]]));
app.post("/body", (ctx) => "b=" + ctx.body);
app.get("/env/:k", (ctx) => {
  switch (ctx.params.k) {
    case "str": return "plain";
    case "obj": return { a: 1 };
    case "env201": return { status: 201, body: "made" };
    case "envct": return { contentType: "text/csv", body: "a,b" };
    case "envjson": return { body: { s: 1 } };
    case "envbytes": return { body: new Uint8Array([1, 2, 3]) };
    case "num": return 42;
    case "nul": return null;
    case "undef": return undefined;
    case "async": return Promise.resolve("resolved");
    case "reject": return Promise.reject(new Error("nope"));
    default: return { status: 418, body: "teapot" };
  }
});

app.get("/pp/:name/:__proto__", (ctx) =>
  JSON.stringify(Object.keys(ctx.params).sort()) + "|" +
  JSON.stringify(ctx.params.name) + "|" +
  JSON.stringify(ctx.params["__proto__"]) + "|" +
  JSON.stringify(Object.keys(ctx.query)) + "|" +
  JSON.stringify(ctx.query["__proto__"]) + "|" +
  JSON.stringify(({}).polluted));

app.get("/thrownul", () => { throw new Error("before\u0000AFTER-KEEPS"); });
app.get("/thrownul2", () => { throw new Error("before\u0000AFTER-tail\u0000MORE"); });
app.get("/envplain", () => ({}));
app.get("/envown", () => {
  const o = {};
  Object.defineProperty(o, "body", {
    get() { return "OWN-ACCESSOR-BODY"; }, enumerable: true });
  return o;
});
app.get("/envnullproto", () => Object.create(null));
app.get("/envarr", () => [1, 2]);
app.get("/envundef", () => ({ body: undefined, contentType: "text/custom" }));

const base = () => "http://127.0.0.1:" + app.port;
app.start();
const c = new HTTPClient();

(async () => {
  let r = await c.getAsync(base() + "/u/42");
  eq(r.status, 200, "GET /u/42 status");
  eq(JSON.parse(r.body).id, "42", ":param extraction");

  r = await c.getAsync(base() + "/u/a%20b%2Fc");
  eq(JSON.parse(r.body).id, "a b/c", ":param value is percent-decoded (%2F included)");

  r = await c.getAsync(base() + "/u/plus+sign");
  eq(JSON.parse(r.body).id, "plus+sign", "'+' stays literal in path params");

  r = await c.getAsync(base() + "/u/7/friends/9");
  eq(JSON.parse(r.body), { id: "7", fid: "9" }, "two params extracted");

  r = await c.getAsync(base() + "/u/");
  eq(r.status, 404, ":param matches NO empty segment (/u/)");
  r = await c.getAsync(base() + "/u");
  eq(r.status, 404, ":param matches NO missing segment (/u)");
  r = await c.getAsync(base() + "/u/1/x");
  eq(r.status, 404, ":param spans exactly one segment (/u/1/x)");

  r = await c.getAsync(base() + "/files/a/b%20c.txt");
  eq(JSON.parse(r.body.slice(5)), "a/b c.txt", "*rest captures the remainder (decoded)");
  r = await c.getAsync(base() + "/files");
  eq(JSON.parse(r.body.slice(5)), "", "*rest on the bare prefix is the empty string");
  r = await c.getAsync(base() + "/files/");
  eq(JSON.parse(r.body.slice(5)), "", "*rest on the prefix+/ is the empty string");

  r = await c.getAsync(base() + "/fixed");
  eq(r.body, "params={}", "a pattern with no params yields an empty params object");

  r = await c.getAsync(base() + "/v");
  eq(r.body, "trailing-ok", "a trailing slash in the pattern is not significant");
  r = await c.getAsync(base() + "/v/");
  eq(r.body, "trailing-ok", "either direction");

  r = await c.getAsync(base() + "/me/first");
  eq(r.body, "first-registered", "first registered match wins (static beats later :param)");
  r = await c.getAsync(base() + "/me/other");
  eq(r.body, "second-registered", "later :param still matches what the static misses");

  r = await c.postAsync(base() + "/u/5", "x", { "Content-Type": "text/plain" });
  eq(r.body, "posted:5", "POST dispatches to the POST route");
  r = await c.requestAsync("PUT", base() + "/u/5", "", {});
  eq(r.status, 405, "pattern matched, method not -> 405");
  eq(JSON.parse(r.body).error, "method not allowed", "405 body names the shape");
  r = await c.requestAsync("HEAD", base() + "/fixed", "", {});
  eq(r.status, 405, "HEAD is NOT aliased to GET (exact method match)");
  r = await c.getAsync(base() + "/nothing-here");
  eq(r.status, 404, "nothing matched -> 404 (typed fallback)");

  r = await c.getAsync(base() + "/q?a=1&b=two+words&c&d=x%3Dy&d=z&e=a%2Bb");
  const q = JSON.parse(r.body.slice(2));
  eq(q.a, "1", "query a");
  eq(q.b, "two words", "query '+' decodes to space");
  eq(q.c, "", "bare key is present with empty value");
  eq(q.d, "z", "repeated key: last wins");
  eq(q.e, "a+b", "query %2B decodes to '+'");
  r = await c.getAsync(base() + "/q?a+b=c+d");
  const qk = JSON.parse(r.body.slice(2));
  eq(qk["a b"], "c d", "query '+' decodes to space in KEYS too (as documented)");

  r = await c.getAsync(base() + "/hd", { "X-Custom": "V1", "X-Dup": "a" });
  const hb = r.body.slice(2).split("/");
  eq(JSON.parse(hb[0]), "V1", "headers carry custom names lower-cased");
  eq(hb[2], "GET", "ctx.method is the request method");

  {
    const KEEP = [];
    const raw = (req) => new Promise((resolve) => {
      let buf = "";
      const conn = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
        connect: (s) => { s.write(new TextEncoder().encode(req)); },
        data: (s, b) => {
          buf += new TextDecoder().decode(b);
          const sp = buf.indexOf("\r\n\r\n");
          const cl = /Content-Length: (\d+)/i.exec(buf);
          if (sp >= 0 && cl && buf.length >= sp + 4 + parseInt(cl[1], 10)) {
            try { s.close(); } catch (e) {}
            resolve(buf);
          }
        },
        close: () => resolve(buf),
      });
      KEEP.push(conn);
      setTimeout(() => resolve(buf), 2000);
    });
    const bodyOf = (s) => s.slice(s.indexOf("\r\n\r\n") + 4);
    const H = (extra) => "GET /hh HTTP/1.1\r\nHost: x\r\nConnection: close\r\n" +
                         extra + "\r\n\r\n";
    let rr = await raw(H("Accept: a\r\nAccept: b"));
    eq(JSON.parse(bodyOf(rr))[0], "a, b", "repeated list fields join with ', '");
    rr = await raw(H("Cookie: a=1\r\nCookie: b=2"));
    eq(JSON.parse(bodyOf(rr))[1], "a=1; b=2", "repeated Cookie joins with '; '");
    rr = await raw(H("Set-Cookie: x=1\r\nSet-Cookie: y=2"));
    eq(JSON.parse(bodyOf(rr))[2], "x=1", "repeated Set-Cookie keeps the first (uncombinable)");
    rr = await raw("GET /hh HTTP/1.1\r\nHost: x\r\nX-Dup: one\r\nX-Dup: two\r\nConnection: close\r\n\r\n");
    check(/400/.test(rr) && /duplicate header/.test(bodyOf(rr)),
          "any other duplicate header is refused with 400");
    for (const s of KEEP) { try { s.close(); } catch (e) {} }
  }

  r = await c.postAsync(base() + "/body", "hello there", { "Content-Type": "text/plain" });
  eq(r.body, "b=hello there", "ctx.body is the raw request body");

  r = await c.getAsync(base() + "/pp/v1/v2?__proto__=zz");
  eq(r.body,
     '["__proto__","name"]|"v1"|"v2"|["__proto__"]|"zz"|undefined',
     "__proto__ param names and query keys land as own data (no pollution)");

  r = await c.getAsync(base() + "/env/str");
  eq([r.status, r.headers["Content-Type"], r.body], [200, "text/plain", "plain"],
     "string -> 200 text/plain");
  r = await c.getAsync(base() + "/env/obj");
  eq([r.status, JSON.parse(r.body)], [200, { a: 1 }], "plain object -> 200 JSON");
  r = await c.getAsync(base() + "/env/env201");
  eq([r.status, r.body], [201, "made"], "envelope status+body");
  r = await c.getAsync(base() + "/env/envct");
  eq([r.headers["Content-Type"], r.body], ["text/csv", "a,b"],
     "envelope contentType wins for a string body");
  r = await c.getAsync(base() + "/env/envjson");
  eq([r.headers["Content-Type"], JSON.parse(r.body)], ["application/json", { s: 1 }],
     "non-string envelope body JSON-encodes");
  r = await c.getAsync(base() + "/env/envbytes");
  eq([r.headers["Content-Type"], r.body.length], ["application/octet-stream", 3],
     "byte view -> octet-stream");
  r = await c.getAsync(base() + "/env/num");
  eq(JSON.parse(r.body), 42, "number -> JSON");
  r = await c.getAsync(base() + "/env/nul");
  eq(r.body, "null", "null -> JSON null");
  r = await c.getAsync(base() + "/env/undef");
  eq(r.status, 500, "undefined handler result -> 500 (a handler must answer)");
  r = await c.getAsync(base() + "/env/async");
  eq([r.status, r.body], [200, "resolved"], "an awaited handler settles the response");
  r = await c.getAsync(base() + "/env/reject");
  eq(r.status, 500, "a rejected handler is a 500");
  check(/nope/.test(r.body), "and the rejection message is in the body");

  {
    const KEEP = [];
    const rawA = (req, waitMs) => new Promise((resolve) => {
      let buf = "";
      const conn = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
        connect: (s) => { s.write(new TextEncoder().encode(req)); },
        data: (s, b) => { buf += new TextDecoder().decode(b); },
        close: () => resolve(buf),
      });
      KEEP.push(conn);
      setTimeout(() => { try { conn.close(); } catch (e) {} resolve(buf); },
                 waitMs || 2000);
    });
    const rr = await rawA(
      "DELETE /u/5 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    check(/405/.test(rr), "DELETE on a GET+POST route is a 405");
    const am = /^Allow:\s*(.*)$/mi.exec(rr);
    check(am !== null, "the 405 carries an Allow header (RFC 9110 MUST)");
    if (am) {
      const methods = am[1].trim().split(/,\s*/);
      eq(methods.sort(), ["GET", "POST"],
         "Allow lists exactly the methods registered for the path");
    }
    for (const s of KEEP) { try { s.close(); } catch (e) {} }
  }

  r = await c.getAsync(base() + "/thrownul");
  eq(r.status, 500, "throw -> 500");
  {
    let parsed = null;
    try { parsed = JSON.parse(r.body); } catch (e) {}
    check(parsed !== null && parsed.error.indexOf("before") >= 0 &&
          parsed.error.indexOf("AFTER-KEEPS") >= 0,
          "thrown message survives an embedded NUL: " + JSON.stringify(r.body));
  }

  r = await c.getAsync(base() + "/thrownul2");
  {
    let parsed = null;
    try { parsed = JSON.parse(r.body); } catch (e) {}
    eq(parsed && parsed.error, "Error: before\u0000AFTER-tail\u0000MORE",
       "two NULs + tails survive the JSON escape byte-exact");
    check(r.body.indexOf("AFTER-tail") >= 0 && r.body.indexOf("MORE") >= 0,
          "and the raw body carries both tails past both NULs");
    eq((r.body.split("\\u0000").length - 1), 2,
       "and exactly two \\u0000 escapes appear (no truncation at either)");
  }

  Object.prototype.body = "PWNED-INHERITED-BODY";
  Object.prototype.status = 999;
  r = await c.getAsync(base() + "/envplain");
  eq([r.status, r.headers["Content-Type"], r.body],
     [200, "application/json", "{}"],
     "inherited body/status do NOT make {} an envelope (own properties only)");
  delete Object.prototype.body;
  delete Object.prototype.status;

  {
    Object.prototype.body = "PWNED-INHERITED-BODY";
    r = await c.getAsync(base() + "/envplain");
    eq([r.status, r.body], [200, "{}"],
       "inherited `body` alone INERT (plain {} stays JSON)");
    delete Object.prototype.body;

    Object.prototype.status = 999;
    r = await c.getAsync(base() + "/envplain");
    eq(r.status, 200, "inherited `status` alone INERT (no forced 500)");
    delete Object.prototype.status;

    Object.defineProperty(Object.prototype, "body", {
      get() { return "PWNED-LATE-GETTER"; }, configurable: true });
    r = await c.getAsync(base() + "/envplain");
    eq([r.status, r.body], [200, "{}"],
       "late defineProperty getter `body` INERT on the envelope read");
    delete Object.prototype.body;

    Object.defineProperty(Object.prototype, "status", {
      get() { return 999; }, configurable: true });
    r = await c.getAsync(base() + "/envplain");
    eq(r.status, 200, "late defineProperty getter `status` INERT");
    delete Object.prototype.status;

    Object.prototype[Symbol.for("body")] = "PWNED-SYM";
    Object.prototype[Symbol.for("status")] = 999;
    r = await c.getAsync(base() + "/envplain");
    eq([r.status, r.body], [200, "{}"], "symbol-keyed gadgets INERT");
    delete Object.prototype[Symbol.for("body")];
    delete Object.prototype[Symbol.for("status")];

    r = await c.getAsync(base() + "/envown");
    eq([r.status, r.body], [200, "OWN-ACCESSOR-BODY"],
       "own `body` accessor is read like any property read");

    r = await c.getAsync(base() + "/envnullproto");
    eq([r.status, r.body], [200, "{}"], "null-prototype return: plain JSON");
    r = await c.getAsync(base() + "/envarr");
    eq([r.status, r.body], [200, "[1,2]"], "array return: plain JSON");

    r = await c.getAsync(base() + "/envundef");
    eq([r.status, r.headers["Content-Type"], r.body],
       [200, "application/json", "{\"contentType\":\"text/custom\"}"],
       "own body:undefined reads as ABSENT (the object stays plain JSON)");

    eq([({}).body, ({}).response], [undefined, undefined],
       "Object.prototype restored clean after the gadget rows");
  }

  c.close();
  app.close();
  print("test_http_routes: " + n + " checks, " + fails + " failures");
  if (fails) throw new Error(fails + " failures");
})();
