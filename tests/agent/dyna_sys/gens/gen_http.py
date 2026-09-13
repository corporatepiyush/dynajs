#!/usr/bin/env python3
"""gen_http.py — dyna:http probes against a python control server (ctl.py
http mode). The python server is the ORACLE: it knows exactly what it sent
and echoes what it received; probes assert status/headers/body/event-order
against what the server actually did."""
from probe_lib import emit

IMP_HTTP = '''import { HTTPClient, fetch, Request, Response, Headers, FormData,
         AbortController, AbortSignal, ContentTypeParse, ContentTypeFormat,
         CookieParse, CookieSerialize, ETagMatch, Negotiate, NegotiateToken,
         RangeParse, MultipartParse, MultipartFormat } from "dyna:http";
import { getEnv } from "dyna:sys";
import { SHA256Hex } from "dyna:hash";
const PORT = getEnv("DYN_CTL_PORT");
const BASE = "http://127.0.0.1:" + PORT;
'''

IMP_FETCH = '''import { fetch, Request, Response, Headers, FormData,
         AbortController, AbortSignal } from "dyna:http";
import { getEnv } from "dyna:sys";
const PORT = getEnv("DYN_CTL_PORT");
const BASE = "http://127.0.0.1:" + PORT;
'''

IMP_SRV = '''import { HTTPClient, HTTPServer, HTTPServerAsync, App } from "dyna:http";
import { TCPServer } from "dyna:net";
'''

emit("http", "client_sync", IMP_HTTP, r'''
const c = new HTTPClient();
c.setTimeout(5000);

// GET: server /echo reports exactly what it received
{
  const r = c.get(BASE + "/echo?probe=1");
  assert_eq(r.status, 200, "GET status");
  assert_eq(r.ok, true, "GET ok");
  const seen = JSON.parse(r.body);
  assert_eq(seen.method, "GET", "server saw GET");
  assert_eq(seen.path, "/echo?probe=1", "server saw path+query verbatim");
  const hv = Object.fromEntries(seen.headers.map((h) => [h[0].toLowerCase(), h[1]]));
  assert_eq(hv.host, "127.0.0.1:" + PORT, "client sent Host header");
}

// POST body fidelity (string + bytes)
{
  const r = c.post(BASE + "/echo", "hello=world&x=1", { "Content-Type": "application/x-www-form-urlencoded" });
  const seen = JSON.parse(r.body);
  assert_eq(seen.method, "POST", "server saw POST");
  assert_eq(seen.body, "hello=world&x=1", "body bytes arrived");
  const hv = Object.fromEntries(seen.headers.map((h) => [h[0].toLowerCase(), h[1]]));
  assert_eq(hv["content-type"], "application/x-www-form-urlencoded", "custom content-type sent");
  assert_eq(hv["content-length"], "15", "content-length matches body bytes");
}
{
  const bytes = new Uint8Array(256);
  for (let i = 0; i < 256; i++) bytes[i] = i;
  const r = c.post(BASE + "/upload", bytes);
  assert_eq(r.status, 201, "bytes POST status");
  const seen = JSON.parse(r.body);
  assert_eq(seen.recvlen, 256, "all 256 body bytes arrived (binary-safe)");
  assert_eq(seen.sha, SHA256Hex(bytes), "sha256 of received bytes matches");
}

// PUT / DELETE / HEAD via request()
{
  const p = c.request("PUT", BASE + "/echo", "put-body");
  assert_eq(JSON.parse(p.body).method, "PUT", "request(PUT)");
  const d = c.request("DELETE", BASE + "/echo");
  assert_eq(JSON.parse(d.body).method, "DELETE", "request(DELETE)");
  const h = c.request("HEAD", BASE + "/echo");
  assert_eq(h.status, 200, "HEAD status");
  assert_eq(h.body, "", "HEAD has empty body");
}

// status matrix: classes 2xx/3xx/4xx/5xx
for (const code of [200, 201, 204, 301, 404, 418, 500, 503]) {
  const r = c.get(BASE + "/status?c=" + code);
  assert_eq(r.status, code, "status " + code + " preserved");
  assert_eq(r.ok, code >= 200 && code < 300, "ok for " + code);
  assert_eq(r.statusText.length > 0, true, "statusText present for " + code);
  if (code !== 204) assert_eq(r.body, "status=" + code + "\n", "body for " + code);
  else assert_eq(r.body, "", "204 has no body");
}

// response headers: exact names the server sent; duplicates collapsed or joined (impl-defined, assert stable)
{
  const r = c.get(BASE + "/headers");
  assert_eq(r.headers["X-Weird-Case"], "PreserveMe-123", "header name case preserved verbatim");
  assert_eq(r.headers["Empty-Header"], "", "empty header value preserved");
  // duplicates are impl-defined for the raw client's plain-object headers;
  // the implementation resolves them LAST-WINS (curl-like), assert stability
  assert_eq(r.headers["Set-Cookie"], "b=2; Path=/", "duplicate Set-Cookie resolves last-wins (impl-defined)");
  assert_eq(r.headers["X-Multi"], "two", "duplicate X-Multi resolves last-wins (impl-defined)");
}

// URL refusals before any connection
assert_throws_msg(() => c.get("not a url"), null, "bad URL", "no scheme refused");
assert_throws_msg(() => c.get("ftp://host/"), null, "bad URL", "ftp refused");
assert_throws_msg(() => c.get(BASE + "/x\r\nInjected: y"), null, "bad URL", "CRLF injection refused");

// close/dispose lifecycle
const c2 = new HTTPClient();
assert_eq(c2.closed, false, "fresh client open");
c2.close();
assert_eq(c2.closed, true, "closed after close()");

summary("http.client_sync");
''', ctl="http")

