// Parametric black-box contract test, generated from dynajs.d.ts (pre-split slice). Engine sources not consulted.
// Pure (socket-free) surface of dyna:http + exactly ONE loopback integration block at the end.
import "../httpc.js";
import {
  ContentTypeParse, ContentTypeFormat, CookieParse, CookieSerialize, ETagMatch,
  Negotiate, NegotiateToken, RangeParse, MultipartParse, MultipartFormat,
  Request, Response, Headers, FormData, AbortController, AbortSignal,
  App, HTTPClient, HTTPServer, HTTPServerAsync, fetch,
} from "dyna:http";
import { Exec, Which } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
// awaits the call: a synchronous throw OR a rejected promise both count
async function assertThrowsAsync(call, msg, errPattern) {
  n++; let threw = false, e = null;
  try { const out = call(); if (out && typeof out.then === "function") await out; }
  catch (err) { threw = true; e = err; }
  if (!threw) throw new Error("expected throw: " + msg);
  if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg);
}
async function runRows(tableName, rows, fn) {
  for (let i = 0; i < rows.length; i++) {
    const row = rows[i];
    const label = tableName + "[" + i + "] " + row[0];
    try { await fn(row); }
    catch (e) { throw new Error("row failed — " + label + ": " + (e && e.message ? e.message : String(e))); }
  }
}
const bytesEq = (u8, arr) => u8.length === arr.length && arr.every((v, i) => u8[i] === v);
const getHeader = (rec, name) => { const k = Object.keys(rec).find(k2 => k2.toLowerCase() === name); return k === undefined ? undefined : rec[k]; };
const sleep2 = (ms) => new Promise((r) => setTimeout(r, ms));

// DOC-TENSION: the slice comment for Negotiate says "the best candidate INDEX" but its declared
// return type is `string | null`; the most specific documented reading (the type signature, and
// the documented `-> string|null` in the reference) is that the candidate STRING is returned.
// All Negotiate rows below expect candidate strings on that basis.

await runRows("ContentTypeParse", [
  ["plain", "text/html", { type: "text", subtype: "html", parameters: {} }],
  ["charset-param", "text/html; charset=utf-8", { type: "text", subtype: "html", parameters: { charset: "utf-8" } }],
  ["case-folded", "Application/JSON", { type: "application", subtype: "json", parameters: {} }],
  ["param-name-lowercased", "text/plain; CharSet=UTF-8", { type: "text", subtype: "plain", parameters: { charset: "UTF-8" } }],
  ["quoted-value-unquoted", 'multipart/form-data; boundary="a;b"', { type: "multipart", subtype: "form-data", parameters: { boundary: "a;b" } }],
  ["empty-is-null", "", null],
  ["bare-token-is-null", "textonly", null],
], async ([, header, expected]) => {
  const ct = ContentTypeParse(header);
  if (expected === null) { assertEq(ct, null, "expected null parse"); return; }
  assert(ct !== null, "expected a parse, got null");
  assertEq(ct.type, expected.type, "type");
  assertEq(ct.subtype, expected.subtype, "subtype");
  assertDeepEq(ct.parameters, expected.parameters, "parameters");
});

await runRows("ContentTypeFormat", [
  ["bare", { type: "text", subtype: "html" }, (s) => assertEq(s, "text/html", "bare format")],
  ["with-params", { type: "text", subtype: "html", parameters: { charset: "utf-8" } }, (s) => { assert(s.startsWith("text/html"), "starts with type/subtype"); assert(s.includes("charset=utf-8"), "includes charset param, got |" + s + "|"); }],
  ["quotes-when-needed", { type: "application", subtype: "form-data", parameters: { boundary: "a;b" } }, (s) => assert(s.includes('"a;b"'), "needs quoting, got |" + s + "|")],
], async ([, ct, check]) => check(ContentTypeFormat(ct)));

{ // round-trip: format(parse(h)) re-parses to the same structure
  const h = "text/html; charset=utf-8";
  const once = ContentTypeParse(h);
  const twice = ContentTypeParse(ContentTypeFormat(once));
  n++; if (JSON.stringify(once) !== JSON.stringify(twice)) throw new Error("row failed — ContentTypeFormat[round-trip]: " + JSON.stringify(twice) + " vs " + JSON.stringify(once));
}

await runRows("CookieParse", [
  ["two-cookies", "a=1; b=2", { a: "1", b: "2" }],
  ["quoted-drops-quotes", 'a=1; b="two words"', { a: "1", b: "two words" }],
  ["duplicate-last-wins", "a=1; a=2", { a: "2" }],
  ["single", "k=v", { k: "v" }],
], async ([, header, expected]) => assertDeepEq(CookieParse(header), expected, "cookie parse"));