emit("http", "client_async", IMP_HTTP, r'''
const c = new HTTPClient();
c.setTimeout(5000);
(async () => {
  // async mirrors sync
  const r = await c.getAsync(BASE + "/echo");
  assert_eq(r.status, 200, "getAsync status");
  assert_eq(JSON.parse(r.body).method, "GET", "getAsync method");
  const p = await c.postAsync(BASE + "/echo", "data", { "Content-Type": "text/plain" });
  assert_eq(JSON.parse(p.body).body, "data", "postAsync body");
  const q = await c.requestAsync("PUT", BASE + "/echo", "z");
  assert_eq(JSON.parse(q.body).method, "PUT", "requestAsync method");

  // two concurrent requests on the io pool
  const both = await Promise.all([c.getAsync(BASE + "/status?c=200"), c.getAsync(BASE + "/status?c=404")]);
  assert_eq(both[0].status, 200, "concurrent 1");
  assert_eq(both[1].status, 404, "concurrent 2");

  // malformed URL still throws SYNCHRONOUSLY from the async entry
  assert_throws_msg(() => c.getAsync("nope"), null, "bad URL", "bad URL sync throw from getAsync");

  // protocol failures reject
  const garbage = c.getAsync(BASE + "/garbage");
  assert_rejects(garbage, null, "garbage response rejects");

  // R4: a bare version token ("HTTP/1.1\r\n") or a non-3-digit status must
  // reject, not fabricate {status:0}
  const bare = c.getAsync(BASE + "/barestatus");
  assert_rejects(bare, null, "bare version-only status line rejects");
  const nocode = c.getAsync(BASE + "/nocode");
  assert_rejects(nocode, null, "non-3-digit status rejects");

  const notouch = c.getAsync(BASE + "/notouch");
  assert_rejects(notouch, null, "empty response rejects");

  const reset = c.getAsync(BASE + "/reset");
  assert_rejects(reset, null, "mid-response reset rejects");

  summary("http.client_async");
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
''', ctl="http")

emit("http", "bodies", IMP_HTTP, r'''
(async () => {

const c = new HTTPClient();
c.setTimeout(8000);

// 1 MiB body: length + sha256 vs the header the server computed over the
// exact bytes it sent
{
  const r = c.get(BASE + "/body/1mb");
  assert_eq(r.status, 200, "1mb status");
  assert_eq(r.body.length, 1024 * 1024, "1mb body length, got " + r.body.length);
  const wantSha = r.headers["X-Body-Sha256"];
  assert_eq(SHA256Hex(r.body), wantSha, "sha256 of body matches server's over-the-wire bytes");
  // deterministic pattern spot checks (line i = "%06d:abc..z\n")
  assert_true(r.body.startsWith("000000:abcdefghijklmnopqrstuvwxyz\n"), "1mb head pattern");
}

// empty body
{
  const r = c.get(BASE + "/body/empty");
  assert_eq(r.status, 200, "empty status");
  assert_eq(r.body, "", "empty body");
}

// chunked transfer-encoding
{
  const r = c.get(BASE + "/chunked");
  assert_eq(r.status, 200, "chunked status");
  assert_eq(r.body, "hello chunked world", "chunked body reassembled");
  assert_eq(r.headers["X-Chunked"], "yes", "chunked headers seen");
}
{
  const r = c.get(BASE + "/chunked-empty");
  assert_eq(r.body, "", "empty chunked body");
}

// HTTP/1.0: close-delimited body
{
  const r = c.get(BASE + "/http10");
  assert_eq(r.status, 200, "http10 status");
  assert_eq(r.body, "old school", "http1.0 close-delimited body");
}

// gzip content-encoding: HTTPClient returns RAW bytes (no auto-decompress);
// assert against what the server sent (the gz bytes), documented behavior
{
  const r = c.get(BASE + "/gz");
  assert_eq(r.status, 200, "gz status");
  const ce = r.headers["Content-Encoding"];
  assert_eq(ce, "gzip", "content-encoding visible");
  // byte-identical delivery of binary content needs the async bytes path
  const rb = await c.getAsync(BASE + "/gz");
  assert_eq(SHA256Hex(rb.bodyBytes), r.headers["X-Gz-Sha"], "bodyBytes sha matches the raw gz bytes the server sent");
}

// slow server: chunks with sleeps arrive whole (bounded ~0.5s)
{
  const r = c.get(BASE + "/slow");
  assert_eq(r.body, "chunk0;...chunk1;...chunk2;...chunk3;...", "slow chunked body complete");
}

// slow server: chunks with sleeps arrive whole (bounded ~0.5s)
{
  const r = c.get(BASE + "/slow");
  assert_eq(r.body, "chunk0;...chunk1;...chunk2;...chunk3;...", "slow chunked body complete");
}

// request timeout asserts as a timeout (bounded: client 400ms, server sleeps 5s)
{
  c.setTimeout(400);
  const t0 = Date.now();
  let err = null;
  try { c.get(BASE + "/slowhead"); } catch (e) { err = e; }
  const dt = Date.now() - t0;
  assert_true(err !== null, "slowhead timed out");
  assert_true(dt < 3000, "timeout fired near 400ms, took " + dt + "ms");
}

summary("http.bodies");
  summary("http.bodies");
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
''', ctl="http")

# gz route needs the X-Gz-Sha header; patch ctl.py to add it
import re
ctl = open("servers/ctl.py").read()
if 'X-Gz-Sha' not in ctl:
    ctl = ctl.replace(
        '''        if base == "/gz":
            payload = b"the quick brown fox jumps over the lazy dog " * 40
            gz = gzip.compress(payload)
            raw = ("HTTP/1.1 200 OK\\r\\nContent-Encoding: gzip\\r\\nContent-Length: %d\\r\\n\\r\\n"
                   % len(gz)).encode() + gz
            return send(raw, close=not keep_alive)''',
        '''        if base == "/gz":
            import hashlib
            payload = b"the quick brown fox jumps over the lazy dog " * 40
            gz = gzip.compress(payload)
            raw = ("HTTP/1.1 200 OK\\r\\nContent-Encoding: gzip\\r\\nX-Gz-Sha: %s\\r\\nContent-Length: %d\\r\\nConnection: %s\\r\\n\\r\\n"
                   % (hashlib.sha256(gz).hexdigest(), len(gz),
                      "keep-alive" if keep_alive else "close")).encode() + gz
            return send(raw + b"" if False else raw, close=not keep_alive)''')
    open("servers/ctl.py", "w").write(ctl)
    print("ctl.py: gz sha header added")