await runRows("CookieSerialize", [
  ["plain", "sid", "abc", undefined, (call) => assertEq(call(), "sid=abc", "plain serialize")],
  ["maxage-path-httponly", "sid", "abc", { maxAge: 3600, path: "/", httpOnly: true }, (call) => {
    const s = call();
    assert(s.includes("sid=abc"), "name=value present");
    assert(/max-age=3600/i.test(s), "Max-Age present, got |" + s + "|");
    assert(/path=\//i.test(s), "Path present, got |" + s + "|");
    assert(/httponly/i.test(s), "HttpOnly present, got |" + s + "|");
  }],
  ["domain-samesite-secure", "k", "v", { domain: "example.com", sameSite: "Strict", secure: true }, (call) => {
    const s = call();
    assert(s.includes("k=v"), "name=value present");
    assert(/domain=example\.com/i.test(s), "Domain present, got |" + s + "|");
    assert(/samesite=strict/i.test(s), "SameSite present, got |" + s + "|");
    assert(/secure/i.test(s), "Secure present, got |" + s + "|");
  }],
  ["refusal-name-space", "bad name", "v", undefined, (call) => assertThrows(call, "name with space refused", TypeError)],
  ["refusal-name-semicolon", "bad;name", "v", undefined, (call) => assertThrows(call, "name with ; refused", TypeError)],
  ["refusal-value-semicolon", "k", "a;b", undefined, (call) => assertThrows(call, "value with ; refused", TypeError)],
  ["refusal-value-comma", "k", "a,b", undefined, (call) => assertThrows(call, "value with , refused", TypeError)],
  ["refusal-value-quote", "k", 'a"b', undefined, (call) => assertThrows(call, "value with quote refused", TypeError)],
  ["refusal-value-backslash", "k", "a\\b", undefined, (call) => assertThrows(call, "value with backslash refused", TypeError)],
  ["refusal-value-control", "k", "a\nb", undefined, (call) => assertThrows(call, "value with control byte refused", TypeError)],
  ["refusal-strict-opts", "k", "v", { bogus: 1 }, (call) => assertThrows(call, "unknown opts key refused", TypeError, /bogus/)],
], async ([, name, value, opts, check]) => check(() => CookieSerialize(name, value, opts)));

await runRows("ETagMatch", [
  ["exact", '"v1"', '"v1"', true],
  ["weak-validator-header", 'W/"v1"', '"v1"', true],
  ["weak-validator-etag", '"v1"', 'W/"v1"', true],
  ["star-matches-anything", "*", '"x"', true],
  ["mismatch", '"v1"', '"v2"', false],
  ["list-hit", '"a", "b"', '"b"', true],
  ["list-miss", '"a", "b"', '"c"', false],
], async ([, header, etag, expected]) => assertEq(ETagMatch(header, etag), expected, "etag match"));

await runRows("Negotiate", [
  ["q-decides", "text/html;q=1.0, text/plain;q=0.5", ["text/plain", "text/html"], "text/html"],
  ["empty-header-first-candidate", "", ["text/plain", "text/html"], "text/plain"],
  ["wildcard-first-candidate", "text/*", ["text/plain", "text/html"], "text/plain"],
  ["q0-refusal", "text/html;q=0", ["text/html"], null],
  ["no-match-null", "image/png", ["text/plain", "text/html"], null],
  ["higher-q-wins", "text/plain;q=0.5, text/html;q=0.8", ["text/plain", "text/html"], "text/html"],
  ["specificity-beats-q", "text/*;q=1.0, text/html;q=0.5", ["text/html", "text/plain"], "text/html"],
], async ([, header, candidates, expected]) => assertEq(Negotiate(header, candidates), expected, "negotiate"));

await runRows("NegotiateToken", [
  ["implicit-q1-beats-explicit", "gzip, deflate;q=0.8", ["deflate", "gzip"], "gzip"],
  ["dash-prefix-match", "en-US", ["en"], "en"],
  ["q-decides", "en;q=0.5, fr;q=1.0", ["fr", "en"], "fr"],
  ["no-match-null", "zz", ["en"], null],
  ["star-first-candidate", "*", ["a", "b"], "a"],
], async ([, header, candidates, expected]) => assertEq(NegotiateToken(header, candidates), expected, "negotiate token"));

await runRows("RangeParse", [
  ["inclusive", "bytes=0-4", 100, [{ start: 0, end: 4 }]],
  ["suffix", "bytes=-5", 100, [{ start: 95, end: 99 }]],
  ["past-resource", "bytes=500-", 100, "unsatisfiable"],
  ["open-end-clamps", "bytes=95-", 100, [{ start: 95, end: 99 }]],
  ["closed-end-clamps", "bytes=0-1000", 100, [{ start: 0, end: 99 }]],
  ["fully-past", "bytes=200-300", 100, "unsatisfiable"],
  ["single-byte", "bytes=0-0", 100, [{ start: 0, end: 0 }]],
  ["start-equals-size", "bytes=100-", 100, "unsatisfiable"],
  ["malformed-null", "junk", 100, null],
  ["empty-spec-null", "bytes=", 100, null],
], async ([, header, size, expected]) => {
  const r = RangeParse(header, size);
  if (typeof expected === "string") assertEq(r, expected, "range result");
  else assertDeepEq(r, expected, "range result");
});

await runRows("MultipartFormat", [
  ["string-field", [{ name: "a", value: "1" }], undefined, (fmt) => {
    assert(fmt.contentType.startsWith("multipart/form-data; boundary="), "contentType carries generated boundary, got |" + fmt.contentType + "|");
    assert(fmt.body instanceof Uint8Array, "body is Uint8Array");
  }],
  ["file-field", [{ name: "f", body: new Uint8Array([1, 2, 3]), filename: "b.bin" }], undefined, (fmt) => {
    assert(fmt.contentType.startsWith("multipart/form-data; boundary="), "contentType");
    assert(fmt.body instanceof Uint8Array, "body");
  }],
  ["explicit-boundary", [{ name: "a", value: "1" }], "xyzbound", (fmt) => {
    assert(fmt.contentType.includes("xyzbound"), "explicit boundary appears, got |" + fmt.contentType + "|");
  }],
], async ([, parts, boundary, check]) => check(MultipartFormat(parts, boundary)));

{ // MultipartParse round trip over a shared formatted body
  const parts = [
    { name: "a", value: "1" },
    { name: "f", body: new Uint8Array([1, 2, 3]), filename: "b.bin" },
  ];
  const fmt = MultipartFormat(parts);
  await runRows("MultipartParse", [
    ["order-and-names", fmt.contentType, fmt.body, (out) => { n++; if (out.length !== 2 || out[0].name !== "a" || out[1].name !== "f") throw new Error("part order/names, got |" + JSON.stringify(out.map(p => p.name)) + "|"); }],
    ["string-part-body", fmt.contentType, fmt.body, (out) => { assertEq(new TextDecoder().decode(out[0].body), "1", "string part payload"); }],
    ["file-part-filename", fmt.contentType, fmt.body, (out) => { assertEq(out[1].filename, "b.bin", "filename"); }],
    ["file-part-bytes", fmt.contentType, fmt.body, (out) => { assert(bytesEq(out[1].body, [1, 2, 3]), "file bytes"); }],
    ["parts-are-uint8array", fmt.contentType, fmt.body, (out) => { assert(out[0].body instanceof Uint8Array, "part payload type"); }],
  ], async ([, ct, body, check]) => check(MultipartParse(ct, body)));
}

await runRows("Headers", [
  ["append-pairs-join", null, (t) => { const h = new Headers([["x-a", "1"], ["x-a", "2"]]); assertEq(h.get("x-a"), "1, 2", "append joins with ', '"); }],
  ["get-missing-null", null, (t) => { const h = new Headers([["x-a", "1"]]); assertEq(h.get("none"), null, "absent get"); }],
  ["has", null, (t) => { const h = new Headers([["x-a", "1"]]); assertEq(h.has("x-a"), true, "has present"); assertEq(h.has("x-b"), false, "has absent"); }],
  ["names-stored-lowercased", null, (t) => { const h = new Headers({ "X-A": "1" }); assertEq(h.get("x-a"), "1", "lowercased lookup"); assertEq(h.get("X-A"), "1", "case-insensitive lookup"); }],
  ["case-insensitive-set", null, (t) => { const h = new Headers({ a: "1" }); h.set("A", "2"); assertEq(h.get("a"), "2", "set through other case"); }],
  ["set-trims", null, (t) => { const h = new Headers(); h.set("x-b", " hi "); assertEq(h.get("x-b"), "hi", "set trims value"); }],
  ["append-trims-and-joins", null, (t) => { const h = new Headers(); h.set("x-b", "hi"); h.append("x-b", " lo "); assertEq(h.get("x-b"), "hi, lo", "append joins"); }],
  ["delete-removes", null, (t) => { const h = new Headers([["x-a", "1"]]); h.delete("x-a"); assertEq(h.get("x-a"), null, "after delete get"); assertEq(h.has("x-a"), false, "after delete has"); }],
  ["entries", null, (t) => { const h = new Headers([["a", "1"], ["b", "2"]]); assertDeepEq([...h.entries()], [["a", "1"], ["b", "2"]], "entries"); }],
  ["keys", null, (t) => { const h = new Headers([["a", "1"], ["b", "2"]]); assertDeepEq([...h.keys()], ["a", "b"], "keys"); }],
  ["values", null, (t) => { const h = new Headers([["a", "1"], ["b", "2"]]); assertDeepEq([...h.values()], ["1", "2"], "values"); }],
  ["symbol-iterator", null, (t) => { const h = new Headers([["a", "1"]]); assertDeepEq([...h], [["a", "1"]], "iterator pairs"); }],
  ["forEach-args", null, (t) => { const h = new Headers([["a", "1"]]); let seen = null; h.forEach((v, k, parent) => { seen = [v, k, parent === h]; }); assertDeepEq(seen, ["1", "a", true], "forEach (value, name, parent)"); }],
  ["init-from-headers", null, (t) => { const h = new Headers([["a", "1"]]); const g = new Headers(h); assertEq(g.get("a"), "1", "copy init"); }],
], async ([, , check]) => check());

await runRows("Request", [
  ["method-uppercased", null, async (t) => {
    const r = new Request("http://example.test/api", { method: "post", body: "x=1&y=2", headers: { "x-token": "t" } });
    assertEq(r.method, "POST", "method upper-cased");
    assertEq(r.url, "http://example.test/api", "url");
    assertEq(r.headers.get("x-token"), "t", "headers");
  }],
  ["default-method-get", null, async (t) => { assertEq(new Request("http://e.test/").method, "GET", "default method"); }],
  ["copy-carries-body", null, async (t) => {
    const r = new Request("http://e.test/", { method: "POST", body: "x=1&y=2" });
    const c = new Request(r);
    assertEq(c.method, "POST", "copied method");
    assertEq(await c.text(), "x=1&y=2", "copied body");
  }],
  ["copy-override-method", null, async (t) => {
    const r = new Request("http://e.test/", { method: "POST", body: "x=1" });
    assertEq(new Request(r, { method: "GET" }).method, "GET", "init overrides method");
  }],
  ["text", null, async (t) => { assertEq(await new Request("http://e.test/", { body: "hello" }).text(), "hello", "text"); }],
  ["empty-body-text", null, async (t) => { assertEq(await new Request("http://e.test/").text(), "", "empty body -> empty string"); }],
  ["bytes", null, async (t) => { const b = await new Request("http://e.test/", { body: "abc" }).bytes(); assert(b instanceof Uint8Array, "bytes type"); assertEq(b.length, 3, "bytes length"); }],
  ["arrayBuffer", null, async (t) => { const b = await new Request("http://e.test/", { body: "abcd" }).arrayBuffer(); assertEq(b.byteLength, 4, "arrayBuffer byteLength"); }],
  ["json", null, async (t) => { assertDeepEq(await new Request("http://e.test/", { body: '{"a":1}' }).json(), { a: 1 }, "json"); }],
  ["json-rejects", null, async (t) => { await assertThrowsAsync(() => new Request("http://e.test/", { body: "nope" }).json(), "json syntax error rejects"); }],
  ["signal-default-null", null, async (t) => { assertEq(new Request("http://e.test/").signal, null, "default signal"); }],
  ["signal-passthrough", null, async (t) => { const ac = new AbortController(); const r = new Request("http://e.test/", { signal: ac.signal }); assertEq(r.signal, ac.signal, "signal carried"); assertEq(r.signal.aborted, false, "not aborted"); }],
], async ([, , check]) => check());

await runRows("Response", [
  ["defaults", null, async (t) => { const r = new Response(); assertEq(r.status, 200, "status"); assertEq(r.statusText, "OK", "statusText"); assertEq(r.ok, true, "ok"); assertEq(r.url, "", "url"); assertEq(r.bodyUsed, false, "bodyUsed"); }],
  ["full-init", null, async (t) => {
    const r = new Response('{"ok":1}', { status: 201, statusText: "Created", headers: { "content-type": "application/json" }, url: "http://e.test/" });
    assertEq(r.status, 201, "status"); assertEq(r.statusText, "Created", "statusText"); assertEq(r.ok, true, "ok"); assertEq(r.url, "http://e.test/", "url");
    assertEq(r.headers.get("content-type"), "application/json", "headers");
  }],
  ["ok-below-200", null, async (t) => { assertEq(new Response(null, { status: 199 }).ok, false, "199 not ok"); }],
  ["ok-at-200", null, async (t) => { assertEq(new Response(null, { status: 200 }).ok, true, "200 ok"); }],
  ["ok-at-299", null, async (t) => { assertEq(new Response(null, { status: 299 }).ok, true, "299 ok"); }],
  ["ok-above-299", null, async (t) => { assertEq(new Response(null, { status: 300 }).ok, false, "300 not ok"); }],
  ["text", null, async (t) => { assertEq(await new Response("hello").text(), "hello", "text"); }],
  ["bytes", null, async (t) => { const b = await new Response(new Uint8Array([1, 2, 3])).bytes(); assertEq(b.length, 3, "len"); assertEq(b[1], 2, "elem"); }],
  ["arrayBuffer", null, async (t) => { assertEq((await new Response(new Uint8Array([1, 2, 3, 4])).arrayBuffer()).byteLength, 4, "byteLength"); }],
  ["json", null, async (t) => { assertDeepEq(await new Response('{"ok":1}').json(), { ok: 1 }, "json"); }],
  ["json-rejects", null, async (t) => { await assertThrowsAsync(() => new Response("{bad").json(), "json parse error rejects"); }],
  ["body-single-use", null, async (t) => { const r = new Response("x"); await r.text(); await assertThrowsAsync(() => r.text(), "second read throws consumed"); }],
  ["clone-before-consume", null, async (t) => { const r = new Response("x"); const c = r.clone(); await r.text(); assertEq(await c.text(), "x", "clone reads same body"); }],
  ["clone-after-consume", null, async (t) => { const r = new Response("x"); await r.text(); await assertThrowsAsync(() => r.clone(), "clone of consumed throws", /clone a consumed response/); }],
  ["bodyUsed-flips", null, async (t) => { const r = new Response("x"); await r.text(); assertEq(r.bodyUsed, true, "bodyUsed after read"); }],
  ["no-body-read-still-fine", null, async (t) => { assertEq(await new Response().text(), "", "null body reads as empty"); }],
], async ([, , check]) => check());

await runRows("FormData", [
  ["multi-append", null, (t) => { const f = new FormData(); f.append("a", "1"); f.append("a", "2"); assertEq(f.get("a"), "1", "get first"); assertDeepEq(f.getAll("a"), ["1", "2"], "getAll"); }],
  ["has", null, (t) => { const f = new FormData(); f.append("f", new Uint8Array([1, 2]), "blob.bin"); assertEq(f.has("f"), true, "has"); assertEq(f.has("x"), false, "has absent"); }],
  ["bytes-value", null, (t) => { const f = new FormData(); f.append("f", new Uint8Array([1, 2]), "blob.bin"); const v = f.get("f"); assert(v instanceof Uint8Array, "byte value type"); assertDeepEq(Array.from(v), [1, 2], "byte value"); }],
  ["set-replaces", null, (t) => { const f = new FormData(); f.append("a", "1"); f.append("a", "2"); f.set("a", "9"); assertDeepEq(f.getAll("a"), ["9"], "set replaces all"); }],
  ["delete", null, (t) => { const f = new FormData(); f.append("a", "1"); f.delete("a"); assertEq(f.has("a"), false, "after delete has"); assertEq(f.get("a"), null, "after delete get"); }],
  ["getAll-missing-empty", null, (t) => { assertDeepEq(new FormData().getAll("nope"), [], "getAll absent"); }],
  ["keys-order", null, (t) => { const f = new FormData(); f.append("a", "1"); f.append("f", "x"); assertDeepEq([...f.keys()], ["a", "f"], "insertion order"); }],
  ["entries", null, (t) => { const f = new FormData(); f.append("a", "1"); assertDeepEq([...f.entries()].map(([k, v]) => [k, v]), [["a", "1"]], "entries"); }],
  ["forEach-args", null, (t) => { const f = new FormData(); f.append("a", "1"); let seen = null; f.forEach((v, k, parent) => { seen = [v, k, parent === f]; }); assertDeepEq(seen, ["1", "a", true], "forEach (value, name, parent)"); }],
], async ([, , check]) => check());

await runRows("Abort", [
  ["initially-live", null, (t) => { const ac = new AbortController(); assertEq(ac.signal.aborted, false, "not aborted"); }],
  ["abort-default-reason", null, (t) => { const ac = new AbortController(); ac.abort(); assertEq(ac.signal.aborted, true, "aborted"); assert(ac.signal.reason instanceof Error, "default reason is an Error"); }],
  ["abort-custom-reason", null, (t) => { const ac = new AbortController(); ac.abort("why"); assertEq(ac.signal.reason, "why", "custom reason"); }],
  ["throwIfAborted", null, (t) => { const ac = new AbortController(); ac.signal.throwIfAborted(); ac.abort("boom"); assertThrows(() => ac.signal.throwIfAborted(), "throwIfAborted throws after abort"); }],
  ["static-abort", null, (t) => { const s = AbortSignal.abort("r"); assertEq(s.aborted, true, "static abort"); assertEq(s.reason, "r", "static reason"); }],
  ["onabort-fires", null, (t) => { const ac = new AbortController(); let calls = 0; ac.signal.onabort = () => { calls++; }; ac.abort(); assertEq(calls, 1, "onabort fired once"); }],
], async ([, , check]) => check());

/* ------------------------------------------------------------------ *
 *  ONE loopback integration block: App on 127.0.0.1 port 0, a handful
 *  of documented routes, then teardown in finally.
 * ------------------------------------------------------------------ */
{
  const app = new App({ port: 0, host: "127.0.0.1" });
  // middleware short-circuit (documented: returning {response} stops the chain)
  app.use((req) => { if (req.path === "/blocked") return { response: { status: 401, body: "denied" } }; });
  app.get("/json/:id", (req) => ({ id: req.params.id, q: req.query }));          // plain object -> JSON 200
  app.post("/echo", (req) => ({ status: 201, body: req.body }));                  // string body -> text/plain
  app.get("/status/418", () => ({ status: 418, body: "teapot" }));
  app.get("/hdr", (req) => ({ status: 200, body: req.headers["x-token"] || "none", contentType: "text/plain" }));
  app.get("/p/:name", (req) => req.params.name);                                  // string -> 200 text/plain
  app.post("/onlypost", () => "ok");                                              // GET on it -> 405
  app.get("/blocked", () => "never");
  app.start();
  assert(app.port > 0, "ephemeral port resolved, got " + app.port);

  const client = new HTTPClient();
  client.setTimeout(2000);
  const base = "http://127.0.0.1:" + app.port;
  try {
    await runRows("loopback:App", [
      ["json-route-params-query", null, async () => {
        const r = await client.getAsync(base + "/json/7?full=1&flag");
        assertEq(r.status, 200, "status"); assertEq(r.ok, true, "ok");
        const v = JSON.parse(r.body);
        assertEq(v.id, "7", ":param captured");
        assertDeepEq(v.q, { full: "1", flag: "" }, "query decoded; bare key is empty string");
      }],
      ["post-echo-envelope", null, async () => {
        const r = await client.postAsync(base + "/echo", "hi there");
        assertEq(r.status, 201, "status"); assertEq(r.body, "hi there", "echoed body");
      }],
      ["custom-status-not-ok", null, async () => {
        const r = await client.getAsync(base + "/status/418");
        assertEq(r.status, 418, "status"); assertEq(r.ok, false, "ok false outside 2xx"); assertEq(r.body, "teapot", "body");
      }],
      ["request-headers-lowercased", null, async () => {
        const r = await client.getAsync(base + "/hdr", { "X-Token": "t" });
        assertEq(r.status, 200, "status"); assertEq(r.body, "t", "header name lower-cased server-side");
      }],
      ["param-percent-decoded", null, async () => {
        const r = await client.getAsync(base + "/p/a%20b");
        assertEq(r.status, 200, "status"); assertEq(r.body, "a b", ":param percent-decoded");
        const ct = getHeader(r.headers, "content-type");
        assert(typeof ct === "string" && ct.includes("text/plain"), "string return is text/plain, got |" + ct + "|");
      }],
      ["method-mismatch-405", null, async () => {
        const r = await client.getAsync(base + "/onlypost");
        assertEq(r.status, 405, "GET on a POST-only pattern is 405");
      }],
      ["middleware-short-circuit", null, async () => {
        const r = await client.getAsync(base + "/blocked");
        assertEq(r.status, 401, "middleware response short-circuits"); assertEq(r.body, "denied", "middleware body");
      }],
      ["global-fetch", null, async () => {
        const fr = await fetch(base + "/json/9?x=2");
        assertEq(fr.status, 200, "fetch status");
        assertEq((await fr.json()).id, "9", "fetch json body");
        const ct = fr.headers.get("content-type");
        assert(typeof ct === "string" && ct.includes("application/json"), "JSON route content type, got |" + ct + "|");
      }],
    ], async ([, , check]) => check());
  } finally {
    client.close();
    app.close();
  }
}

/* ------------------------------------------------------------------ *
 *  Second loopback integration block: HTTPServer (thread-pool) and
 *  HTTPServerAsync (reactor) smoke — d.ts L2069-2115. "A route table:
 *  path -> literal body, or {status, contentType, body}. Static only."
 *  Ephemeral port (127.0.0.1:0), one static route per server via
 *  fetch, stop()/close() idempotence, teardown in finally.
 * ------------------------------------------------------------------ */
{
  const routes = {
    "/hello": "world",
    "/json": { status: 201, contentType: "application/json", body: '{"ok":true}' },
  };

  // HTTPServer: thread-pool static server (d.ts L2079-2096)
  const srv = new HTTPServer({ port: 0, host: "127.0.0.1", routes });
  try {
    srv.start();
    assertEq(srv.port > 0, true, "HTTPServer ephemeral port resolved, got " + srv.port);
    const base = "http://127.0.0.1:" + srv.port;
    await runRows("loopback:HTTPServer", [
      ["string-route-body", null, async () => {
        const fr = await fetch(base + "/hello");
        assertEq(fr.status, 200, "literal-body route answers 200");
        assertEq(await fr.text(), "world", "literal body served verbatim");
      }],
      ["object-route-status-and-contentType", null, async () => {
        const fr = await fetch(base + "/json");
        assertEq(fr.status, 201, "the route object's status is honored");
        const ct = fr.headers.get("content-type");
        assert(typeof ct === "string" && ct.includes("application/json"), "the route object's contentType is honored, got |" + ct + "|");
        assertDeepEq(await fr.json(), { ok: true }, "the route object's body is served");
      }],
    ], async ([, , check]) => check());
    // "stop(): void" / "close(): void" — calling twice must not throw (idempotent teardown);
    // "readonly closed: boolean" is true once close() has run.
    srv.stop(); srv.stop();
    srv.close(); srv.close();
    assertEq(srv.closed, true, "HTTPServer reports closed after close()");
  } finally {
    srv.close();
  }

  // HTTPServerAsync: single-threaded reactor (d.ts L2098-2115)
  const asrv = new HTTPServerAsync({ port: 0, host: "127.0.0.1", routes });
  try {
    asrv.start();
    assertEq(asrv.port > 0, true, "HTTPServerAsync ephemeral port resolved, got " + asrv.port);
    const abase = "http://127.0.0.1:" + asrv.port;
    await runRows("loopback:HTTPServerAsync", [
      ["static-route-body", null, async () => {
        const fr = await fetch(abase + "/hello");
        assertEq(fr.status, 200, "literal-body route answers 200");
        assertEq(await fr.text(), "world", "literal body served verbatim");
      }],
      ["object-route-body", null, async () => {
        const fr = await fetch(abase + "/json");
        assertEq(fr.status, 201, "the route object's status is honored");
        assertDeepEq(await fr.json(), { ok: true }, "the route object's body is served");
      }],
    ], async ([, , check]) => check());
    asrv.stop(); asrv.stop();
    asrv.close(); asrv.close();
    assertEq(asrv.closed, true, "HTTPServerAsync reports closed after close()");
  } finally {
    asrv.close();
  }
}

/* ------------------------------------------------------------------ *
 *  Third loopback block: HTTP framing semantics cluster.
 *
 *  C1: a HEAD or 304 response has NO body regardless of Content-Length
 *  (RFC 9110 §8.6: "A server MAY send a Content-Length header field in a
 *  response to a HEAD request"; a 304 likewise never carries body bytes)
 *  -- the client reader must not wait for the advertised bytes.
 *  C4: statuses 1xx/204/304 must not emit Content-Length, Content-Type
 *  or body bytes on the SERVER side (RFC 9110 §8.6: "A server MUST NOT
 *  send Content-Length in any response with a status code of 1xx
 *  (Informational) or 204 (No Content)"), so a handler envelope
 *  {status: 204, body: "x"} answers headers-only with the body dropped.
 *  S3: HTTPClient.getStream parses framing exactly like request(): an
 *  exact "chunked" TOKEN match (never a substring probe), conflicting
 *  duplicate Content-Length refused, Content-Length + Transfer-Encoding:
 *  chunked refused (d.ts getStream: "Rejects on ... malformed chunked
 *  framing").
 *  C11: the stream head parse skips 1xx interim responses (same as the
 *  client reader's existing 1xx skip).
 *  C3: HEAD requests are answered headers-only (Content-Length of the
 *  would-be body kept for keep-alive framing) on every App route type,
 *  and the connection stays framing-clean for a follow-up request.
 *  Raw canned responses come from a dyna:net TCPServer on port 0.
 * ------------------------------------------------------------------ */
{
  const { TCPServer } = await import("dyna:net");
  // One-shot canned HTTP responder: replies to the first received bytes,
  // then closes (each row builds its own responder on its own port).
  const canned = (reply) => {
    const srv = new TCPServer({ port: 0 });
    let done = false;
    srv.start({
      data(conn) {
        if (done) return;
        done = true;
        const out = reply();
        if (out !== null) conn.write(out);
        conn.close();
      },
    });
    return srv;
  };
  const client2 = new HTTPClient();
  client2.setTimeout(2000);
  try {
    await runRows("framing:client-reader", [
      ["HEAD with Content-Length completes with an empty body (RFC 9110 §8.6; d.ts request())", null, async () => {
        const srv = canned(() => "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nConnection: close\r\n\r\n");
        try {
          const r = await client2.requestAsync("HEAD", "http://127.0.0.1:" + srv.port + "/x");
          assertEq(r.status, 200, "HEAD status");
          assertEq(r.body, "", "HEAD carries no body despite the declared CL");
        } finally { srv.dispose(); }
      }],
      ["304 with Content-Length completes with an empty body (RFC 9110 §8.6)", null, async () => {
        const srv = canned(() => "HTTP/1.1 304 Not Modified\r\nETag: \"abc\"\r\nContent-Length: 5\r\nConnection: close\r\n\r\n");
        try {
          const r = await client2.getAsync("http://127.0.0.1:" + srv.port + "/x");
          assertEq(r.status, 304, "304 status");
          assertEq(r.body, "", "304 carries no body despite the declared CL");
        } finally { srv.dispose(); }
      }],
      ["TE: chunkedx is NOT chunked (exact token match): CL framing applies, same as request()", null, async () => {
        const srv = canned(() => "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunkedx\r\nConnection: close\r\n\r\nhello");
        try {
          const r = await client2.getAsync("http://127.0.0.1:" + srv.port + "/x");
          assertEq(r.status, 200, "status");
          assertEq(r.body, "hello", "body read by Content-Length framing");
        } finally { srv.dispose(); }
      }],
    ], async ([, , check]) => check());

    // getStream is SYNCHRONOUS: it blocks this thread, so a dyna:net
    // reactor-driven responder could never serve it. The canned heads come
    // from a python3 raw-socket origin instead (the bb_scrape pattern),
    // gated on python3.
    if (!Which("python3")) {
      print("SKIP(dyna:http get-stream framing rows: python3 not found — raw canned heads need the mock origin)");
    } else {
      const T = makeTempDir("bb_http_framing");
      const PY = [
        "import socket, sys, threading, os",
        "threading.Timer(30, lambda: os._exit(0)).start()",
        "RESP = {",
        "  '/clte': b'HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n5\\r\\nhello\\r\\n0\\r\\n\\r\\n',",
        "  '/dupcl': b'HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\nContent-Length: 6\\r\\nConnection: close\\r\\n\\r\\nhello',",
        "  '/chunkedx': b'HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\nTransfer-Encoding: chunkedx\\r\\nConnection: close\\r\\n\\r\\nhello',",
        "  '/listte': b'HTTP/1.1 200 OK\\r\\nTransfer-Encoding: gzip, chunked\\r\\nConnection: close\\r\\n\\r\\n5\\r\\nhello\\r\\n0\\r\\n\\r\\n',",
        "  '/interim': b'HTTP/1.1 100 Continue\\r\\n\\r\\nHTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\nContent-Length: 5\\r\\nConnection: close\\r\\n\\r\\nhello',",
        "}",
        "TRAILERS = (b'HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n0\\r\\n'",
        "            + (b'x' * 62 + b'\\r\\n') * 2000 + b'\\r\\n')",
        "s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)",
        "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)",
        "s.bind(('127.0.0.1', 0))",
        "s.listen(16)",
        "open(sys.argv[1], 'w').write(str(s.getsockname()[1]))",
        "while True:",
        "    c, _ = s.accept()",
        "    try:",
        "        data = b''",
        "        while b'\\r\\n\\r\\n' not in data:",
        "            chunk = c.recv(4096)",
        "            if not chunk: break",
        "            data += chunk",
        "        path = data.split(b' ')[1].decode() if b' ' in data else '/'",
        "        c.sendall(TRAILERS if path == '/trailers' else RESP.get(path, b'HTTP/1.1 404 Not Found\\r\\nContent-Length: 0\\r\\nConnection: close\\r\\n\\r\\n'))",
        "    except Exception:",
        "        pass",
        "    finally:",
        "        c.close()",
      ];
      try {
        writeFile(new Path(T + "/framing.py"), PY.join("\n") + "\n");
        Exec("/bin/sh", ["-c", "python3 " + T + "/framing.py " + T + "/port > " + T + "/log 2>&1 &"]);
        let fport = 0;
        for (let i = 0; i < 60 && !fport; i++) {
          try { fport = parseInt(readFile(new Path(T + "/port")).trim(), 10); }
          catch (e) { await sleep2(50); }
        }
        assert(fport > 0, "framing mock started on an ephemeral port, log: " + T + "/log");
        const FB = "http://127.0.0.1:" + fport;
        const sclient = new HTTPClient();
        sclient.setTimeout(2000);
        try {
          await runRows("framing:get-stream", [
            ["Content-Length + Transfer-Encoding: chunked is refused (d.ts getStream: malformed framing rejects; request() refuses the same pair)", null, () => {
              assertThrows(() => sclient.getStream(FB + "/clte"),
                           "CL+TE conflict refused as a parse error", undefined, /malformed/);
            }],
            ["conflicting duplicate Content-Length is refused (same hardened rule as request())", null, () => {
              assertThrows(() => sclient.getStream(FB + "/dupcl"),
                           "conflicting duplicate CL refused as a parse error", undefined, /malformed/);
            }],
            ["TE: chunkedx is not a chunked token: the stream frames by Content-Length (exact token match, d.ts getStream)", null, async () => {
              const st = sclient.getStream(FB + "/chunkedx");
              assertEq(st.status, 200, "status");
              const buf = new Uint8Array(16);
              const k = await st.read(buf);
              assertEq(new TextDecoder().decode(buf.subarray(0, k)), "hello", "CL-framed body");
              st.close();
            }],
            ["TE: gzip, chunked IS a chunked token (list-exact match): chunked framing applies", null, async () => {
              const st = sclient.getStream(FB + "/listte");
              assertEq(st.status, 200, "status");
              const buf = new Uint8Array(16);
              const k = await st.read(buf);
              assertEq(new TextDecoder().decode(buf.subarray(0, k)), "hello", "dechunked body");
              st.close();
            }],
            ["chunked TRAILER lines are bounded by the remaining maxBody budget (a terminal chunk followed by 2000 trailer lines rejects instead of draining forever)", null, async () => {
            const tclient = new HTTPClient(1024);
            tclient.setTimeout(2000);
            try {
              const st = tclient.getStream(FB + "/trailers");
              const buf = new Uint8Array(64);
              let threw = false;
              try { await st.read(buf); } catch (e) { threw = /maxBodyBytes/.test(String(e)); if (!threw) throw e; }
              assert(threw, "trailer over-consumption rejects naming maxBodyBytes");
              st.close();
            } finally { tclient.close(); }
          }],
          ["a 1xx interim response is skipped and the real head is surfaced (same 1xx skip as request())", null, async () => {
              const st = sclient.getStream(FB + "/interim");
              assertEq(st.status, 200, "interim 100 not surfaced as final");
              assertEq(st.contentType, "text/plain", "real response headers parsed");
              const buf = new Uint8Array(16);
              const k = await st.read(buf);
              assertEq(new TextDecoder().decode(buf.subarray(0, k)), "hello", "body of the real response");
              st.close();
            }],
          ], async ([, , check]) => check());
        } finally {
          sclient.close();
        }
      } finally {
        Exec("/bin/sh", ["-c", "pkill -f '" + T + "/framing.py' >/dev/null 2>&1; true"]);
        removeAll(new Path(T));
      }
    }
  } finally {
    client2.close();
  }
}

/* ------------------------------------------------------------------ *
 *  Fourth loopback block: server-side C3/C4. An App on port 0 with a
 *  dyn route (405 on HEAD, per the d.ts method-match contract), an rpc
 *  route, a 204 envelope route, a 304 envelope route, and a proxy route
 *  backed by a canned TCPServer upstream.
 * ------------------------------------------------------------------ */
{
  const { TCPServer } = await import("dyna:net");
  const up = new TCPServer({ port: 0 });
  let upDone = false;
  up.start({
    data(conn) {
      if (upDone) return;
      upDone = true;
      conn.write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
      conn.close();
    },
  });
  const app = new App({ port: 0, host: "127.0.0.1" });
  app.get("/json/:id", (req) => ({ id: req.params.id }));
  app.post("/onlypost", () => "ok");
  app.rpc("/rpc", { ping: () => "pong" });
  app.get("/nocontent", () => ({ status: 204, body: "dropped" }));
  app.get("/notmod", () => ({ status: 304 }));
  app.proxy("/px", { host: "127.0.0.1", port: up.port });
  app.start();
  assert(app.port > 0, "ephemeral App port resolved, got " + app.port);

  const client3 = new HTTPClient();
  client3.setTimeout(2000);
  const base3 = "http://127.0.0.1:" + app.port;
  try {
    await runRows("framing:server", [
      ["204 envelope drops the body and emits no Content-Length/Content-Type (RFC 9110 §8.6)", null, async () => {
        const r = await client3.getAsync(base3 + "/nocontent");
        assertEq(r.status, 204, "status");
        assertEq(r.body, "", "handler body dropped");
        assert(getHeader(r.headers, "content-length") === undefined, "no Content-Length on 204");
        assert(getHeader(r.headers, "content-type") === undefined, "no Content-Type on 204");
      }],
      ["304 envelope answers headers-only", null, async () => {
        const r = await client3.getAsync(base3 + "/notmod");
        assertEq(r.status, 304, "status");
        assertEq(r.body, "", "no body on 304");
        assert(getHeader(r.headers, "content-length") === undefined, "no Content-Length on 304");
      }],
      ["HEAD on a dyn route is headers-only with the would-be CL (405 method mismatch per the d.ts contract)", null, async () => {
        const r = await client3.requestAsync("HEAD", base3 + "/json/7");
        assertEq(r.status, 405, "HEAD does not match a GET-only pattern");
        assertEq(r.body, "", "no body bytes on HEAD");
        assert(typeof getHeader(r.headers, "content-length") === "string", "would-be CL kept for framing");
      }],
      ["HEAD on an rpc route is headers-only", null, async () => {
        const r = await client3.requestAsync("HEAD", base3 + "/rpc");
        assertEq(r.body, "", "no body bytes on HEAD");
        assert(r.status >= 400, "HEAD dispatch refuses like a malformed rpc call, got " + r.status);
      }],
      ["HEAD through a proxy route is headers-only and relays the upstream would-be CL", null, async () => {
        const r = await client3.requestAsync("HEAD", base3 + "/px/file");
        assertEq(r.status, 200, "upstream status relayed");
        assertEq(r.body, "", "no body bytes on a proxied HEAD");
        assertEq(getHeader(r.headers, "content-length"), "5", "upstream's declared CL relayed");
      }],
      ["a URL fragment is never sent on the wire (d.ts get(url): the request target is the path)", null, async () => {
        const r = await client3.getAsync(base3 + "/json/7#frag-not-sent");
        assertEq(r.status, 200, "fragment did not break the request");
        assertEq(JSON.parse(r.body).id, "7", "the route matched the bare path");
      }],
      ["a bracketed IPv6 literal URL resolves and reaches a server bound on [::1] (d.ts get(url): string url)", null, async () => {
        let srv6 = null;
        try {
          srv6 = new HTTPServer({ port: 0, host: "[::1]", routes: { "/v6": "six" } });
          srv6.start();
          const r = await client3.getAsync("http://[::1]:" + srv6.port + "/v6");
          assertEq(r.status, 200, "bracketed-literal URL connected");
          assertEq(r.body, "six", "body served over the IPv6 loopback");
        } finally { if (srv6) srv6.close(); }
      }],
    ], async ([, , check]) => check());

    // keep-alive desync pin: a raw HEAD followed by a GET on the SAME
    // connection -- a stale body byte after the HEAD head would corrupt
    // the second response.
    {
      let stage = 0, headResp = "", nextResp = "";
      const cli = TCPServer.connect({ host: "127.0.0.1", port: app.port, connectTimeoutMs: 1000 }, {
        connect(conn) { conn.write("HEAD /json/7 HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"); },
        data(conn, bytes) {
          const s = new TextDecoder().decode(bytes);
          if (stage === 0) {
            headResp += s;
            if (headResp.includes("\r\n\r\n")) {
              stage = 1;
              conn.write("GET /json/7 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
            }
          } else {
            nextResp += s;
          }
        },
      });
      try {
        const t0 = Date.now();
        while (stage === 0 && Date.now() - t0 < 2000) await sleep2(5);
        assert(stage === 1, "HEAD head arrived within the deadline");
        assert(headResp.startsWith("HTTP/1.1 405"), "HEAD answered 405, got |" + headResp.split("\r\n")[0] + "|");
        assertEq(headResp.indexOf("\r\n\r\n") + 4, headResp.length, "zero body bytes after the HEAD head");
        const t1 = Date.now();
        while (!nextResp.includes("\r\n\r\n") && Date.now() - t1 < 2000) await sleep2(5);
        assert(nextResp.startsWith("HTTP/1.1 200"), "follow-up GET on the reused connection parses cleanly, got |" + nextResp.split("\r\n")[0] + "|");
        assert(nextResp.includes('{"id":"7"}'), "GET body intact (no stale bytes in the stream)");
      } finally {
        cli.dispose();
      }
    }

    // C4 on the static route servers: a route declared {status: 204}
    // serves headers only.
    {
      const srv = new HTTPServer({ port: 0, host: "127.0.0.1", routes: { "/nocontent": { status: 204, contentType: "text/plain", body: "x" } } });
      try {
        srv.start();
        const fr = await fetch("http://127.0.0.1:" + srv.port + "/nocontent");
        assertEq(fr.status, 204, "static route status");
        assertEq(await fr.text(), "", "static 204 carries no body");
        assert(fr.headers.get("content-length") === null, "static 204 has no Content-Length");
        assert(fr.headers.get("content-type") === null, "static 204 has no Content-Type");
      } finally { srv.close(); }
    }
  } finally {
    client3.close();
    app.close();
    up.dispose();
  }
}

print("bb_http: all tests passed (" + n + " assertions)");