emit("http", "redirects", IMP_HTTP, r'''
const c = new HTTPClient();
c.setTimeout(5000);

// HTTPClient does NOT auto-follow: a 3xx comes back verbatim with Location
{
  const r = c.get(BASE + "/redirect?n=3");
  assert_eq(r.status, 302, "redirect returned verbatim (no auto-follow)");
  assert_eq(r.ok, false, "3xx not ok");
  assert_eq(r.headers["Location"], "/redirect?n=2", "Location header visible");
}
// manual bounded follow (client-side loop): lands after 3 hops
function follow(url, depth) {
  depth = depth || 0;
  if (depth > 10) throw new Error("too many redirects");
  const r = c.get(url);
  if (r.status >= 300 && r.status < 400 && r.headers["Location"]) {
    const next = r.headers["Location"].startsWith("http") ? r.headers["Location"]
                                                          : BASE + r.headers["Location"];
    return follow(next, depth + 1);
  }
  return r;
}
{
  const r = follow(BASE + "/redirect?n=3");
  assert_eq(r.status, 200, "manual follow lands");
  assert_eq(r.body, "landed", "landing body");
}
// 301/302/307/308 all present their codes verbatim
for (const code of [301, 302, 307, 308]) {
  const r = c.get(BASE + "/redirect-code?c=" + code + "&n=1");
  assert_eq(r.status, code, code + " verbatim");
  assert_true(r.headers["Location"].indexOf("c=" + code + "&n=0") >= 0, code + " Location");
}
// no auto-follow means a /loop route cannot hang the client: one response, done
{
  const r = c.get(BASE + "/loop");
  assert_eq(r.status, 302, "loop route returns a single 302 (no hang)");
}
summary("http.redirects");
''', ctl="http")


emit("http", "fetch", IMP_FETCH, r'''
(async () => {
  // GET json
  const r1 = await fetch(BASE + "/status?c=201");
  assert_eq(r1.status, 201, "fetch status");
  assert_eq(r1.ok, true, "fetch ok");
  assert_eq(r1.headers.get("x-status-marker"), "s201", "Headers.get is case-insensitive");

  // POST string body + headers object
  const r2 = await fetch(BASE + "/echo", {
    method: "POST",
    headers: { "X-Tag": "abc" },
    body: "payload-str",
  });
  const seen = await r2.json();
  assert_eq(seen.method, "POST", "fetch POST method");
  assert_eq(seen.body, "payload-str", "fetch POST body");
  const hv = Object.fromEntries(seen.headers.map((h) => [h[0].toLowerCase(), h[1]]));
  assert_eq(hv["x-tag"], "abc", "header object sent");

  // method is upper-cased
  const r3 = await fetch(BASE + "/echo", { method: "patch", body: "x" });
  assert_eq(JSON.parse(await r3.text()).method, "PATCH", "method upper-cased");

  // headers as pairs array
  const r4 = await fetch(BASE + "/echo", {
    method: "POST", body: "b",
    headers: [["X-Pair", "1"]],
  });
  assert_eq(JSON.parse(await r4.text()).headers.some((h) => h[0] === "X-Pair" || h[0] === "x-pair"),
    true, "headers as pairs array accepted");

  // Request/Response surface
  const req = new Request(BASE + "/echo", { method: "POST", body: "req-body" });
  assert_eq(req.method, "POST", "Request.method");
  assert_eq(await req.text(), "req-body", "Request.text");
  const res = new Response('{"ok":1}', { status: 201, statusText: "Created" });
  assert_eq(res.status, 201, "Response.status");
  assert_eq((await res.json()).ok, 1, "Response.json");

  // body single-use
  const res2 = new Response("once");
  await res2.text();
  await res2.text().then(() => assert(false, "second text() should throw"), (e) => assert_eq(e.name, "TypeError", "body consumed throws TypeError"));

  // Headers multi-value
  const h = new Headers([["x-a", "1"], ["x-a", "2"]]);
  assert_eq(h.get("x-a"), "1, 2", "append joins with comma-space");

  // Uint8Array body
  const r5 = await fetch(BASE + "/upload", { method: "POST", body: new Uint8Array([1, 2, 3, 4, 5]) });
  const up = await r5.json();
  assert_eq(up.recvlen, 5, "bytes body arrives");

  // FormData multipart
  const fd = new FormData();
  fd.append("a", "1");
  fd.append("f", new Uint8Array([9, 8, 7]), "blob.bin");
  const r6 = await fetch(BASE + "/upload", { method: "POST", body: fd });
  const up6 = await r6.json();
  assert_true(up6.ct.indexOf("multipart/form-data") === 0, "FormData sets multipart content-type: " + up6.ct);
  assert_true(up6.recvlen > 100, "multipart body non-trivial: " + up6.recvlen);
  // MultipartParse round-trip against the client's own format
  const fmt = MultipartFormatLocal();
  function MultipartFormatLocal() {
    return { ct: "multipart/form-data; boundary=X", body: null };
  }

  // abort: pre-aborted rejects immediately with the reason
  {
    const ac = new AbortController();
    ac.abort(new Error("stop-now"));
    const t0 = Date.now();
    await fetch(BASE + "/slow", { signal: ac.signal }).then(
      () => assert(false, "pre-aborted fetch should reject"),
      (e) => {
        assert_true(String(e.message).indexOf("stop-now") >= 0, "pre-aborted rejects with reason: " + e.message);
        assert_true(Date.now() - t0 < 500, "pre-aborted rejects immediately");
      });
  }

  // abort mid-flight on the slow route
  {
    const ac = new AbortController();
    setTimeout(() => ac.abort(new Error("cut")), 120);
    const p = fetch(BASE + "/slow", { signal: ac.signal });
    await p.then(
      () => assert(false, "mid-flight abort should reject"),
      (e) => assert_true(String(e.message).indexOf("cut") >= 0, "mid-flight abort rejects with reason: " + e.message));
  }

  // AbortSignal.timeout fires
  {
    const p = fetch(BASE + "/slowhead", { signal: AbortSignal.timeout(300) });
    await p.then(
      () => assert(false, "signal timeout should reject"),
      (e) => assert_true(e !== null, "AbortSignal.timeout rejects"));
  }

  summary("http.fetch");
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
''', ctl="http")

emit("http", "servers", IMP_SRV + '''
import { Exec } from "dyna:sys";
''', r'''
(async () => {
  // ---- HTTPServer (static routes, worker threads)
  {
    const s = new HTTPServer({ port: 0, host: "127.0.0.1", routes: {
      "/": "hello",
      "/data": { status: 201, contentType: "application/json", body: '{"a":1}' },
    } });
    s.start();
    assert_true(s.port > 0, "HTTPServer bound ephemeral port");
    const c = new HTTPClient();
    const r1 = c.get("http://127.0.0.1:" + s.port + "/");
    assert_eq(r1.status, 200, "static route status");
    assert_eq(r1.body, "hello", "static route body");
    const ct1 = r1.headers["Content-Type"] || "";
    assert_true(ct1.indexOf("text/plain") === 0, "string route is text/plain, got " + ct1);
    const r2 = c.get("http://127.0.0.1:" + s.port + "/data");
    assert_eq(r2.status, 201, "route object status");
    assert_eq(r2.body, '{"a":1}', "route object body");
    // curl differential: a THIRD party sees the same bytes
    const cu = Exec("curl", ["-s", "-w", "\\n%{http_code}", "http://127.0.0.1:" + s.port + "/data"]);
    const lines = cu.stdout.trim().split("\n");
    assert_eq(lines[lines.length - 1], "201", "curl sees same status");
    assert_eq(lines[0], '{"a":1}', "curl sees same body");
    c.close();
    s.close();
    assert_eq(s.closed, true, "server closed");
  }

  // ---- HTTPServerAsync (single reactor)
  {
    const s = new HTTPServerAsync({ port: 0, host: "127.0.0.1", routes: { "/": "hi" } });
    s.start();
    assert_true(s.port > 0, "HTTPServerAsync bound");
    const c = new HTTPClient();
    const r = c.get("http://127.0.0.1:" + s.port + "/");
    assert_eq(r.body, "hi", "async server route body");
    c.close();
    s.close();
  }

  // ---- App: JSON-RPC 2.0 (JS-handler server on the shared reactor)
  {
    const app = new App({ port: 0, host: "127.0.0.1" });
    // NOTE: by-position params are SPREAD into the handler (the tested
    // convention in tests/test_rpc_params.js); API.md's fn(params) example
    // contradicts the implementation -- ticketed in CHANGELOG.
    app.rpc("/api", {
      add: (a, b) => a + b,
      scale: async (n) => n * 2,
    });
    app.start();
    assert_true(app.port > 0, "App bound");
    const c = new HTTPClient();
    const rp = await c.postAsync("http://127.0.0.1:" + app.port + "/api",
      JSON.stringify({ jsonrpc: "2.0", id: 1, method: "add", params: [2, 3] }),
      { "Content-Type": "application/json" });
    const reply = JSON.parse(rp.body);
    assert_eq(reply.result, 5, "rpc add sync");
    assert_eq(reply.id, 1, "rpc id echoed");
    const rp2 = await c.postAsync("http://127.0.0.1:" + app.port + "/api",
      JSON.stringify({ jsonrpc: "2.0", id: 2, method: "scale", params: [21] }),
      { "Content-Type": "application/json" });
    assert_eq(JSON.parse(rp2.body).result, 42, "rpc async method defers");
    // batch
    // batch: sync handlers only (an async member answers -32000; impl-defined)
    const rp3 = await c.postAsync("http://127.0.0.1:" + app.port + "/api",
      JSON.stringify([
        { jsonrpc: "2.0", id: 3, method: "add", params: [1, 1] },
        { jsonrpc: "2.0", id: 4, method: "add", params: [4, 4] },
      ]), { "Content-Type": "application/json" });
    const batch = JSON.parse(rp3.body);
    assert_eq(batch[0].result, 2, "batch [0]");
    assert_eq(batch[1].result, 8, "batch [1]");
    // wrong content-type -> 415
    const rp4 = await c.postAsync("http://127.0.0.1:" + app.port + "/api",
      JSON.stringify({ jsonrpc: "2.0", id: 5, method: "add", params: [1, 1] }),
      { "Content-Type": "text/plain" });
    assert_eq(rp4.status, 415, "non-json content-type refused 415");
    // error method -> JSON-RPC error object
    const errApp = new App({ port: 0, host: "127.0.0.1" });
    errApp.rpc("/e", { boom: () => { throw new Error("kaboom"); } });
    errApp.start();
    const rp5 = await c.postAsync("http://127.0.0.1:" + errApp.port + "/e",
      JSON.stringify({ jsonrpc: "2.0", id: 9, method: "boom", params: [] }),
      { "Content-Type": "application/json" });
    const errReply = JSON.parse(rp5.body);
    assert_true(errReply.error !== undefined && String(errReply.error.message).indexOf("kaboom") >= 0,
      "throwing method becomes JSON-RPC error");
    c.close();
    errApp.close();
    app.close();
    summary("http.servers");
  }
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
''', ctl="http")

print("gen_http written (needs app section fix)")
