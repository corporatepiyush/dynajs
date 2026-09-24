# DynaJS API Reference

This document is the reference for every module in the DynaJS standard library. For each module it explains what the module is for, how its classes and functions behave — arguments, defaults, bounds, and the exact reasons a call refuses input — and what each operation returns. It is written to be read from top to bottom within a section, not just queried.

Two ground rules apply throughout.

First, every code example in this document is executed by the project's documentation gate against the current binary (`make check-api`). An example that stops running fails the build, so the examples and the behaviour they show cannot drift apart.

Second, the reference is generated against the binary's own export enumeration (`tools/api-inventory.js`). If a name is described here, it exists in the build; conversely, every exported name is described somewhere in its module's section.

In the signatures, arguments in square brackets are optional, and `->` states the return shape. The word "refused" always means the call throws an exception that names the reason — this library prefers a loud refusal over a silent wrong answer.

## The `--std` compatibility layer (std/os)

With `--std` the engine also exposes QuickJS's classic `std` and `os` modules. They are a compatibility layer, never the canonical door, and they follow different conventions (errno tuples instead of throws; `os.platform` is a string *property*, `sys.platform()` a function):

| Job | Canonical door | std/os equivalent |
|---|---|---|
| Filesystem | `dyna:file` | `os.stat`/`readdir`/`rename`/`mkdir`/`symlink`/`realpath` (errno tuples), `std.open` for handles |
| Environment | `sys.env`/`getEnv`/`setEnv` | `std.getenv`/`setenv`/`getenviron` |
| Spawn | `sys.Exec` (result object, throws) | `os.exec` + `os.waitpid` (pid/negative-errno conventions) |
| Timers | global `setTimeout`/`setInterval` | `os.setTimeout`/`setInterval` (same names, `--std`-gated) |
| Clocks | `dyna:time` now-family | `os.now` (ms, arbitrary origin), `os.sleep` (blocking ms) |
| Platform | `sys.platform()` | `os.platform` (property) |

New code should import the canonical module; `std`/`os` exist so QuickJS-shaped scripts run unmodified.

**`Worker.postMessage(value, transfer?)`**

The optional `transfer` list (array of `ArrayBuffer`/`ArrayBufferView`) detaches (neuters) each listed buffer on the sender — `byteLength` drops to 0 — after the message is queued. The receiver still gets its own copy (copy+detach, not a zero-copy move: the payload is serialized first). A `DataView` entry transfers its underlying buffer; a `SharedArrayBuffer` entry is refused with a `TypeError` (shared memory cannot be detached, so a "transfer" would silently keep the sender's alias). Any failure — a non-array list, an entry that is not a buffer, more than 1024 entries, a value that cannot be cloned, a queue error — throws and detaches nothing. The transfer list is an ownership hand-off, not a throughput knob: measured against the copy path it adds no material per-message cost and saves none either (the serialization copy dominates both).

<!-- check:skip -->
```js
const { Worker } = await import("os");   // needs --std; the example runner has no os module
const w = new Worker("worker.js");
const ab = new ArrayBuffer(1 << 20);
w.postMessage({ ab }, [ab]);   // sender's ab is neutered: ab.byteLength === 0
```

# dyna:http

HTTP client and server, WebSockets, and the WHATWG Fetch surface: `fetch`, `Request`, `Response`, `Headers`, `FormData`, `AbortController`/`AbortSignal` globals re-exported from the module, the blocking `HTTPClient`, the static-route `HTTPServer`, the single-reactor `HTTPServerAsync`, the JS-handler `App`, the `WsClient` WebSocket client, and a set of RFC header codecs (`ContentTypeParse`, `CookieParse`, `RangeParse`, `Negotiate`, `ETagMatch`, `Multipart*`).

Servers — which one when: `App` is the routed application server (rpc/static/proxy/upload/ws/sse plus dynamic routes and middleware on one event loop); `HTTPServer` is the static-route table server on its own reactor thread (safe to drive with the blocking `HTTPClient` from the script thread); `HTTPServerAsync` is the same shape for many mostly-idle connections. Start with `App`; drop to the others for their specific shapes.

Every options bag in this module is strict: an unknown option key throws a `TypeError` naming the key and the valid set (`unknown option "workerz" (valid: port, workers, backlog, requestTimeoutMs, host, routes)`), instead of being silently ignored. This covers the server/App constructor bags, route value specs (`{status, contentType, body}`), `static`/`upload`/`proxy`, the `ws`/`sse`/`WsClient` handler bags, `CookieSerialize` opts, and the fetch/Request/Response init bags — `fetch` validates synchronously, so a misspelled key throws before any network activity.

`import { fetch, Request, Response, Headers, FormData, AbortController, AbortSignal, HTTPClient, HTTPServer, HTTPServerAsync, App, WsClient, ContentTypeParse, ContentTypeFormat, CookieParse, CookieSerialize, ETagMatch, Negotiate, NegotiateToken, RangeParse, MultipartParse, MultipartFormat } from "dyna:http";`

### fetch

**`fetch(input, init?) -> Promise<Response>`**

- `input` *(string | Request)* — a URL string or a `Request`.
- `init` *(object, optional)* — request options:
  - `method` — upper-cased.
  - `headers` — an object, a `Headers`, or an array of pairs.
  - `body` — a string, `Uint8Array`, `ArrayBuffer`, or `FormData` (multipart-encoded with its content type set).
  - `signal` — an `AbortSignal`; a pre-aborted signal rejects immediately and an abort mid-flight rejects with `signal.reason`.
  - `timeout` — ms, passed to `HTTPClient.setTimeout`.

Returns a WHATWG-style fetch backed by `HTTPClient`. The response body is held fully in memory, capped by the client's 16 MiB default max body. The exchange runs through the `dyna:net` client; the fetch globals and the rest of `dyna:http` can be loaded in the same process, and `fetch` is also re-exported from `dyna:http`.

### Request

**`new Request(input, init?)`**

- `input` *(string | Request)* — a URL string or an existing `Request`; an existing request's `method`, `headers`, `body`, and `signal` carry over unless `init` overrides them.
- `init` *(object, optional)* — request options:
  - `method` — default `"GET"`, upper-cased.
  - `headers` — request headers.
  - `body` — the request body.
  - `signal` — an `AbortSignal`.

Constructs a request object. The constructor validates nothing about the URL; that happens when the request is used. Accessors: `method`, `url`, `headers` (a `Headers`), `signal`.

**`request.text() -> Promise<string>`**

Returns the body decoded as UTF-8; an empty body yields `""`.

**`request.json() -> Promise<any>`**

Returns the body parsed as JSON; rejects on a syntax error.

**`request.bytes() -> Promise<Uint8Array>`**

Returns the body as bytes.

**`request.arrayBuffer() -> Promise<ArrayBuffer>`**

Returns the body as an `ArrayBuffer`.

```js
import { Request } from "dyna:http";
const r = new Request("http://example.test/api", { method: "post", body: "x=1&y=2", headers: { "x-token": "t" } });
console.log(r.method, r.url, r.headers.get("x-token"));   // POST http://example.test/api t
const copy = new Request(r, { method: "GET" });
r.text().then(t => console.log("text:", t));               // text: x=1&y=2
r.json().catch(e => console.log("json rejects:", e.message));
copy.bytes().then(b => console.log("bytes:", b.length));   // bytes: 7
```

### Response

**`new Response(body?, init?)`**

- `body` *(null | string | Uint8Array | ArrayBuffer, optional)* — the response body.
- `init` *(object, optional)* — response options:
  - `status` — default 200.
  - `statusText` — default `"OK"`.
  - `headers` — default empty.
  - `url` — default `""`.

Constructs a response object. Accessors: `status`, `statusText`, `ok` (status in 200–299), `headers`, `url`, `bodyUsed`. The body is single-use: the first consumer takes it and every later call throws `TypeError: Body has already been consumed` (or `Cannot clone a consumed response` for `clone`).

**`response.text() -> Promise<string>`**

Returns the body as text.

**`response.json() -> Promise<any>`**

Returns the body parsed as JSON; rejects on a parse error.

**`response.bytes() -> Promise<Uint8Array>`**

Returns the body as bytes.

**`response.arrayBuffer() -> Promise<ArrayBuffer>`**

Returns the body as an `ArrayBuffer`.

**`response.clone() -> Response`**

Returns a second view of the same body; throws if the body was already consumed.

```js
import { Response } from "dyna:http";
const res = new Response('{"ok":1}', { status: 201, statusText: "Created", headers: { "content-type": "application/json" }, url: "http://e.test/" });
console.log(res.status, res.ok, res.statusText, res.url);   // 201 true Created http://e.test/
res.json().then(v => console.log("parsed:", v.ok));          // parsed: 1
const t = new Response("hello", { headers: { "content-type": "text/plain" } });
t.text().then(s => console.log("text:", s));                 // text: hello
const b = new Response(new Uint8Array([1, 2, 3]));
b.bytes().then(u => console.log("bytes:", u.length, u[1]));  // bytes: 3 2
const c = new Response("x");
const d = c.clone();
c.text().then(() => {});
d.text().then(s => console.log("clone:", s));                // clone: x
```

### Headers

**`new Headers(init?)`**

- `init` *(Headers | [name, value][] | object, optional)* — initial entries: another `Headers`, an array of `[name, value]` pairs, or a plain object.

Constructs a case-insensitive multi-value header map; names are stored lower-cased and `append` joins values with `", "`.

**`headers.get(name) -> string|null`**

- `name` *(string)* — the header name.

Returns the value, or `null` when absent.

**`headers.set(name, value)`**

- `name` *(string)* — the header name.
- `value` *(string)* — the replacement value, trimmed.

Replaces the value.

**`headers.append(name, value)`**

- `name` *(string)* — the header name.
- `value` *(string)* — the value to append, trimmed.

Appends a value, joining it with existing values by `", "`.

**`headers.has(name) -> boolean`**

- `name` *(string)* — the header name.

Returns membership.

**`headers.delete(name)`**

- `name` *(string)* — the header name.

Removes the header.

**`headers.forEach(cb, thisArg?)`**

- `cb` *(function)* — called as `cb(value, name, headers)`.
- `thisArg` *(any, optional)* — the `this` for `cb`.

Calls `cb` for each entry.

**`headers.keys()`**

Returns an iterator over names.

**`headers.values()`**

Returns an iterator over values.

**`headers.entries()`**

Returns an iterator over `[name, value]` pairs.

**`headers[Symbol.iterator]()`**

Returns an iterator over `[name, value]` pairs.

```js
import { Headers } from "dyna:http";
const h = new Headers([["x-a", "1"], ["x-a", "2"]]);
console.log(h.get("x-a"), h.has("x-a"), h.get("none"));       // 1, 2 true null
h.set("x-b", " hi ");
h.append("x-b", "lo");
console.log(JSON.stringify(h.get("x-b")));                    // "hi, lo"
h.delete("x-a");
console.log([...h.entries()].map(p => p.join("=")).join(";")); // x-b=hi, lo
const flat = new Headers({ a: "1" });
flat.set("A", "2");
console.log(flat.get("a"));                                   // 2
```

### FormData

**`new FormData()`**

Constructs an ordered, multi-value field list for multipart bodies. Values may be strings or bytes (`Uint8Array`/`ArrayBuffer`, with an optional `filename`).

**`form.append(name, value, filename?)`**

- `name` *(string)* — the field name.
- `value` *(string | Uint8Array | ArrayBuffer)* — the field value.
- `filename` *(string, optional)* — a filename for byte values.

Appends a value.

**`form.set(name, value, filename?)`**

- `name` *(string)* — the field name.
- `value` *(string | Uint8Array | ArrayBuffer)* — the field value.
- `filename` *(string, optional)* — a filename for byte values.

Replaces all values for a name.

**`form.get(name) -> value|null`**

- `name` *(string)* — the field name.

Returns the value, or `null` when absent.

**`form.getAll(name) -> value[]`**

- `name` *(string)* — the field name.

Returns every value for the name.

**`form.has(name) -> boolean`**

- `name` *(string)* — the field name.

Returns membership.

**`form.delete(name)`**

- `name` *(string)* — the field name.

Removes the name's values.

**`form.forEach(cb, thisArg?)`**

- `cb` *(function)* — called as `cb(value, name, form)`.
- `thisArg` *(any, optional)* — the `this` for `cb`.

Calls `cb` for each entry.

**`form.keys()`**

Returns an iterator over names.

**`form.values()`**

Returns an iterator over values.

**`form.entries()`**

Returns an iterator over `[name, value]` pairs.

**`form[Symbol.iterator]()`**

Returns an iterator over `[name, value]` pairs.

```js
import { FormData } from "dyna:http";
const fd = new FormData();
fd.append("a", "1");
fd.append("a", "2");
fd.append("f", new Uint8Array([1, 2]), "blob.bin");
console.log(fd.get("a"), fd.getAll("a").join(","), fd.has("f"));   // 1 1,2 true
fd.set("a", "9");
console.log(fd.getAll("a").join(","));                              // 9
console.log([...fd.keys()].join(","));                              // a,f
```

### AbortController / AbortSignal

**`new AbortController()`**

Constructs an object pairing a signal with a way to fire it. `controller.signal` is an `AbortSignal`; `controller.abort(reason?)` transitions it.

**`controller.abort(reason?)`**

- `reason` *(any, optional)* — the abort reason.

Transitions the signal to aborted.

**`signal.aborted`**

Read-only boolean; `true` once the signal has aborted.

**`signal.reason`**

The abort reason, defaulting to an `Error`.

**`signal.onabort`**

The `abort` event handler property; assignable in addition to `addEventListener("abort", fn)`.

**`signal.throwIfAborted()`**

Throws `signal.reason` when aborted, otherwise returns.

**`AbortSignal.abort(reason?)`**

- `reason` *(any, optional)* — the abort reason.

Returns a pre-aborted signal.

**`AbortSignal.timeout(ms)`**

- `ms` *(number)* — the timeout in milliseconds.

Returns a signal that aborts with `Error: The operation timed out` after `ms`; the timer is cleared on abort.

```js
import { AbortController, AbortSignal } from "dyna:http";
const ac = new AbortController();
console.log(ac.signal.aborted, ac.signal.reason);          // false undefined
ac.signal.addEventListener("abort", () => console.log("fired"));
ac.abort(new Error("stop"));
console.log(ac.signal.aborted, ac.signal.reason.message);  // true stop
const t = AbortSignal.timeout(30);
setTimeout(() => console.log("timeout aborted:", t.aborted), 60);
const pre = AbortSignal.abort("nope");
console.log(pre.aborted);                                   // true
```

### HTTPClient

**`new HTTPClient(maxBodyBytes?)`**

- `maxBodyBytes` *(number, optional)* — caps the response body the client accepts (default 16 MiB); a larger body fails with `response too large`.

Constructs a blocking HTTP/1.1 client, one fresh connection per request, with a default request timeout of 15000 ms. It opens no sockets until a request is made, so construction and the URL refusals below are safe to exercise anywhere. Every request URL must be `http://` or `https://` (TLS on this build) with a printable-ASCII host; anything else (no scheme, a control byte, a CRLF injection, a path past 2048 bytes) throws `bad URL` before any connection is attempted.

Which HTTP client when: the global `fetch` for one-shot async calls (WHATWG shape; `RequestInit.timeout` is honored); `HTTPClient` for blocking `get`/`post`/`request` with explicit `setTimeout`/`disconnect` control and a max-body cap; scrape's `Fetcher` when robots policy, retries, backoff and caching matter.

**`client.onConnect`** — an optional property: set it to a function and it is called with the RESOLVED IP (a string) between TCP connect and the first request byte. Return `false` — or throw — to refuse the connection for that request; a non-function value is ignored. (`client.onConnect = (ip) => allowlist.has(ip);`)

**`client.get(url, headers?) -> { status, statusText, ok, headers, body }`**

- `url` *(string)* — the request URL; must be `http://` or `https://` with a printable-ASCII host.
- `headers` *(object, optional)* — extra request headers.

Returns the response object. A malformed URL throws `bad URL` before any connection is attempted. Framing, for `get`/`post`/`request` and their `Async` forms alike: `Content-Length` — a connection ending before the declared length is a **truncated body**: the exchange fails naming the bytes served and declared, never a short-but-complete answer; chunked with incremental dechunking — a body with no terminal chunk is the same refusal; read-to-EOF only where the server declared no length at all (`Connection: close`). The framing rule is not advisory: a cut exchange must never read as a complete 200. The response `body` is always a text string — the raw bytes are decoded as UTF-8 with invalid sequences replaced — so binary content comes back mangled in `body`; the **exact octets** the server sent are in `bodyBytes` (an `ArrayBuffer`, present on every response), which is how binary content reaches the caller unmangled:
`new Uint8Array(r.bodyBytes)`.

**`client.post(url, body, headers?) -> response`**

- `url` *(string)* — the request URL.
- `body` *(any)* — the request body.
- `headers` *(object, optional)* — extra request headers.

Returns the response object. A blocking POST.

**`client.request(method, url, body?, headers?) -> response`**

- `method` *(string)* — the HTTP method.
- `url` *(string)* — the request URL.
- `body` *(any, optional)* — the request body.
- `headers` *(object, optional)* — extra request headers.

Returns the response object. Sends any method.

**`client.getAsync(url, headers?) -> Promise<response>`**

- `url` *(string)* — the request URL.
- `headers` *(object, optional)* — extra request headers.

Returns a promise of the response object (`{ status, statusText, ok, headers, body }` with `headers` keyed by the exact header names the server sent). The same exchange runs on the io pool; a malformed URL still throws synchronously, and network and protocol failures reject.

**`client.postAsync(url, body, headers?) -> Promise<response>`**

- `url` *(string)* — the request URL.
- `body` *(any)* — the request body.
- `headers` *(object, optional)* — extra request headers.

Returns a promise of the response object. The same exchange runs on the io pool; a malformed URL still throws synchronously, and network and protocol failures reject.

**`client.requestAsync(method, url, body?, headers?) -> Promise<response>`**

- `method` *(string)* — the HTTP method.
- `url` *(string)* — the request URL.
- `body` *(any, optional)* — the request body.
- `headers` *(object, optional)* — extra request headers.

Returns a promise of the response object. The same exchange runs on the io pool; a malformed URL still throws synchronously, and network and protocol failures reject.

**`client.setTimeout(ms)`**

- `ms` *(number)* — the timeout in milliseconds.

Sets the per-request timeout for later requests.

**`client.disconnect()`**

A no-op: each request uses its own connection and the client holds no socket.

**`client.close()`**

Releases the client; `client.closed` is true afterwards. `[Symbol.dispose]` works with `using`.

**`client.getStream(url[, headers]) -> StreamedResponse`**

ONE exchange (no redirect chasing, exactly like `get` — a 3xx is the response it is) with the body left on the socket. The returned object is the response head and the body stream in one: `{ status, statusText, ok, headers, url, contentType, read(buf), close(), dispose(), [Symbol.dispose], closed }`. `read(buf)` fills the caller's buffer with up to `buf.length` bytes and resolves with the count — `0` is the end of the body — which is exactly dyna:stream's ByteSource shape, so `stream.pipe(resp, sink)`, `stream.lines(resp)` and `stream.ndjson(resp)` consume the response directly; this module neither imports nor links dyna:stream.

Framing matches the buffered reader (`Content-Length`, chunked with incremental dechunking, read-to-EOF only where the server declared no length) and its truncation rule: a connection ending before a declared `Content-Length` fails the same way the buffered exchange fails — a named truncation, never a clean end. The client's `maxBody` is enforced ON the stream: a declared `Content-Length` over the cap refuses before the body starts; received bytes are counted as they are handed out and crossing the cap rejects the current read and closes the stream. A connection ending before a declared `Content-Length` is a truncated body — the read REJECTS naming bytes served and declared, never a silent 0; every later read rejects the same error. The client's `setTimeout` applies; `disconnect()` does not reach a half-read stream — aborting it is `close()` (or `using`), the caller owns it.

```js
import { HTTPClient } from "dyna:http";
const c = new HTTPClient();
let opened = false;
try { c.getStream("http://127.0.0.1:1/nothing"); }        // nothing listens there
catch (e) { opened = /connection failed/.test(e.message); }
console.log(opened);                                       // true
```

(A streaming example that READS needs a live origin, which `tests/test_http_stream.js` runs against a real server: `lines()` over a chunked body, byte-exact content-length streaming, and named truncation.)

**`client.dispose()`**

Releases the client; `client.closed` is true afterwards.

```js
import { HTTPClient } from "dyna:http";
const c = new HTTPClient(1024 * 1024);   // cap responses at 1 MiB
try { c.get("not a url"); } catch (e) { console.log(e.message); }          // HTTP GET not a url failed: bad URL
try { c.get("ftp://host/"); } catch (e) { console.log(e.message); }        // HTTP GET ftp://host/ failed: bad URL
try { c.get("http://x.test/\r\nInjected: y"); } catch (e) { console.log(e.message); }
c.setTimeout(5000);
console.log(typeof c.getAsync, c.closed);                                  // function false
c.close();
console.log(c.closed);                                                     // true
```

### HTTPServer

**`new HTTPServer({ port?, host?, workers?, backlog?, requestTimeoutMs?, routes? })`**

- `port` *(number, optional)* — default 0; binds an ephemeral port, resolved into `.port` after construction.
- `host` *(string, optional)* — default all interfaces.
- `workers` *(number, optional)* — default the process-wide `--io-threads` setting, clamped to 1–64. Accepted and echoed by `.workers` for configuration compatibility, but sizes nothing: the server is served by one reactor thread (io_uring on Linux, kqueue/epoll elsewhere). One process serves one core; the listening socket sets `SO_REUSEPORT`, but on macOS the kernel hands every incoming connection to a single member of a reuseport group (measured here: four processes on one port, all traffic to the first — reproduced with a 20-line C program, so it is the platform, not this server), so extra processes buy nothing there. Linux does distribute, which is where multi-process on one port helps.
- `backlog` *(number, optional)* — default the system `SOMAXCONN`.
- `requestTimeoutMs` *(number, optional)* — default 30000; the idle budget for a connection, stamped on completed requests. A connection that completes nothing within it is closed; 0 disables. A single request (head + body) is bounded by the same budget and by the 16 KiB whole-request cap below.
- `routes` *(object, optional)* — maps a path to a response; a route value is a string (served as `text/plain`) or `{ status, contentType, body }`.

Constructs an HTTP/1.1 server serving static routes on a single reactor thread; it keeps the class's original property that a blocking `HTTPClient` on the script's own thread can talk to it, because the server's I/O never runs on the script's thread. It binds in the constructor, so construction fails if the port is taken. A header block larger than 16 KiB is refused with `431`, a head+body past the same cap with `413`, and connections past the simultaneous ceiling (8192, the `HTTPServerAsync` default) are closed on accept.

**`server.start()`**

Starts the reactor thread; idempotent.

**`server.stop()`**

Stops accepting, closes live connections, and joins the reactor thread.

**`server.port`**

The bound port (useful when constructed with `port: 0`).

**`server.close()`**

Releases the listener and reactor; `server.closed` turns true.

**`server.dispose()`**

Releases the listener and workers; `server.closed` turns true.

```js
import { HTTPServer } from "dyna:http";
const s = new HTTPServer({ port: 0, routes: { "/": "hello", "/data": { status: 201, contentType: "application/json", body: '{"a":1}' } } });
console.log(typeof s.start, s.port > 0, s.closed);   // function true false
s.close();
console.log(s.closed);                               // true
```

### HTTPServerAsync

**`new HTTPServerAsync({ port?, host?, backlog?, idleTimeoutMs?, maxConns?, routes? })`**

- `port` *(number, optional)* — default 0 (ephemeral).
- `host` *(string, optional)* — default all interfaces.
- `backlog` *(number, optional)* — default `SOMAXCONN`.
- `idleTimeoutMs` *(number, optional)* — default 30000; 0 disables the idle sweep.
- `maxConns` *(number, optional)* — default 8192; 0 unbounded.
- `routes` *(object, optional)* — the same static-route model and shape as `HTTPServer`.

Constructs the single-reactor server: one background kqueue/epoll thread multiplexes all connections, so connection count is bounded by the fd limit rather than a worker cap. Hard bounds per connection: a single buffered request is capped at 1 MiB and a keep-alive connection at 100000 requests.

**`server.start()`**

Spawns the reactor thread and waits for it to initialize.

**`server.stop()`**

Stops accepting and drains the workers.

**`server.port`**

The bound port.

**`server.close()`**

Releases the listener; `server.closed` turns true.

**`server.dispose()`**

Releases the listener; `server.closed` turns true.

```js
import { HTTPServerAsync } from "dyna:http";
const s = new HTTPServerAsync({ port: 0, routes: { "/": "hi" }, idleTimeoutMs: 10000, maxConns: 32 });
console.log(s.port > 0, typeof s.start);   // true function
s.close();
console.log(s.closed);                     // true
```

### App

**`new App({ port?, host?, idleTimeoutMs?, maxConns?, compress?, metrics?, workers?, backlog? })`**

- `port` *(number, optional)* — default 0.
- `host` *(string, optional)* — default all interfaces; an IPv6 literal (`"::"`) binds dual-stack and also serves IPv4-mapped peers.
- `idleTimeoutMs` *(number, optional)* — default 30000; 0 disables the idle sweep.
- `maxConns` *(number, optional)* — default 8192; connections beyond the cap are accepted and closed immediately. 0 unbounded.
- `compress` *(boolean, optional)* — default true; gzip only for clients that send `Accept-Encoding`.
- `metrics` *(boolean, optional)* — default false.
- `workers` *(number, optional)* — integer 1–64, default 1. Validated and echoed by `app.workers` so a configuration shared with `HTTPServer` does not silently drop the key. App itself is the single-reactor server (model B): every handler runs on the one loop thread, so today `workers` sizes nothing — one process serves one core. The listening socket sets `SO_REUSEPORT`, which balances across processes on Linux; on macOS every connection lands on the first process in the group, so extra processes on one port do not scale (see `HTTPServer.workers`).
- `backlog` *(number, optional)* — integer 1–65535, default 1024. Forwarded to the `listen(2)` call at `start()`; `app.backlog` echoes the effective value.

Constructs the JS-handler server (model B): everything runs on the JS thread through the shared io reactor, so route handlers are plain JS functions with no cross-thread hop. It does not bind until `start()`.

**`app.rpc(path, methods)`**

- `path` *(string)* — the URL path.
- `methods` *(object)* — the RPC method table.

Registers a strict JSON-RPC 2.0 endpoint. Each method is called as `fn(params)` where `params` is the request's `params` value; returning a promise defers the response until it settles; throwing produces a JSON-RPC error object. Batches are supported. A `content-type` other than `application/json` is refused with 415.

**Response streaming:** a handler may return a dyna:stream-compatible ByteSource instead of a JSON value — any object with a callable `read(buf) -> Promise<number>` (0 = end of body), the shape `stream.pipe`/`lines`/`ndjson` accept — or `{ stream, contentType?, status? }`. That response opts OUT of the JSON-RPC envelope: the App answers with the handler's status (default 200) and content type (default `application/octet-stream`) under `Transfer-Encoding: chunked`, and pumps the source one `read()` per chunk — backpressure is the socket, and a source that stops is bounded by the peer walking away. A `0` ends the body with the terminal chunk (the source's `close()` runs there); anything thrown or rejected mid-stream is past the status line, so the connection truncates — the one behaviour an HTTP server has once headers have flushed. A read whose promise is dropped by the pump (an already-rejected read, or a read abandoned by a close during its call) truncates the same way, and its late rejection is swallowed deliberately — it can never surface as an unhandled rejection. HTTP/1.0 peers get a 500 (chunked framing does not exist below 1.1), a peer refusing identity encoding gets 406 (streamed bodies go identity, uncompressed), and a ByteSource inside a batch is an internal error (a batch is ONE response). Ordering: while a stream is pumping, the connection's request pump HOLDS exactly as it does for a pending handler promise — a request arriving behind a live stream (pipelined in one write, or written mid-stream) is buffered and dispatched only after the stream's terminal chunk (or its truncation), so responses leave in request order and a follow-up response is never injected inside a live chunked body. One stream per connection, and the pump slot is never overwritten: a second streamed response cannot displace a live one.

**`app.get(pattern, handler)` · `app.post` · `app.put` · `app.patch` · `app.del`**

- `pattern` *(string)* — a URL path: literal segments, `:param` segments, and one trailing `*rest`.
- `handler` *(function)* — called as `handler(req)`; `req` is the request context below.

Registers a method-specific dynamic route (`del` is DELETE — a method name cannot be a keyword). A computed JSON response stops being a string-built StaticRoute: the handler returns a value and the App shapes the reply.

The pattern grammar, enforced at registration (an invalid pattern throws a `TypeError` naming the problem): it must start with `/`, carry no query or fragment, and have no empty segments (`"/v/"` and `"/v"` match each other — a trailing slash is not significant). A `:param` segment matches exactly one non-empty path segment and captures it percent-decoded into `req.params` (`%2F` decodes inside the value; `+` stays literal — paths are not query strings). Names match `[A-Za-z0-9_]+`, are unique per pattern (at most 16 named captures), and a param name stays under 64 bytes. A `*rest` segment must be last (at most one per pattern) and captures the path remainder, slashes included and decoded — `"/files/*rest"` matches `/files/a/b` with `rest === "a/b"`, and the bare `/files` with `rest === ""`. A `:` or `*` that does not start its segment is refused.

Dispatch: matching is against the raw path (query stripped), patterns are tried in registration order and the first method+pattern match wins. Typed routes (rpc/static/proxy/upload/ws/sse) are checked first and never displaced. When a pattern matches the path but no registered method matches the request method the answer is 405 with an `Allow` header listing the methods registered for that path; when nothing matches at all the request falls through to the metrics endpoints and the 404. Method matching is exact and case-sensitive — HEAD needs its own registration.

Pipelining: responses leave in request order (RFC 9112). While a handler's promise is unsettled — and while a streamed response is still pumping — the connection's request pump holds: buffered pipelined requests are dispatched once the pending response settles (or once the stream reaches its terminal chunk), so a later request's `Connection: close` cannot close the socket before an earlier still-pending response is delivered, and an ordinary response can never land inside a live chunked body (the held bytes are bounded by the whole-request ceiling). A handler that abandons its response promise (never settles, and the promise becomes unreachable) closes the connection: the request can never be answered, and leaving the peer waiting would desync the pipeline.

`req` is `{ method, path, params, query, headers, body }`: `path` is the raw request path, `params` the captures above (an empty object for a paramless pattern), `query` the query string as an object (keys and values percent-decoded, `+` as space, a bare key present with `""`, a repeated key keeping the LAST occurrence), `headers` an object with lower-cased names — only the comma-joinable list fields and `cookie`/`set-cookie` can repeat (any other duplicate is refused with 400 before dispatch), and repeats join with `", "`, `Cookie` with `"; "`, `Set-Cookie` keeping the first copy (its values cannot be combined) — and `body` the raw request body as a string.

The handler's return value shapes the response: a string is 200 `text/plain`; a byte view (`Uint8Array`/`ArrayBuffer`) is 200 `application/octet-stream`; an object with an own numeric `status` or an own `body` is the envelope `{ status?, body?, contentType? }` — a string body defaults to `text/plain` unless `contentType` says otherwise, a byte body to `application/octet-stream`, any other body JSON-encodes; anything else JSON-encodes at 200. Envelope detection reads OWN properties only — `status`, `body` and `contentType` found on the prototype chain are ignored, so a plain `{}` return answers JSON `{}` however polluted `Object.prototype` is. A `null` return is the JSON `null`; an `undefined` return is a 500 (a handler must answer). A thenable return settles before the response is sent — `async` handlers work as written — and a thenable that settles more than once (or settles and then throws) answers exactly once, from its first settle. A throw or rejection is a 500 whose JSON error message is escaped like data, never spliced into body bytes.

**`app.use(fn)`**

- `fn` *(function)* — called as `fn(req)` with the same request context the handler receives.

Appends middleware to the chain that runs before the matched dynamic handler, in registration order. Auth, logging and CORS stop being copy-paste per handler: the chain sees one request context, and because the handler receives the SAME object, a middleware annotates it (`req.user = …`) to pass state along.

`fn` returning an object with an OWN `response` property short-circuits (a `response` on the prototype chain is ignored — own-property reads only, like envelope detection): that value is sent exactly like a handler's return (a thenable response is awaited first), and the rest of the chain — later middleware and the handler — does not run. Any other return continues the chain. Middleware is synchronous: a thenable return is a 500 (async work belongs in the handler, which may be async). A throw ends the chain with a 500 carrying the escaped message; rewrites already applied by earlier middleware stand.

Scope, stated precisely: middleware gates matched DYNAMIC routes only. Typed routes (rpc/static/proxy/upload/ws/sse) and unmatched paths (including the 404) are not passed through it — the chain governs handlers, not the server plumbing.

**`app.static(prefix, rootPath, { maxFileSize?, allow? })`**

- `prefix` *(string)* — the URL prefix.
- `rootPath` *(Path)* — a `dyna:path` `Path`, resolved once at registration.
- `maxFileSize` *(number, optional)* — the file size cap.
- `allow` *(string[], optional)* — an array of `.ext` or mime strings; an empty list refuses everything.

Serves files from a directory tree under a URL prefix.

**`app.upload(path, { dir, maxFileSize?, allow? }, handler(savedPath, meta))`**

- `path` *(string)* — the URL path.
- `dir` *(Path)* — the directory uploads are saved into.
- `maxFileSize` *(number, optional)* — the file size cap.
- `allow` *(string[], optional)* — an array of `.ext` or mime strings.
- `handler` *(function)* — called as `handler(savedPath, meta)`.

Accepts multipart file uploads into `dir`, then calls the handler with the saved path and metadata. The body is streamed to disk through the aio disk pool (up to 4 writes of 256 KiB in flight per upload), so a slow disk does not block the reactor; the request buffer is bounded by `maxFileSize`, not by whole-body memory.

**`app.proxy(prefix, { host?, port })`**

- `prefix` *(string)* — the URL prefix.
- `host` *(string, optional)* — default `127.0.0.1`.
- `port` *(number)* — must be 1–65535.

Forwards matching requests upstream.

**`app.ws(path, { open?, message?, close? })`**

- `path` *(string)* — the URL path.
- `open` *(function, optional)* — called as `open(conn)`.
- `message` *(function, optional)* — called as `message(conn, data, binary)`.
- `close` *(function, optional)* — called as `close(conn, code, reason)`.

Registers a WebSocket endpoint. `conn.send(data)` (string or bytes) sends a frame; `conn.close()` closes the connection.

**`app.sse(path, { open?, close? })`**

- `path` *(string)* — the URL path.
- `open` *(function, optional)* — called as `open(conn)`.
- `close` *(function, optional)* — called as `close(conn)`.

Registers a server-sent events endpoint. `open(conn)` keeps the connection open until either side closes; `conn.send(data)` writes an SSE event; `conn.close()` ends it.

**`app.start()`**

Binds, resolves `.port` for port 0, and arms the idle sweep; fails loudly if the reactor backend cannot arm a clock.

**`app.port`**

The bound port.

**`app.workers`**

The ctor's `workers` value (default 1). Validated and reported; see the ctor note for what it does not (yet) do.

**`app.backlog`**

The effective listen backlog: the ctor's `backlog`, or 1024.

**`app.close()`**

Releases the listener; `app.closed` turns true.

**`app.dispose()`**

Releases the listener; `app.closed` turns true.

<!-- check:skip -->
```js
import { App, HTTPClient } from "dyna:http";
const app = new App({ port: 0 });
app.rpc("/api", {
  add: (params) => params[0] + params[1],
  async scale(params) { return params[0] * 2; }
});
app.start();
(async () => {
  const c = new HTTPClient();
  const r = await c.postAsync(`http://127.0.0.1:${app.port}/api`,
    JSON.stringify({ jsonrpc: "2.0", id: 1, method: "add", params: [2, 3] }),
    { "Content-Type": "application/json" });
  console.log(r.body);               // {"jsonrpc":"2.0","result":5,"id":1}
  app.close();
})();
```

<!-- check:skip (opens live sockets; the loop never drains) -->
```js
import { App, HTTPClient } from "dyna:http";

const app = new App({ port: 0 });
app.use((req) => {                     // HP-2: one auth gate, every handler
  if (!req.headers["x-token"])
    return { response: { status: 401, body: "denied" } };
});
app.get("/users/:id", (req) => ({ id: req.params.id, q: req.query }));
app.get("/files/*rest", (req) => ({ file: req.params.rest }));
app.post("/echo", async (req) => ({ status: 201, body: req.body }));
app.start();
(async () => {
  const c = new HTTPClient();
  const hit = await c.getAsync(`http://127.0.0.1:${app.port}/users/7?full=1`,
                               { "X-Token": "t" });
  console.log(hit.body);               // {"id":"7","q":{"full":"1"}}
  const denied = await c.getAsync(`http://127.0.0.1:${app.port}/users/7`);
  console.log(denied.status);          // 401 -- the chain stopped before the handler
  app.close();
})();
```

### WsClient

**`new WsClient("ws://host:port/path", { open?, message?, close? })`**

- `url` *(string)* — the WebSocket URL; must use the `ws://` scheme.
- `open` *(function, optional)* — called as `open(conn)` when the 101 is verified; frames that arrived with it are delivered after.
- `message` *(function, optional)* — called as `message(conn, data, binary)` for each frame; `data` is a string for text, a `Uint8Array` for binary.
- `close` *(function, optional)* — called as `close(code, reason)` on teardown.

Constructs an RFC 6455 client; the DNS lookup, connect, and upgrade handshake run off the event loop on the io pool. Network and handshake failures surface as `close(1006, reason)`; there is no error event. Argument errors throw from the constructor: a `wss://` URL throws (client TLS on the reactor does not exist in this build), and the handlers object is required. All frames it sends are masked, as the spec requires; frames it receives are reassembled with a 1 MiB budget, control frames are budgeted at 64 between messages, and an oversized or unmasked inbound frame closes the connection.

**`conn.send(data)`**

- `data` *(string | Uint8Array | ArrayBuffer)* — the frame payload.

Sends a text frame (string) or binary frame (bytes). A closed connection silently drops the frame.

**`conn.close()`**

Performs polite teardown: a close frame first, then the connection. `conn.closed` reports state.

**`conn.dispose()`**

Performs polite teardown: a close frame first, then the connection. `conn.closed` reports state.

```js
import { WsClient } from "dyna:http";
try { new WsClient("wss://example.test/", {}); } catch (e) { console.log(e.message); }
try { new WsClient("ftp://example.test/", {}); } catch (e) { console.log(e.message); }
try { new WsClient("ws://example.test/"); } catch (e) { console.log(e.message); }
// a live server: open fires after the handshake, close(1006, reason) on failure
```

### Message codecs

**`ContentTypeParse(header) -> { type, subtype, parameters } | null`**

- `header` *(string)* — an HTTP media type header.

Parses an HTTP media type; `type` and `subtype` are lower-cased, parameter names are lower-cased, quoted values are unquoted. Returns `null` when the header is not `type/subtype` (an empty or bare header too).

**`ContentTypeFormat({ type, subtype, parameters? }) -> string`**

- `type` *(string)* — required.
- `subtype` *(string)* — required.
- `parameters` *(object, optional)* — media type parameters.

Produces the header string; values needing quoting are quoted and escaped.

**`CookieParse(header) -> { name: value, ... }`**

- `header` *(string)* — a `Cookie` header.

Parses a `Cookie` header; duplicate names keep the last value and a quoted value drops its quotes.

**`CookieSerialize(name, value, opts?) -> string`**

- `name` *(string)* — must be an RFC token.
- `value` *(string)* — must not carry a delimiter (`;`, `,`, `"`, `\`, control bytes).
- `maxAge` *(number, optional)* — seconds.
- `domain` *(string, optional)* — the cookie domain.
- `path` *(string, optional)* — the cookie path.
- `sameSite` *(string, optional)* — the SameSite attribute.
- `secure` *(boolean, optional)* — the Secure flag.
- `httpOnly` *(boolean, optional)* — the HttpOnly flag.

Produces a `Set-Cookie` value. A name that is not an RFC token, or a value carrying a delimiter, is refused with a `TypeError` rather than escaped.

**`ETagMatch(ifNoneMatch, etag) -> boolean`**

- `ifNoneMatch` *(string)* — the `If-None-Match` header value.
- `etag` *(string)* — the resource's etag.

Performs weak comparison (a `W/` prefix is ignored) per `If-None-Match`; `*` matches anything.

**`Negotiate(header, candidates) -> string|null`**

- `header` *(string)* — a media-type `Accept` header.
- `candidates` *(string[])* — the available media types.

Picks the best candidate by specificity then q-value; `q=0` is an explicit refusal, an empty header accepts the first candidate, no match returns `null`.

**`NegotiateToken(header, candidates) -> string|null`**

- `header` *(string)* — an `Accept`-style header of plain tokens.
- `candidates` *(string[])* — the available tokens.

Picks the best candidate by specificity then q-value; a `-` prefix match also counts (`en` satisfies `en-US`).

**`RangeParse(header, size) -> [{start,end},...] | "unsatisfiable" | null`**

- `header` *(string)* — a `Range` header.
- `size` *(number)* — the resource size in bytes.

Parses a `Range` against a resource of `size` bytes. Ranges are inclusive; a suffix range (`-500`) counts from the end; an open end clamps to `size - 1`; a range past the resource returns `"unsatisfiable"`; a malformed header returns `null`.

```js
import { ContentTypeParse, ContentTypeFormat, CookieParse, CookieSerialize, ETagMatch, Negotiate, NegotiateToken, RangeParse } from "dyna:http";
const ct = ContentTypeParse("text/html; charset=utf-8");
console.log(ct.type, ct.subtype, ct.parameters.charset);              // text html utf-8
console.log(ContentTypeFormat({ type: "text", subtype: "html", parameters: { charset: "utf-8" } }));
const ck = CookieParse('a=1; b="two words"');
console.log(ck.a, ck.b);                                              // 1 two words
console.log(CookieSerialize("sid", "abc", { maxAge: 3600, path: "/", httpOnly: true }));
console.log(ETagMatch('W/"v1"', '"v1"'), ETagMatch('*', '"x"'));      // true true
console.log(Negotiate("text/html;q=1.0, text/plain;q=0.5", ["text/plain", "text/html"]));   // text/html
console.log(NegotiateToken("gzip, deflate;q=0.8", ["deflate", "gzip"]));                    // gzip
console.log(JSON.stringify(RangeParse("bytes=0-4", 100)));            // [{"start":0,"end":4}]
console.log(JSON.stringify(RangeParse("bytes=-5", 100)));             // [{"start":95,"end":99}]
console.log(JSON.stringify(RangeParse("bytes=500-", 100)));           // "unsatisfiable"
```

### Multipart

**`MultipartFormat(parts) -> { contentType, body }`**

- `parts` *(array)* — each part is `{ name, value }` (string) or `{ name, body, filename? }` (a `Uint8Array`/`ArrayBuffer`).

Builds a `multipart/form-data` body. The returned `contentType` carries the generated boundary; `body` is a `Uint8Array`.

**`MultipartParse(contentType, body) -> [{ name, filename?, body }]`**

- `contentType` *(string)* — the header or an object's `boundary`.
- `body` *(bytes)* — the multipart body.

Parses a multipart body back into parts. Part payloads come back as `Uint8Array`s, one per field in order.

```js
import { MultipartFormat, MultipartParse } from "dyna:http";
const fmt = MultipartFormat([
  { name: "a", value: "1" },
  { name: "f", body: new Uint8Array([1, 2, 3]), filename: "b.bin" }
]);
console.log(fmt.contentType.startsWith("multipart/form-data; boundary="));   // true
const parts = MultipartParse(fmt.contentType, fmt.body);
console.log(parts[0].name, parts[1].filename, parts[1].body.length);          // a b.bin 3
```

### connectHappy

**`connectHappy(host, port, opts?, handlers?) -> TCPSocket`**

- `host` *(string)* — the host to connect to.
- `port` *(number)* — must be 1–65535.
- `fallbackMs` *(number, optional)* — the deadline for the whole race; default 250, must be at least 1.
- `handlers` *(object, optional)* — the connect handlers.

Exported from `dyna:net`. RFC 6555 Happy Eyeballs: both address families are resolved and connect at once, the first success wins and the loser is closed. DNS failure and `no usable addresses` throw synchronously; the returned socket is the same resource the TCP connect API returns, and the first winner's `connect` handler fires when the race lands. Requires the `dyna:net` module and a reachable host, so it is shown without a runnable example.

### swallowedHandlerThrows

**`swallowedHandlerThrows() -> number`**

Exported from `dyna:net`. Counts every exception thrown out of a net event
handler (TCP `connect`/`data`/`close`, UDP `message`) since process start. The
doctrine is unchanged -- a net event handler must not tear down the event loop,
so its throws are swallowed -- and the swallow is observable: read this
counter from JS, or run `dynajs -d`, which prints it after the memory dump and
also emits a one-line stderr diagnostic the FIRST time each handler kind throws
(further throws of that kind are counted silently). Apart from the counter,
runs without `-d` behave identically.

<!-- check:skip (opens live sockets; the loop never drains) -->
```js
import { swallowedHandlerThrows, UDPSocket } from "dyna:net";
const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
s.start({ message() { throw new Error("handler bug"); } });
// ... datagrams arrive; the loop keeps running ...
console.log(swallowedHandlerThrows());   // e.g. 3 -- three swallowed handler throws
```

### remaining exports

- `AbortSignal.prototype._abort(reason)` — the internal transition function `abort()` calls; not part of the public surface.
- `AbortSignal.prototype.onabort` — the `abort` event handler property; assignable in addition to `addEventListener`.
- `Request.prototype.signal` — the signal a fetch would race against; stored but unused outside `fetch`.
- `Response.prototype.url` — the value passed as `init.url` (set by `fetch` to the request URL).
- Every resource class (`HTTPClient`, `HTTPServer`, `HTTPServerAsync`, `App`, `WsClient`) additionally gains `close()`, `dispose()`, `closed`, and `[Symbol.dispose]` from the shared resource wrapper.

# dyna:html

An HTML5 tokenizer, a lenient tree, compiled CSS selectors, a sanitizer, markdown rendering, an in-place link rewriter, and a Mustache-like template — everything escapes through one escaper. The tree shape is shared with `dyna:xml`: an element is `{ name, attrs, children }` with text as plain strings inside `children`.

`import { HTMLParse, HTMLStringify, HTMLText, MarkdownToHTML, Selector, Sanitizer, Template, rewriteLinks } from "dyna:html";`

### HTMLParse

**`HTMLParse(html) -> node[]`**

- `html` *(string)* — the HTML source.

Tokenizes and tree-builds HTML, returning the top-level node list. Tag and attribute names are lower-cased; entities (`&amp;`, `&#35;`, `&copy;`, …) are decoded (numeric references out of range become U+FFFD; an unknown named entity stays literal, as a browser leaves it); void elements (`img`, `br`, …) never nest; an open `<p>`/`<li>` is closed by the next block element; an unmatched close tag is ignored. Input is capped at 64 MiB (throws `RangeError`), nesting at depth 256, attributes at 512 per element. The tokenizer is lenient: it produces a tree for almost any text, and the scalar result of a bad parse is a shallow or empty tree, not an exception.

### HTMLStringify

**`HTMLStringify(nodes) -> string`**

- `nodes` *(node[])* — a node list from `HTMLParse` or built by hand.

Serializes a node list back to markup, re-escaping text. A node needs a string `name`; the depth bound is 256. Useful for round-tripping a modified tree.

### HTMLText

**`HTMLText(node | nodes) -> string`**

- `node` *(node | node[])* — a subtree or node list.

Returns the visible text of a subtree: concatenated string nodes with `<script>` and `<style>` contents skipped (returning JS source as page text is the classic scraping bug).

```js
import { HTMLParse, HTMLStringify, HTMLText } from "dyna:html";
const tree = HTMLParse('<div class="card"><h1>Hi &amp; bye</h1><p>A <b>bold</b> para</p><script>var x = 1</script></div>');
console.log(tree[0].name, tree[0].attrs.class);                    // div card
console.log(HTMLStringify(tree));                                  // the input, re-escaped
console.log(JSON.stringify(HTMLText(tree)));                       // "Hi & byeA bold para"
tree[0].children[0].children[0] = "Changed";
console.log(HTMLStringify(tree).includes("Changed"));              // true
```

### MarkdownToHTML

**`MarkdownToHTML(text, { allowRawHTML? }) -> string`**

- `text` *(string)* — markdown source.
- `allowRawHTML` *(boolean, optional)* — when true, raw HTML passes through.

Renders CommonMark-flavored markdown to HTML. Raw HTML in the source is escaped by default; `allowRawHTML: true` passes it through. Input is capped at 16 MiB (throws `RangeError`).

```js
import { MarkdownToHTML } from "dyna:html";
console.log(MarkdownToHTML("# Title\n\nSome *em* text"));          // <h1>Title</h1>...
console.log(MarkdownToHTML("<script>x()</script>", {}));           // escaped, not raw
```

### Selector

**`new Selector(css)`**

- `css` *(string)* — a CSS selector; the source is capped at 4096 bytes.

Compiles a CSS selector once. Supports tag, `.class`, `#id`, `[attr]`, `[attr=value]`, descendant and child combinators, and groups; a trailing orphan combinator throws a `SyntaxError` at construction. It runs against parsed trees, not strings.

**`selector.all(node | nodes) -> node[]`**

- `node` *(node | node[])* — a tree or node list.

Returns every matching node.

**`selector.first(node | nodes) -> node|undefined`**

- `node` *(node | node[])* — a tree or node list.

Returns the first match, or `undefined` when none.

**`selector.matches(node) -> boolean`**

- `node` *(node)* — the node to test.

Returns whether the node matches. A selector containing a combinator throws here: `matches` sees a single node with no ancestors.

```js
import { HTMLParse, Selector } from "dyna:html";
const doc = HTMLParse('<div><p class="x">a</p><p>b</p></div>');
const s = new Selector("p.x");
console.log(s.first(doc).children[0]);        // a
console.log(s.all(doc).length);               // 1
const sp = new Selector("p");
console.log(sp.matches(doc[0].children[0]));  // true
console.log(sp.matches(doc[0]));              // false
try { new Selector("p >"); } catch (e) { console.log(e.message); }
```

### Sanitizer

**`new Sanitizer({ allow, protocols? })`**

- `allow` *(object)* — maps a tag name to the array of attribute names it may keep.
- `protocols` *(object, optional)* — maps a `"tag.attr"` pair to the URL schemes its value may use (`{ "a.href": ["https", "http"] }`).

Builds an allow-list policy; there is no default policy, because a default is a policy nobody read. Matching is case-insensitive, and the scheme check strips leading control characters and spaces, so `java\tscript:` cannot slip through. Protocol-RELATIVE values (`//host/path`) carry no scheme and pass — they inherit the page's scheme, the standard browser behavior; deployments that must pin the scheme list `https`/`http` explicitly via `protocols`. A disallowed element loses its tag but keeps its children, except raw-text elements (`script`, `style`), whose content is dropped entirely.

**`sanitizer.clean(html) -> string`**

- `html` *(string)* — the HTML source.

Parses the input, drops every tag or attribute the policy does not allow, and re-escapes the survivors. Depth is bounded at 256.

Note on `style`: URL-sink attributes without an explicit `protocols` rule fall back to a
safe-scheme check over the WHOLE attribute value, and every CSS declaration contains a colon
(`color:red` reads as scheme `color`) — so a `style` attribute is dropped entirely unless a
`protocols` rule names it. CSS allow-listing is therefore only practical through `protocols`
or by dropping `style`; a url()-token-aware style check is future work.

```js
import { Sanitizer } from "dyna:html";
const san = new Sanitizer({
  allow: { p: [], a: ["href"], b: [] },
  protocols: { "a.href": ["https", "http"] }
});
const out = san.clean('<p onclick="x()">hi <a href="javascript:alert(1)">x</a> <a href="https://ok.test/">y</a></p>');
console.log(out);   // <p>hi <a>x</a> <a href="https://ok.test/">y</a></p>
const s2 = new Sanitizer({ allow: { em: [] } });
console.log(s2.clean("<div><em>keep</em><script>drop()</script>text</div>"));  // <em>keep</em>text
```

### rewriteLinks

**`rewriteLinks(doc, fn) -> number`**

- `doc` *(element | element[])* — a parsed tree node or root array (the `HTMLParse` shape).
- `fn` *(function)* — called as `fn(url, tag, attr)` for each URL-bearing attribute.

Rewrites URL-bearing attributes in place — the crawler's canonicalize step, one walk instead of hand-written recursion per project. Single-URL sinks (`href`, `src`, `action`, `formaction`, `cite`, `data`, `poster`, `background`, `ping`, `xlink:href`, `longdesc`) pass the whole attribute value to `fn`; `srcset` is parsed into candidates exactly like the HTML srcset grammar — a URL is a whitespace-delimited run, so an interior comma (a `data:` URI, a comma path) stays part of the URL, trailing commas on the URL end the candidate, and descriptors run to the next comma at paren depth 0 — and each candidate URL passes through `fn` (the original whitespace and descriptors preserved, candidates rejoined with `", "`). `tag` and `attr` reach `fn` lower-cased.

`fn` returning `undefined` or `null` keeps the original value; any other return replaces it (a non-string is coerced with ToString — `0` and `""` are replacements, not keeps). The return value is the number of attributes actually replaced (srcset candidates count each). Mutation is in place: a `fn` that throws leaves the earlier rewrites applied and its exception propagates out of the walk.

```js
import { HTMLParse, rewriteLinks } from "dyna:html";
const doc = HTMLParse('<a href="/p?x=1"><img src="/i.png" srcset="/2x.png 2x"></a>');
const n = rewriteLinks(doc, (url) => "https://ex.test" + url);
console.log(n);                                    // 3
console.log(doc[0].attrs.href);                    // https://ex.test/p?x=1
const kept = rewriteLinks(doc, (url) => url.includes("2x") ? "/cdn/2x.png" : undefined);
console.log(kept);                                 // 1 -- everything else kept
```

### Template

**`new Template(source)`**

- `source` *(string)* — the template source.

Compiles a Mustache-like template. `{{name}}` escapes; `{{{name}}}` and `{{&name}}` are raw; `{{#section}}…{{/section}}` renders per item for an array or once for a truthy value; `{{^section}}…{{/section}}` renders for a falsy one; `{{.}}` is the current item; `{{! comment }}` is dropped. A missing name renders as nothing; a function value is refused with a `TypeError`. `{{> partial}}` and `{{=delimiters=}}` throw at compile time. Source is capped at 4 MiB, node count at 65536, nesting at depth 64.

**`template.render(context) -> string`**

- `context` *(object)* — the render context.

Renders against a context object; sections see the enclosing scope too.

```js
import { Template } from "dyna:html";
const tpl = new Template("Hello {{name}}!{{#items}} <li>{{.}}</li>{{/items}}{{^items}} none{{/items}}");
console.log(tpl.render({ name: "World", items: [1, 2] }));   // Hello World! <li>1</li> <li>2</li>
console.log(tpl.render({ name: "Alone" }));                  // Hello Alone! none
console.log(new Template("{{escaped}} {{{raw}}}").render({ escaped: "<b>", raw: "<b>" }));
try { new Template("{{> partial}}"); } catch (e) { console.log(e.message); }
```

# dyna:scrape

The policy layer over fetch and parse: robots.txt handling, per-host pacing, retries and backoff, conditional requests, redirect and body bounds, and a schema-checked extractor. It owns no parsing — URLs are `dyna:url`, HTML is `dyna:html`, transport is an injected client — so everything here is the politeness nobody gets right. Bodies arrive as the injected client decoded them: transfer encoding (gzip, chunked), charsets and BOMs are the transport's boundary, and re-decoding legacy encodings belongs to `dyna:bytes`.

Every options bag in this module is strict: an unknown option key throws a `TypeError` naming the key and the valid set (`unknown option "agnt" (valid: agent)`), instead of being silently ignored. This covers the `Robots`, `Extractor` (constructor, field specs, `run`) and `Fetcher`/`Crawl` constructor bags.

`import { Robots, Extractor, Fetcher, Crawl } from "dyna:scrape";`

### Robots

**`new Robots(text, { agent? })`**

- `text` *(string)* — a robots.txt body.
- `agent` *(string, optional)* — default `"*"`.

Parses a robots.txt body (RFC 9309). The text is capped at 512 KiB while parsing, rules at 1000, a single pattern at 2048 bytes; groups at 64 with up to 8 `User-agent` lines each. Patterns support `*` (any run) and `$` (end anchor) with prefix semantics; `%2F`, `%2A` and `%24` stay encoded because RFC 9309 compares octets — a rule seeking a literal `*` or `$` must write `%2A`/`%24`, and a raw `*` or `$` in the url path is percent-encoded before comparison, so `/file-with-a-*.html` meets a `/file-with-a-%2A.html` rule. A bare `Disallow:` (empty value) means allow-all and adds no rule. A leading UTF-8 BOM is skipped — some CDNs serve BOM'd files and the first group must not be orphaned.

Group selection follows the crawler conventions: the group whose `User-agent` equals the configured agent wins; otherwise a product-token match (`User-agent: examplebot` matches agent `examplebot/2.3`) picks the group with the longest name; the `*` group applies only when nothing matches. Consecutive `User-agent:` lines share one group, so a record listing several bots is honoured for any of them.

**`robots.allows(path) -> boolean`**

- `path` *(string)* — the URL path.

Returns whether the path is allowed. Longest match wins, and an `Allow` beats a `Disallow` of equal length.

**`robots.crawlDelay() -> number|null`**

Returns the winning group's `Crawl-delay` in seconds, or `null` when none is set.

**`robots.sitemaps() -> string[]`**

Returns every `Sitemap:` URL in the file, in order.

**`robots.ruleCount`**

The number of rules of the winning group that survived parsing.

```js
import { Robots } from "dyna:scrape";
const rb = new Robots("User-agent: *\nDisallow: /private/\nAllow: /private/pub$\nCrawl-delay: 2\nSitemap: https://example.test/sitemap.xml\n");
console.log(rb.allows("/private/a"), rb.allows("/private/pub"), rb.allows("/open"));   // false true true
console.log(rb.crawlDelay(), rb.ruleCount);                                            // 2 2
const tok = new Robots("User-agent: *\nDisallow: /\n\nUser-agent: mybot\nAllow: /\n", { agent: "mybot/1.2 (+https://x.test)" });
console.log(tok.allows("/x"));                                                        // true -- token match won its group
console.log(new Robots("User-agent: *\nDisallow: /*.pdf$\n").allows("/a/b.pdf"));      // false
console.log(new Robots("User-agent: *\nDisallow:\n").allows("/anything"));             // true
```

### Extractor

**`new Extractor(spec, { text? })`**

- `spec` *(object)* — the field spec. Each field is `{ sel, attr?, all?, required?, trim?, source?, default?, as? }`.
- `text` *(function, optional)* — `dyna:html`'s `HTMLText`, injected for text extraction.

Compiles a field spec against a schema. `sel` must be a `Selector` instance (a second CSS engine is a second thing to keep correct); `attr` reads a node attribute instead of text; `source` takes the node's raw child source — how `<script>`/`<style>` bodies are read, since `HTMLText` deliberately refuses to serve code as page text; `all` collects every match into an array; `required` flags layout drift by naming the empty field; `trim` strips surrounding whitespace from strings before any coercion; `default` substitutes a value when the field finds nothing — for scalar fields the missing value, for `all` fields the EMPTY array, replaced wholesale — keeps the field out of `missing`, and opts it out of the required alarm (the author has already said what absence means); `as` coerces and validates:

- `"number"` — a non-numeric string is refused (never `NaN`);
- `"url"` — resolves against `run()`'s `base`; a relative URL without a base is refused;
- `"json"` — parses the text (`ld+json`, embedded config); invalid JSON is refused like any other miss. Any other `as` string refuses at construction — an ignored typo would silently drop a coercion, which is the drift `required` exists to catch.

**`extractor.run(doc, { base? }) -> { ok, value, missing }`**

- `doc` *(node[])* — a parsed document.
- `base` *(string, optional)* — the base URL for `as: "url"` resolution.

Runs the spec. `ok` is false when any `required` field came back empty even after defaults, and `missing` names them.

```js
import { Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";
const ex = new Extractor({
  title: { sel: new Selector("h1"), trim: true },
  hrefs: { sel: new Selector("a"), attr: "href", all: true },
  price: { sel: new Selector(".price"), as: "number", required: true },
  ldjson: { sel: new Selector("script[type='application/ld+json']"),
            source: true, as: "json" }
}, { text: HTMLText });
const doc = HTMLParse('<h1> T </h1><a href="/a">x</a><p class="price">12.50</p>');
const r = ex.run(doc, { base: "http://example.test/" });
console.log(r.ok, r.value.title, r.value.price, r.value.hrefs[0]);   // true T 12.5 http://example.test/a
const gone = new Extractor({ gone: { sel: new Selector(".gone"), required: true }, alt: { sel: new Selector(".gone"), default: "-" } }, { text: HTMLText });
console.log(JSON.stringify(gone.run(doc).missing));                  // ["gone"]
```

### Fetcher

**`new Fetcher({ agent, client?, headers?, revalidate?, robots?, robotsTtlMs?, minDelayMs?, retries?, maxRedirects?, maxBodyBytes?, allowPrivateHosts?, allowInsecureDowngrade?, proxy?, ca?, poolSize? })`**

- `agent` *(string)* — required; there is no default user agent (a shared one is indistinguishable from anonymous).
- `client` *(object, optional)* — an injected `dyna:net` `HTTPClient` (or a mock with a `request(method, url, body, headers)` method), which lets a test drive the policy against a mock. **Omitted**, the module constructs its own `HTTPClient` — capped at this fetcher's `maxBodyBytes` — so a one-page fetch needs no `dyna:net` import and no hand-built client.
- `proxy` *(string, optional)* — a proxy URL: a non-empty string, anything else a `TypeError`. Stored and echoed by `stats()`. The built-in transport opens direct connections and does not route through it — a proxied fetch is an injected `client` configured for one (a client carries its own transport). The option is recorded, validated and observable rather than refused or silently dropped, so a shared configuration reads back exactly.
- `ca` *(string, optional)* — a CA bundle path/PEM (non-empty string). Same contract as `proxy`: stored, echoed by `stats()`; the built-in transport uses the platform trust store.
- `poolSize` *(number, optional)* — an integer 1..64, default 4. The type is strict: a non-number or a non-integer (a string `"5"`, `1.5`, `NaN`, `Infinity`) throws a `TypeError` naming the option, and an integer outside the range throws `RangeError`. Stored and echoed by `stats()`; the built-in transport opens one connection per request, so this sizes a pool only where the transport has one.
- `headers` *(object, optional)* — extra request headers sent on every request including robots.txt; the crawler's own `User-Agent` always wins a collision.

Extra headers reach every host a crawl touches — redirect targets included — so credential-bearing keys (`Authorization`, `Cookie`, `Proxy-Authorization`, `Set-Cookie`) are refused at construction with a `TypeError` naming the key; authenticated fetching belongs to `dyna:http`, not to a crawler that follows redirects.
- `revalidate` *(boolean, optional)* — default true; see conditional requests below.
- `robots` *(boolean, optional)* — default true.
- `robotsTtlMs` *(number, optional)* — robots.txt refresh interval, default 24 h; 0 re-fetches per request. A failed refresh keeps serving the last-known-good copy (RFC 9309 2.3.1.6) rather than falling to a fresh disallow, and backs off by the same interval.
- `minDelayMs` *(number, optional)* — default 1000.
- `retries` *(number, optional)* — default 3.
- `maxRedirects` *(number, optional)* — default 5.
- `maxBodyBytes` *(number, optional)* — default 8 MiB.
- `allowPrivateHosts` *(boolean, optional)* — default false.
- `allowInsecureDowngrade` *(boolean, optional)* — default false; a redirect that moves a page from https to http is refused with a `RangeError`, because no Location header gets to strip the crawler's transport security silently.

Constructs a resource that fetches one URL through all the policy at once. The SSRF gate is on by default: loopback, private, link-local, and single-label hosts are refused with a `TypeError` unless `allowPrivateHosts: true` — on every redirect hop and robots redirect too, not just the first request. Its boundary, stated plainly: this gate classifies what the URL NAMES. A literal IP is fully checked; a hostname that RESOLVES into a private range (DNS rebinding) passes here, because resolution belongs to the injected client — `dyn:http`'s connect performs no address-class check today. A crawler that must never touch internal networks needs an egress allowlist at the network boundary; no name-based, in-process check can honestly promise more.

**`fetcher.get(url) -> { status, headers, body, url, fromCache, notModified, contentType, robotsDirectives?, canonicalUrl? }`**

A request-url fragment is stripped before anything else: it is client-side state, never sent on the wire, never part of the robots check, and never part of a dedup key (RFC 9110). Performs one policy pass: per-host delay floor (a `Crawl-delay` raises it, never lowers), robots gate, request with retries and jittered exponential backoff (429 and 5xx retried; a `Retry-After` header overrides the curve untouched — both RFC 7231 forms, delay-seconds and IMF-fixdate HTTP-date — and jitter never touches a server-given delay), redirect chase, body cap (over-large throws `RangeError`). Redirect locations resolve per RFC 3986 — relative paths, dot segments, protocol-relative and cross-host hops all work, and each hop re-runs the SSRF gate on its host. A robots refusal returns `status: 0` with `skippedByRobots: true`. A fragment-only `Location` (which resolves back to the same URL) ends the chain instead of spinning to the cap.

`maxBodyBytes` is checked TWICE by design: on the declared `Content-Length` before any body is trusted — an honest server is refused in time and nothing is transferred — and again on the bytes actually received, so a server whose header lies still cannot exceed retention.

Every response carries `contentType` verbatim from the origin (`""` when absent or skipped). It is deliberately NOT filtered here: extraction of an `image/png` or `application/pdf` body would be silent garbage, so the type is surfaced for callers to branch on — and a revalidation hit replays the STORED content-type rather than an empty one.

The store honours `Cache-Control: no-store` — such responses are fetched unconditionally every time. `no-cache` and `max-age=0` are already the store's behaviour by construction, since every replay is revalidated.

Conditional requests are on when `revalidate` holds: when a response carries `ETag` or `Last-Modified`, they are stored under the request URL (bounded: 32 entries, 1 MiB per body, 4 MiB total, FIFO eviction) and replayed as `If-None-Match` / `If-Modified-Since`. A `304` answer comes back with `status: 304`, the **cached body**, `fromCache: true` and `notModified: true` — the status says exactly what the wire said, and the stats show what it saved. Fetches without validators behave exactly as before.

Robots.txt is fetched once per host, before any other request, following up to 3 redirects per RFC 9309 2.3.1.2 — the canonical-host 301 is the norm, not an anomaly. Any hop that is unresolvable, points at a private host without the opt-in, or downgrades https to http ends the chain as "undefined file": the whole host is treated as fully disallowed, exactly as RFC 9309 2.3.1.4 requires for one that could not be fetched. The file is refreshed every `robotsTtlMs`; a refresh that fails serves the previous good copy.

Every response also surfaces the page-level robots signals a modern crawler must honour: `robotsDirectives` is the `X-Robots-Tag` header's directives that apply to THIS fetcher's agent (comma-separated and multiple headers joined, `bot:`-scoped rules filtered against the agent's product token, `none` expanded to `noindex, nofollow`), and `canonicalUrl` is the `Link: <url>; rel="canonical"` header (RFC 8288) resolved against the request url, when one is present. `Crawl` consumes both: `nofollow` (from either source) gates link following, and the canonical header dedups like the HTML canonical.

**`fetcher.getAsync(url) -> Promise<response>`**

The same policy pass as `get()` — robots gate, per-host delay floor, retries with `Retry-After`, redirect chase, body caps — settled as a promise, so one-page fetches compose with `await` and never construct a `Crawl`. It never throws synchronously: a transport error becomes a REJECTION carrying the cause, and a policy skip (a robots refusal) resolves with the status-0 response `get()` returns. A missing url rejects.

```js
import { Fetcher } from "dyna:scrape";
const client = { request(method, url) {
  return url.endsWith("/robots.txt")
    ? { status: 404, headers: {}, body: "" }
    : { status: 200, headers: {}, body: "page:" + url };
} };
const f = new Fetcher({ agent: "bot/1.0 (+https://example.test/bot)", client,
                        robots: false, minDelayMs: 0 });
const r = await f.getAsync("http://origin.test/a");
console.log(r.status, r.body);            // 200 page:http://origin.test/a
const boom = new Fetcher({ agent: "bot/1.0", minDelayMs: 0, robots: false,
                           client: { request() { throw new Error("refused"); } } });
await boom.getAsync("http://origin.test/b")
  .catch((e) => console.log(e.message));  // refused
f.close(); boom.close();
```

**`fetcher.stats() -> { fetched, skippedByRobots, retried, throttledMs, bytes, revalidated, savedBytes, proxy, ca, poolSize }`**

Returns the crawl statistics; `savedBytes` sums cached-body sizes served on revalidation. `proxy`, `ca` and `poolSize` echo the stored simple options — `null`, `null` and `4` when unset — so a shared configuration reads back instead of vanishing. A crawler whose politeness cannot be observed cannot be trusted.

**`fetcher.getStream(url) -> FetcherStream`** (+ CC-2-scrape)

Fetches `url` under the SAME policy as `get` — robots gate (including the status-0 `skippedByRobots` shape, with the body at EOF), per-host delay floor, retries with `Retry-After` and jittered backoff (a server delay beyond 60s is served as the final answer), redirect chase with the private-host and https-downgrade gates, `maxBodyBytes`, the SSRF name gate, this fetcher's agent and `headers` — and returns the body as a live stream instead of a string. The returned object is the RESPONSE and the ByteSource in one: `{ status, statusText, ok, headers, url, contentType, read(buf), close(), dispose(), [Symbol.dispose], closed }`.

`read(buf)` fills the caller's buffer with up to `buf.length` body bytes and resolves with the count; `0` is the end. That is exactly dyna:stream's ByteSource shape, and the duck typing is the integration: `stream.pipe(resp, sink)`, `stream.lines(resp)`, `stream.ndjson(resp)` consume the response directly — dyna:scrape neither imports nor links dyna:stream.

Bounds and failures, precisely: a DECLARED `Content-Length` over `maxBodyBytes` refuses before the body starts (the same `RangeError` `get` throws). Bytes received are counted as they are handed out, so crossing `maxBodyBytes` rejects the current read and closes the stream. A connection that ends before a declared `Content-Length` is a truncated body: the read rejects naming the bytes served and declared — never a silent `0`. After any rejection every later read rejects the same error. Reads answer on the promise, never by throwing.

Transport, stated honestly: `getStream` does NOT go through the injected client. `request(method, url, body, headers)` buffers the whole body by contract, so a stream through it could only fake streaming. This path owns its sockets — plain `http://`, and `https://` under `CONFIG_TLS` with the platform trust store (the engine client's defaults) — always sends `Accept-Encoding: identity` (a compressed body would need a streaming inflater between socket and caller; that is dyna:stream `inflate()`'s job when wanted), and uses fixed 15s connect/recv timeouts. Not carried over from `get()`, on purpose: `revalidate`/conditional-GET (a 304 replay needs a buffered copy) and the response decorations (`X-Robots-Tag`, `Link: rel=canonical`) — a stream is consumed, not parsed for directives.

```js
import { Fetcher } from "dyna:scrape";
const f = new Fetcher({ agent: "bot/1.0 (+https://example.test/bot)", minDelayMs: 0 });
// The stream path owns its sockets, so the offline-checkable arm is a refusal:
// the SSRF gate answers before any connection is attempted.
let refused = false;
try { f.getStream("http://127.0.0.1:1/feed.ndjson"); }
catch (e) { refused = /private\/loopback/.test(e.message); }
console.log(refused);                              // true
f.close();
// Against a live origin the same call returns a response that is ALSO the
// ByteSource: `await stream.pipe(resp, sink)`, `stream.lines(resp)`,
// `stream.ndjson(resp)` -- tests/test_scrape_stream.js runs that path.
```

**`fetcher.close()`**

Releases per-host state and the validator store; `fetcher.closed` is true afterwards.

**`fetcher.dispose()`**

Releases per-host state; `fetcher.closed` is true afterwards.

```js
import { Fetcher } from "dyna:scrape";
const seen = [];
let etagSeq = 0;
const client = {
  request(method, url, body, headers) {
    seen.push([url, headers["User-Agent"], !!headers["If-None-Match"]]);
    if (url.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
    if (!etagSeq++) return { status: 200, headers: { ETag: '"v1"' }, body: "<h1>ok</h1>" };
    return { status: 304, headers: {}, body: "" };
  }
};
const f = new Fetcher({ agent: "mybot/1.0 (+https://example.test/bot)", client, minDelayMs: 0 });
try { f.get("http://127.0.0.1/secret"); } catch (e) { console.log(e.message); }  // refused: private host
const res = f.get("http://example.test/page");
const again = f.get("http://example.test/page");
console.log(res.status, again.status, again.notModified, again.body);   // 200 304 true <h1>ok</h1>
console.log(seen[1][2], f.stats().savedBytes > 0);                      // true true
f.close();
```

### Crawl

**`new Crawl(fetcher, { maxPages?, maxDepth?, sameHost?, linkField?, baseField?, canonicalField?, relField?, robotsField?, concurrency? })`**

- `fetcher` *(Fetcher)* — the `Fetcher` to drive the traversal.
- `maxPages` *(number, optional)* — default 100.
- `maxDepth` *(number, optional)* — default 2.
- `sameHost` *(boolean, optional)* — default true; compared case-insensitively with the scheme's default port ignored (`EXAMPLE.com:80` IS `example.com`).
- `linkField` *(string, optional)* — default `"links"`.
- `baseField` *(string, optional)* — names an extractor string field that carries the page's effective base (e.g. read `<base href>` via `attr`). The declaration itself may be relative and resolves against the page first. Unset means links resolve against the request url.
- `canonicalField` *(string, optional)* — names an extractor string field carrying the page's canonical url (`link[rel=canonical]`, attr `href`). When that canonical is an absolute http(s) url different from the page and it has already been seen, the page is a DUPLICATE: it still emits, but its links are not queued. Relative or malformed canonicals are ignored, never guessed; a canonical pointing somewhere not yet seen marks nothing — the target stays fetchable. The `Link: <url>; rel="canonical"` response header (RFC 8288) dedups the same way.
- `relField` *(string, optional)* — names a parallel extractor array of rel strings (one per link, same order). Any slot whose text carries the `nofollow` token politeness-skips that link.
- `robotsField` *(string, optional)* — names an extractor string field carrying the page's `<meta name="robots" content="...">` content. Its `nofollow` gates link following exactly like the `X-Robots-Tag` header's; the directives join `page.robots`.
- `concurrency` *(number, optional)* — default 1, range 1..16: how many page fetches are kept in flight at once. The per-host politeness floor is unchanged — `concurrency` is parallelism across the frontier, not a stampede at one host. Outside 1..16 throws a `RangeError`. The setting round-trips through `serialize()`/`resume()`.

Value extraction runs on ANY status: a 403 or 500 page can still carry data worth keeping, and a bad page is exactly when a scraper needs its extractor. LINK FOLLOWING is gated to 2xx — error pages are not trusted to point anywhere. Constructs a bounded traversal that composes `Fetcher` and `Extractor`. Execution is source-level only — there is no JavaScript evaluation, so script-rendered SPA shells, and client-side techniques like `meta refresh` rewrites, are invisible to it by design; pages advertising themselves through those arrive exactly as their markup arrived. It never scans HTML for links itself: it reads them from a named field of the extractor's output, so there is one link predicate (the real parser's). Links resolve against the page that emitted them (`./b`, `../c`, `//other.test/d` all work); fragments, empty values and non-navigational schemes (`javascript:`, `mailto:`, `tel:`, `data:`) are dropped. The pending frontier is capped at 10000 entries — a hostile page cannot convert `maxPages` into unbounded queue memory. The seed URL must be `http(s)://`.

**`crawl.start(seed, extractor?, parseFn?) -> this`**

- `seed` *(string)* — the seed URL, deduplicated.
- `extractor` *(Extractor, optional)* — the extractor to run.
- `parseFn` *(function, optional)* — e.g. `dyna:html`'s `HTMLParse`; converts each body before extraction.

Primes the frontier with the seed and stores the extractor. Returns the `Crawl` itself, which is iterable.

**`crawl.next() -> { value, done } | Promise<{ value, done }>`**

Returns one page per call, so a bound of 100 pages does not mean 100 fetches before the first result. Each value is `{ url, depth, status, value, robots? }` — `robots` is the merged directive list from `X-Robots-Tag` and `robotsField` (when either exists), and `nofollow` from either source stops this page's links from being followed while the page itself still emits. At `concurrency` 1 the result is a plain `{ value, done }`; above 1 `next()` returns a promise of one, and the pages being fetched in parallel are the crawl's in-flight promises.

**`crawl.pages()`**

Returns the `Crawl` (the iterator); `[Symbol.iterator]` is the same, so `for (const p of crawl) …` works at `concurrency` 1. Every crawl also carries `[Symbol.asyncIterator]`, so `for await (const p of crawl) …` works at any `concurrency` and is the natural form above 1.

**`crawl.serialize() -> string`**

The crawl's resumable state as a versioned JSON string: the frontier (remaining urls with depths, in FIFO order), the visited dedup keys, the bounds (page/depth limits and `concurrency`), the field names and the counters. Throws `TypeError` on a crawl that was never started. What is deliberately NOT in the state: in-flight fetches (a page mid-fetch is dropped by a resume), the fetcher's own live state (robots copies, per-host delay floors, the conditional-GET validator cache — those live on the `Fetcher` you resume with), and every JS object (the fetcher, extractor and parser are arguments of `resume`, not data). Serialize a quiescent crawl when the remaining page set must be exact.

**`Crawl.resume(fetcher, state, extractor?, parseFn?) -> Crawl`** (static)

Rebuilds a crawl from `serialize()` output — a static, because the state outlives any one instance. Bounds, field names, frontier, visited set and counters come from the state, so the remaining page set — and its order — matches the donor's exactly; the state's page budget keeps bounding it (an exhausted budget resumes as an already-finished crawl). The fetcher/extractor/parser are supplied fresh. Any malformed or wrong-version state is refused with a `TypeError` naming the problem — never half-restored: the state file is untrusted input and every field is re-validated on the way in.

```js
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";
const pages = { "/": '<h1>P</h1><a href="/b">x</a>', "/b": '<h1>Q</h1>' };
const client = {
  request(method, url) {
    if (url.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
    return { status: 200, headers: {},
             body: pages[url.slice("http://example.test".length).split("?")[0]] ?? "" };
  }
};
const mkEx = () => new Extractor({
  title: { sel: new Selector("h1") },
  links: { sel: new Selector("a"), attr: "href", all: true },
}, { text: HTMLText });
const f = new Fetcher({ agent: "bot/1.0", client, minDelayMs: 0 });
const donor = new Crawl(f, { maxPages: 10, maxDepth: 3 }).start("http://example.test/", mkEx(), HTMLParse);
const first = donor.next().value;            // page 1 emitted
const state = donor.serialize();             // checkpoint the frontier + visited set
const f2 = new Fetcher({ agent: "bot/1.0", client, minDelayMs: 0 });  // e.g. next process
const rest = Crawl.resume(f2, state, mkEx(), HTMLParse);  // continues where the donor stopped
console.log(first.url.replace("http://example.test", ""),
            rest.next().value.url.replace("http://example.test", ""));
// / /b
```

```js
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";
const pages = {
  "/": '<h1>P</h1><a href="/sub/a">x</a><a href="#top">y</a><a href="javascript:void(0)">z</a>',
  "/sub/a": '<h1>Q</h1><a href="../deep/b">go</a>',
  "/deep/b": '<h1>R</h1>'
};
const client = {
  request(method, url) {
    if (url.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
    const path = url.slice("http://example.test".length).split("?")[0];
    return { status: 200, headers: {}, body: pages[path] ?? "" };
  }
};
const f = new Fetcher({ agent: "bot/1.0", client, minDelayMs: 0 });
/* without an extractor, p.value is undefined; pass one to extract fields */
const ext = new Extractor({
  title: { sel: new Selector("h1") },
  links: { sel: new Selector("a"), attr: "href", all: true }
}, { text: HTMLText });
const c = new Crawl(f, { maxPages: 3, maxDepth: 2 });
for (const p of c.start("http://example.test/", ext, HTMLParse))
  console.log(p.url.replace("http://example.test", ""), p.value.title);
// / P
// /sub/a Q
// /deep/b R
```

### Sitemap

`Robots.sitemaps()` hands out sitemap URLs, and until now nothing in the runtime could read one. `Sitemap` is a namespace (parse/list are stateless — an instance would hold nothing), not a class.

**`Sitemap.parse(xml) -> string[]`**

Parses sitemap XML into the `<loc>` list, in document order. A `<urlset>` yields its page URLs; a `<sitemapindex>` yields its child sitemap URLs. The scanner is a bounded walker over the sitemap vocabulary, not a general XML parser (dyna:xml exists for that): comments, PIs, DOCTYPE and CDATA are skipped, attributes (and the default `xmlns`) are ignored, the five named entities plus numeric references decode inside `<loc>` (the `&amp;` in a query string is the case that exists for), and element names match case-insensitively. Structural breakage — an unterminated comment, a `<loc>` that never closes — throws `SyntaxError` naming the byte offset, never a silently truncated list; text that is not XML at all (a robots.txt fed to the wrong function) throws `TypeError`; a well-formed urlset with no `<loc>` entries is simply `[]`. Bounds, because sitemaps are attacker-influenced input exactly like robots.txt: 16 MiB of XML, 50000 entries (the spec's per-sitemap cap) and 2048 bytes of `<loc>` text (the spec's URL budget) — over any of them refuses.

**`Sitemap.list(url, source?) -> string[]`**

Fetch + parse + flatten. A `<sitemapindex>` is followed ONE level: each child is fetched in turn and its list appended; a child that is itself an index has its entries returned as-is (two levels would need a cycle policy, and the spec's index files point at urlsets). A non-2xx answer throws `RangeError` naming the status. `source` is any object with a `get(url)` method — a `Fetcher`, or a mock — or an options bag for building one (`agent`, `headers`, `minDelayMs`, `allowPrivateHosts`, `maxBodyBytes`, `retries`: the Fetcher ctor's keys); omit it and the module fetches with its own fetcher under the agent `dyna-sitemap/1.0`, named so an operator can see and rate-limit it. Duplicates, if a document ships them, are preserved: Crawl is the dedupper, `list()` is a reader.

```js
import { Sitemap, Fetcher } from "dyna:scrape";
const xml = `<?xml version="1.0"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url><loc>http://example.test/a</loc><priority>0.8</priority></url>
  <url><loc>http://example.test/b?x=1&amp;y=2</loc></url>
</urlset>`;
console.log(Sitemap.parse(xml));   // [ "http://example.test/a", "http://example.test/b?x=1&y=2" ]
console.log(Sitemap.parse('<sitemapindex><sitemap><loc>http://example.test/s.xml</loc></sitemap></sitemapindex>'));
// [ "http://example.test/s.xml" ]
/* list() through a mock (the same shape a real Fetcher presents): */
const f = new Fetcher({ agent: "doc-bot/1.0", client: {
  request(method, url) {
    if (url.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
    return { status: 200, headers: {}, body: xml };
  }
}, minDelayMs: 0, allowPrivateHosts: true });
console.log(Sitemap.list("http://example.test/s.xml", f));  // [ "http://example.test/a", "http://example.test/b?x=1&y=2" ]
```
# dyna:cli

Argument parsing, terminal interaction and ANSI styling for command-line programs: a yargs-style `Command`, `prompt`/`confirm`/`select`/`keypress` for reading from the terminal, `ProgressBar`/`Spinner`/`Table` for reporting, and `StyleText`/`Styles`/`IsTTY`/`Columns`/`ColorDepth` for deciding how to render.

`import { Command, StyleText, Styles, IsTTY, Columns, ColorDepth, prompt, confirm, select, keypress, ProgressBar, Spinner, Table } from "dyna:cli";`

Every options bag in this module is STRICT: an unknown key throws a `TypeError` naming the key and the valid set (e.g. `unknown option "defualt" (valid: default)`). Only a `null` or `undefined` bag counts as absent; any other non-object (a primitive, a string) is refused with `TypeError`. Symbol and inherited keys are not considered.

### Command

**`new Command(name?)`**

- `name` *(string, optional)* — the command name.

Constructs an argument parser built from declarative specs. Bounds on configuration: 256 options, 64 arguments, 64 subcommands, 65536 argv elements (each a `RangeError` past its cap).

**`command.describe(text)`**

- `text` *(string)* — the description.

Sets the description shown by `help()`.

**`command.option(flags, desc?, { type?, required?, variadic?, default?, env? })`**

- `flags` *(string)* — a spec string like `"-e, --env <name>"` (short name optional, placeholder marks a value-taking option); a long name is required, so `"-e"` alone is refused.
- `desc` *(string, optional)* — the option description.
- `type` *(string, optional)* — `"boolean"`, `"string"` (both inferred from the placeholder), or `"number"` (a non-numeric value throws `TypeError` — no yargs-style implicit coercion). A number value must spell a finite decimal number: optional surrounding ASCII whitespace, an optional sign, digits with an optional fraction and exponent (`8`, `-.5`, `1e3`). JavaScript's own number spellings are refused — `NaN`, `Infinity` and `0x10` would arrive as surprising values (a `NaN`, a valid double, `16`) where `expects a number` promises a refusal — and so is anything that overflows to infinity (`1e999`).
- `required` *(boolean, optional)* — the option must be present.
- `variadic` *(boolean, optional)* — collects repeated occurrences into an array.
- `default` *(any, optional)* — seeds the parsed object.
- `env` *(string, optional)* — an environment variable used as the option's value when the flag is absent. Precedence per option: the CLI beats `env` beats `default`. `env` is consulted lazily — the CLI is resolved first and the variable is neither read nor validated for an option the command line supplies, so a poisoned variable can never refuse (or replace) an explicit CLI value. A boolean env maps `0`/`false`/`no`/`n`/`off`/empty (ASCII case-insensitive) to `false` and any other non-empty value to `true`; a number env is validated through the exact grammar above (CLI and env never drift apart), and a bad value throws naming the variable. A variadic option's env value seeds a one-element array (replacing any `default`); when the CLI supplies the option, its occurrences stand alone and the variable is not consulted. `help()` notes `(env: NAME)`.

Declares an option.

**`command.argument("<name>" | "[name]", desc?)`**

- `name` *(string)* — a positional; angle brackets are required, brackets optional, a trailing `...` makes it variadic.
- `desc` *(string, optional)* — the argument description.

Declares a positional argument.

**`command.command(sub)`**

- `sub` *(Command)* — a `Command` to register.

Registers a subcommand; the first non-option token that matches a subcommand name dispatches the whole tail to it.

**`command.allowUnknown(true?)`**

- `true` *(boolean, optional)* — pass unknown options through as positionals instead of throwing.

**`command.action(fn)`**

- `fn` *(function)* — called as `fn(options, args)` once a parse of this command settles.

Registers the action handler. When `parse()` matches this command and nothing dispatches to a subcommand, the handler runs and its return value becomes the parse result's `result` field. Anything that is not a function is refused.

**`command.version(v?)`**

- `v` *(string, optional)* — the version string.

With a string, sets the version (chainable) and enables the automatic `--version` flag; with no argument, returns the configured version (`""` when unset). `parse()` answers `--version` with a result carrying `version` and does not run the action. Scope follows dispatch: `tool --version sub` reports the root's version even though a subcommand follows, while `tool sub --version` dispatches first and the subcommand answers with its own (a subcommand without `version()` refuses the flag as unknown). `--version=value` throws — the flag takes no value. `help()` shows `Version: ...`.

**`command.parse(argv) -> { options, arguments, command, result?, version? }`**

- `argv` *(string[])* — the argv array.

Parses an argv array. `options` is keyed by long name. Each option resolves on its own, CLI first: if the command line supplies it, its parsed value(s) are used and `env` is never consulted; otherwise the `env` variable (when declared and set) provides the value — for a variadic option as a one-element array that replaces any `default` — and otherwise the `default` stands. A variadic option supplied on the CLI collects its repeated occurrences into an array (pushing onto the seeded `default` array). `arguments` is the positional array. With a subcommand, `command` is its name and `result` is the subcommand's own parse result; with an action handler, `result` is the handler's return value. A `--version` answer carries `version` instead. Required options and arguments are checked after the whole argv is consumed, and their absence throws. An argv above 65536 entries throws `RangeError`; `parse()` with no argument parses the engine's own `scriptArgs` under the same rules.

**`command.help() -> string`**

Returns a usage text built from the specs.

**`command.name`**

The configured name.

```js
import { Command } from "dyna:cli";
const cmd = new Command("deploy")
  .describe("Deploy a service")
  .option("-e, --env <name>", "environment", { required: true })
  .option("-v, --verbose", "verbose logging")
  .option("-r, --retries <n>", "retry count", { type: "number", default: 3 })
  .option("-t, --tag <name>", "repeatable tag", { variadic: true })
  .argument("<service>", "service name");
const r = cmd.parse(["-v", "--env", "prod", "--retries", "5", "--tag", "a", "--tag", "b", "api"]);
console.log(r.options.env, r.options.verbose, r.options.retries, r.options.tag.join(","));   // prod true 5 a,b
console.log(r.arguments[0]);                          // api
console.log(cmd.parse(["--env", "prod", "api"]).options.retries);   // 3 (the default)
console.log(cmd.help().startsWith("Usage: deploy [options] <service>"));
try { cmd.parse(["--env", "prod"]); } catch (e) { console.log(e.message); }  // required argument "service" is missing
try { cmd.parse(["api", "--wat"]); } catch (e) { console.log(e.message); }   // unknown option "--wat"
const git = new Command("git")
  .option("-q, --quiet", "quiet")
  .command(new Command("clone").argument("<url>"));
const sub = git.parse(["-q", "clone", "http://x"]);
console.log(sub.command, sub.options.quiet, sub.result.arguments[0]);       // clone true http://x
const tool = new Command("tool")
  .version("2.1.0")
  .option("--level <n>", "log level", { type: "number", env: "TOOL_LEVEL", default: 1 })
  .action((options, args) => "ran:" + args.join(","));
console.log(tool.parse(["a", "b"]).result);            // ran:a,b
console.log(tool.parse(["--version"]).version);        // 2.1.0
console.log(tool.parse([]).options.level);             // 1 (or $TOOL_LEVEL when set)
try { tool.parse(["--version=x"]); } catch (e) { console.log(e.message); }  // option "--version" is a flag and takes no value
```

### prompt, confirm, select, keypress

The interactive readers write their prompt to stdout and read stdin. `select` and `keypress` put a TTY into raw mode (no echo, no line buffering, no signals) for the duration of one read and restore it before returning; piped input works without a terminal. The mode switches discard nothing: input typed during the raw window is queued for the next reader instead of being flushed with the restore (so a `keypress()` followed by a `prompt()` receives everything typed after the key it consumed), and type-ahead queued before raw mode was engaged is preserved too.

**`prompt(message, { default? }) -> string`**

- `message` *(string)* — written to stdout verbatim.
- `default` *(string, optional)* — returned for an empty answer.

Reads one line from stdin (the terminator and a CRLF's `CR` are dropped). An empty line, or EOF, yields `default` — or `""` without one. An answer longer than 1 MiB throws `RangeError`, counted **after** that `CR` drop — exactly 1 MiB of content is accepted with either `LF` or `CRLF`, one content byte more is refused with either — and the remainder of a refused line is drained first, so the stream stays line-aligned for the next reader.

**`confirm(message) -> boolean`**

- `message` *(string)* — written to stdout verbatim.

Reads one line from stdin. `"y"`/`"yes"` (ASCII case-insensitive) answers `true`; everything else — an empty line, `"n"`, `"maybe"`, EOF — answers `false`.

**`select(message, options) -> string | null`**

- `message` *(string)* — written to stdout above the menu.
- `options` *(string[])* — a non-empty array of menu entries.

Renders the menu with a `>` marker on the highlighted row and redraws it in place as the arrow keys move the highlight (wrapping). Enter returns the highlighted option's string. Escape, Ctrl-C and EOF cancel and return `null`. Any other key is ignored. `options` must be a non-empty array of strings (65536 entries at most).

**`keypress() -> { name, sequence, ctrl, meta, shift } | null`**

Reads and decodes one key event from stdin; `null` at EOF.

- `name` *(string | null)* — `up`/`down`/`left`/`right`, `home`, `end`, `insert`, `delete`, `pageup`, `pagedown`, `enter`, `tab`, `backspace`, `escape`, or the character itself for a printable key (including a full UTF-8 character). Ctrl chords name their letter (`{name: "c", ctrl: true}`); `null` means the bytes matched no known key.
- `sequence` *(string)* — the input the event consumed (UTF-8 decoded; a malformed run appears as `U+FFFD`). Byte accounting is exact: an event never consumes a byte `sequence` does not report. A byte that cannot extend the current event — a non-continuation after a truncated UTF-8 lead, input past the 15-byte `sequence` cap (an escape run that long becomes several events), or a CSI parameter byte past the 11-byte parameter buffer, which ends the sequence at that byte — is delivered as the NEXT event's input instead of being swallowed.
- `ctrl`, `meta`, `shift` *(boolean)* — the modifiers. `meta` is the `ESC` prefix; shift+tab and the `;2~`-style CSI modifiers set `shift`/`meta`/`ctrl` accordingly.

Escape decoding has one inherent timing rule: a lone `ESC` is told apart from the head of a sequence only by a quiet window of 50 ms after it, and the same window bounds every gap *inside* a sequence (escape continuation and UTF-8 continuation alike). Identical bytes decode identically as long as the gaps between them stay inside the window — where they land across `read()` calls is irrelevant — but push a gap past 50 ms and the event before the gap ends where it stands (a bare `ESC` becomes `escape`, a half-read sequence becomes a `name: null` event, a truncated character becomes `U+FFFD`), with the remaining bytes delivered as later events. A CSI's parameter overflow, in contrast, is purely a function of the byte sequence: parameter bytes past the buffer end the sequence at that byte and the rest decode as their own keys.

<!-- check:skip -->
```js
import { prompt, confirm, select, keypress } from "dyna:cli";
const name = prompt("Name: ", { default: "world" });  // empty answer or EOF -> "world"
console.log("hello " + name);
if (confirm("Deploy to prod? [y/n] ")) console.log("deploying");
const picked = select("Environment:", ["dev", "staging", "prod"]);   // Escape/Ctrl-C -> null
console.log(picked);
const k = keypress();   // e.g. { name: "up", sequence: "\u001b[A", ctrl: false, meta: false, shift: false }
console.log(k && k.name);
```

### ProgressBar

**`new ProgressBar({ total })`**

- `total` *(number)* — the final count; a finite number greater than 0.

A single-line progress bar.

**`bar.update(n) -> this`**

- `n` *(number)* — the current count, within `0..total`.

Redraws one line in place: a 20-cell bar, the percentage, the count and a time-based ETA (`ETA ?` before the first progress). Reaching `total` prints `100%`, closes the line with a newline and finishes the bar; any later `update` throws `RangeError`.

### Spinner

**`new Spinner({ text? })`**

- `text` *(string, optional)* — the label beside the frame.

A single-line spinner over the frames `|`, `/`, `-`, `\`.

**`spinner.start() -> this`** — draws the first frame.

**`spinner.tick() -> this`** — advances one frame and redraws the line.

**`spinner.stop(text?) -> this`** — clears the line; with a `text` *(string)*, leaves it as the final line instead (with a newline).

The lifecycle is strict: `tick`/`stop` before `start`, a second `start`, and any use after `stop` each throw `RangeError`.

```js
import { ProgressBar, Spinner } from "dyna:cli";
const bar = new ProgressBar({ total: 3 });
bar.update(1); bar.update(2); bar.update(3);   // one redrawn line; the last closes it with a newline
const spin = new Spinner({ text: "working" });
spin.start().tick().tick();                    // "working" beside | -> / -> - on one line
spin.stop("done");                             // leaves "done" as the final line
```

### Table

**`Table(rows, { head?, align?, format? }) -> string`**

- `rows` *(Array<Array<string | number>>)* — the table body; every row must have the same number of cells (a string or number each).
- `head` *(string[], optional)* — a header row, rendered first.
- `align` *(`("left" | "right" | "center")[]`, optional)* — per-column alignment; columns beyond the array are `"left"`.
- `format` *(string, optional)* — `"grid"` (default), `"tsv"` or `"csv"`.

Renders rows as text (every rendered line ends with `\n`, head first):

- `"grid"` — columns size to their content, cells are padded to the column width (the last column carries no trailing padding) with a two-space gutter, and a head is ruled off with dashes.
- `"tsv"` — raw cells joined by tabs. TSV quotes nothing.
- `"csv"` — RFC 4180: quotes only cells containing `"`, `,`, `CR` or `LF`, doubling embedded quotes.

An unknown `format` or alignment throws `RangeError` naming the valid set; a ragged row or non-string/non-number cell throws `TypeError` naming the row and column.

```js
import { Table } from "dyna:cli";
console.log(Table([["a", 1], ["longer", 22]], { head: ["name", "n"], align: ["left", "right"] }));
// name     n
// ------  --
// a        1
// longer  22
console.log(Table([["a,b", 'say "hi"']], { format: "csv" }));   // "a,b","say ""hi"""
console.log(Table([["x", "y"]], { format: "tsv" }));            // x<TAB>y
```

### StyleText

**`StyleText(style, text) -> string`**

- `style` *(string | string[])* — one style name or an array of them, applied in order and closed in reverse.
- `text` *(string)* — the text to wrap.

Wraps text in ANSI escape sequences. Beyond the named styles, colors take dynamic forms: `"256:<0-255>"` / `"bg256:<0-255>"` (256-color), `"#rrggbb"` / `"bg#rrggbb"`, and `"rgb:r,g,b"` / `"bgRgb:r,g,b"` (truecolor, each component 0–255). An index or component past 255 throws `RangeError`; a malformed form or an unknown style is refused with a `TypeError`, never silently dropped.

Output auto-dims: with `$NO_COLOR` set and non-empty, or when stdout is not a TTY, the escapes are dropped and the text is returned unchanged. `$FORCE_COLOR`, when set and non-empty, overrides both (`"0"`/`"false"` refuse; any other value enables). Style names are validated the same way in every environment.

**`Styles()`**

Returns the named styles: `reset`, `bold`, `dim`, `italic`, `underline`, `inverse`, `strikethrough`, `blink`, `hidden`, `doubleunderline`, `framed`, `overlined`, the 8 basic colors, `gray`/`grey`, the `*Bright` families, and the `bg*` backgrounds. The dynamic color forms are not listed here — they are unbounded.

**`IsTTY(fd?) -> boolean`**

- `fd` *(number, optional)* — the fd to test; default 1.

Returns `isatty` for the fd.

**`Columns() -> number`**

Returns the terminal width, from `$COLUMNS` or 80.

**`ColorDepth(fd?) -> number`**

- `fd` *(number, optional)* — the fd to test.

Returns 0 (none), 4 (16 colors), 8 (256), or 24 (truecolor). `$FORCE_COLOR` wins over everything (`"0"`/`"false"` refuse, `"2"` is 256, `"3"` is truecolor, anything else non-empty is 16); otherwise `$NO_COLOR` set and non-empty returns 0, a non-TTY fd returns 0, `$COLORTERM` truecolor/24bit gives 24, a 256color `$TERM` gives 8, `dumb` gives 0, else 4.

```js
import { StyleText, Styles, IsTTY, Columns, ColorDepth } from "dyna:cli";
const on = ColorDepth() > 0;                               // emission follows $FORCE_COLOR/$NO_COLOR/TTY
console.log(StyleText("bold", "hi") === (on ? "\u001b[1mhi\u001b[22m" : "hi"));   // true
console.log(StyleText(["red", "underline"], "x") === (on ? "\u001b[31m\u001b[4mx\u001b[24m\u001b[39m" : "x"));  // true
console.log(StyleText("256:200", "x") === (on ? "\u001b[38;5;200mx\u001b[39m" : "x"));      // the 256-color form
console.log(StyleText("#102030", "x") === (on ? "\u001b[38;2;16;32;48mx\u001b[39m" : "x")); // truecolor hex
console.log(StyleText("rgb:1,2,3", "x") === (on ? "\u001b[38;2;1;2;3mx\u001b[39m" : "x"));  // truecolor triple
console.log(Styles().includes("bold"), Styles().includes("bgBlue"));        // true true
console.log(IsTTY(1) === (typeof IsTTY(1) === "boolean"));       // true
console.log(Columns() > 0, ColorDepth() >= 0);                   // true true
try { StyleText("neon", "x"); } catch (e) { console.log(e.message); }  // StyleText: unknown style "neon"
try { StyleText("256:999", "x"); } catch (e) { console.log(e.message); }  // StyleText: 256 color index out of range (0-255)
```

### remaining exports

- `Command.prototype.name` — a read-only accessor returning the constructor's name argument (default `""`).

# dyna:net

IP addresses and CIDR prefixes, TCP and UDP sockets, DNS, the Redis/PostgreSQL/SQLite clients, a rate limiter, metrics, an L4 proxy — and the shared HTTP surface (fetch, Request, Response, Headers, FormData, AbortController/AbortSignal, HTTPClient, HTTPServer, HTTPServerAsync, App, WsClient, the message codecs) re-exported from `dyna:http`. `dyna:net` and `dyna:http` can be imported in the same process in either order; import the module that owns the classes you need. This section's examples import from `dyna:net` only.

Every options bag in the native clients is strict: an unknown option key throws a `TypeError` naming the key and the valid set (`unknown option "portz" (valid: tls, ca, host, path, username, password, port, db, binary, bigint, maxReplyBytes, maxPending, connectTimeoutMs, commandTimeoutMs)`), instead of being silently ignored. This covers the `Redis`, `PostgreSQL`, `DNSResolver`, `DNSServer` and `TCPProxy` constructor bags, `PostgreSQL.query`'s `{maxRows}`, `TCPServer`/`connect`/`connectHappy` options and their `tls` sub-bags, and the connect/WebSocket handler bags. Parameter objects, upstream lists and command arguments are data, not options — their keys are never checked.

`import { Prefix, parseAddr, parsePrefix, canonical, isValid, compareAddr, contains, masked, isLoopback, isPrivate, isGlobalUnicast, isLinkLocalUnicast, isLinkLocalMulticast, isMulticast, isUnspecified, RateLimiter, Metrics, DNSResolver, DNSServer, Redis, PostgreSQL, SQLite, TCPServer, UDPSocket, TCPProxy, connectHappy, swallowedHandlerThrows, fetch, Request, Response, Headers, FormData, AbortController, AbortSignal, HTTPClient, HTTPServer, HTTPServerAsync, App, WsClient, ContentTypeParse, ContentTypeFormat, CookieParse, CookieSerialize, ETagMatch, Negotiate, NegotiateToken, RangeParse, MultipartParse, MultipartFormat } from "dyna:net";`

Note: `PgPool` and `RedisPool` are NOT exports of this module. They are pure-JS pools over the native `PostgreSQL`/`Redis` clients, shipped as the source file `src/pool.js` — import them by file path (`import { PgPool, RedisPool } from "./src/pool.js"`). `queryIter(sql, params?, opts?)` — the constant-memory DECLARE-CURSOR stream — is a NATIVE method of `PostgreSQL` in this module; importing `pool.js` is not needed for it, and pool.js's installer stands down when the native method is present, so the pools delegate any `queryIter` use to the native driver.

### IP addresses

**`parseAddr(str) -> { is4, is6, bytes, string }`**

- `str` *(string)* — a strict dotted-quad IPv4 or an IPv6, including `::` compression and an embedded IPv4 tail (e.g. `::ffff:1.2.3.4`).

`bytes` is a `Uint8Array` of 4 or 16 bytes and `string` is the RFC 5952 canonical text; `is4`/`is6` say which family parsed. A malformed address throws `TypeError`, and a zone (`fe80::1%eth0`) is refused.

**`parsePrefix(str) -> { addr, bits }`**

- `str` *(string)* — a CIDR string `"addr/bits"`.

`addr` is the canonical address and `bits` the prefix length (0–32 for IPv4, 0–128 for IPv6). A malformed CIDR throws `TypeError`.

**`canonical(str) -> string`**

- `str` *(string)* — an address.

The RFC 5952 canonical text: lowercase hex, the leftmost longest run of zero groups compressed, and a 4-in-6 address formatted as `::ffff:a.b.c.d`. Throws on a malformed address.

**`isValid(str) -> boolean`**

- `str` — any value; a non-string argument is `false`.

True when the string parses as an address. Never throws.

**`compareAddr(a, b) -> -1 | 0 | 1`**

- `a`, `b` *(string)* — two addresses.

The total order over addresses: equal addresses give 0, and IPv4 sorts before IPv6. Throws on a malformed argument.

**`contains(prefix, addr) -> boolean`**

- `prefix` *(string)* — a CIDR prefix.
- `addr` *(string)* — an address.

True when the prefix contains the address. The two must be the same family: an IPv4 address is never inside an IPv6 prefix. Throws on a malformed prefix or address.

**`masked(prefix) -> string`**

- `prefix` *(string)* — a CIDR prefix.

The prefix's network address (host bits zeroed), canonically formatted: `masked("192.168.1.55/24")` is `"192.168.1.0"`.

**`isLoopback(str)`, `isPrivate(str)`, `isMulticast(str)`, `isUnspecified(str)`, `isLinkLocalUnicast(str)`, `isGlobalUnicast(str)`, `isLinkLocalMulticast(str) -> boolean`**

- `str` *(string)* — an address.

One predicate per address class: `isLoopback` is `127.0.0.0/8` and `::1`; `isPrivate` is RFC 1918 space and `fc00::/7`; `isMulticast` is `224.0.0.0/4` and `ff00::/8`; `isUnspecified` is `0.0.0.0` and `::`; `isLinkLocalUnicast` is `169.254.0.0/16` and `fe80::/10`; `isLinkLocalMulticast` is `224.0.0.0/24` and `ff02::/16`; `isGlobalUnicast` is everything else routable. Each throws on a malformed address, and a 4-in-6 address is classified by its mapped IPv4 form (`::ffff:10.0.0.1` is private).

```js
import { parseAddr, parsePrefix, canonical, isValid, compareAddr, contains, masked,
         isLoopback, isPrivate, isGlobalUnicast, isLinkLocalUnicast, isLinkLocalMulticast,
         isMulticast, isUnspecified } from "dyna:net";
const a = parseAddr("192.168.1.1");
console.log(a.is4, a.is6, a.string, a.bytes.length);          // true false 192.168.1.1 4
const v6 = parseAddr("::1");
console.log(v6.is6, v6.string, v6.bytes.length);              // true ::1 16
console.log(JSON.stringify(parsePrefix("10.0.0.0/8")));       // {"addr":"10.0.0.0","bits":8}
console.log(canonical("0:0:0:0:0:0:0:1"));                    // ::1
console.log(canonical("2001:0db8:0000:0000:0000:ff00:0042:8329"));  // 2001:db8::ff00:42:8329
console.log(isValid("1.2.3.4"), isValid("999.1.1.1"), isValid("nope"));  // true false false
console.log(compareAddr("10.0.0.1", "192.168.1.1"), compareAddr("1.1.1.1", "1.1.1.1"));  // -1 0
console.log(contains("10.0.0.0/8", "10.1.2.3"), contains("10.0.0.0/8", "11.1.2.3"));      // true false
console.log(masked("192.168.1.55/24"));                       // 192.168.1.0
console.log(isLoopback("127.0.0.1"), isPrivate("10.0.0.1"), isGlobalUnicast("8.8.8.8"),
            isLinkLocalUnicast("169.254.5.5"), isMulticast("224.0.0.1"),
            isLinkLocalMulticast("ff02::1"), isUnspecified("0.0.0.0"));
// true true true true true true true
```

### Prefix

**`new Prefix(cidr)`**

- `cidr` *(string)* — a CIDR prefix.

The string is parsed and masked once at construction, so testing an address is a masked compare rather than a re-parse — the same shape as compiling a regexp once and matching many strings. A malformed CIDR throws `TypeError`.

**`prefix.contains(addr) -> boolean`**

- `addr` *(string)* — an address.

True when the address is inside the prefix; an unparseable address is `false`, matching the free `contains()`'s "not in this prefix" answer.

**`prefix.overlaps(other) -> boolean`**

- `other` *(Prefix)* — another prefix.

True when the two prefixes share any address; different families never overlap.

**`prefix.masked`**

The network address, canonically formatted.

**`prefix.bits`**

The prefix length.

**`prefix.isIPv4`**

True for an IPv4 prefix.

```js
import { Prefix } from "dyna:net";
const p = new Prefix("10.0.0.0/8");
console.log(p.contains("10.1.2.3"), p.contains("11.0.0.1"), p.contains("not-an-ip"));
// true false false
console.log(p.bits, p.masked, p.isIPv4);                      // 8 10.0.0.0 true
const p6 = new Prefix("2001:db8::/32");
console.log(p6.contains("2001:db8::1"), p6.isIPv4);            // true false
console.log(new Prefix("10.0.0.0/8").overlaps(new Prefix("10.5.0.0/16")));   // true
console.log(new Prefix("10.0.0.0/8").overlaps(new Prefix("192.168.0.0/16"))); // false
try { new Prefix("nonsense"); } catch (e) { console.log(e.message); }
// dyna:net: invalid CIDR prefix
```

### RateLimiter

**`new RateLimiter({ tokensPerSec, burst?, slots?, refill?, capacity?, grow? })`**

- `tokensPerSec` *(number)* — required, 1 to 1e9. `refill` is an alias; giving the pair with different values throws.
- `burst` *(number)* — default `tokensPerSec` (one second of traffic), 1 to 1e9. `capacity` is an alias, with the same rule.
- `slots` *(number)* — default 1024, rounded up to a power of two within 8 to 2^20.
- `grow` *(boolean, default `false`)* — opt in to table growth.

Options bags are strict: an unknown key in the constructor's options throws a `TypeError` naming the key and the valid set (`unknown option "toksPerSec" (valid: tokensPerSec, refill, burst, capacity, grow, slots)`) — a typo'd rate option is refused, never read as a wrong rate.

A token bucket over a direct-mapped table. The table is FIXED by default — that is the security property, not a limitation: a limiter that allocates a slot per key lets a forged-key attacker turn the defence into the memory exhaustion it exists to prevent. Two keys can hash to one slot and share a budget; that is the price of a bound an attacker cannot move. With `grow: true`, keys that are honest can ask the table to double itself past a 3/4 load factor — up to the hard ceiling of 2^20 slots, which an attacker still cannot move; past the ceiling, colliding keys share slots exactly as before, and every doubling is counted in `stats.grew`. Refill is exact integer arithmetic (milli-tokens), so the bucket never drifts.

**`limiter.allow(key, cost?) -> boolean`**

- `key` — the key to check.
- `cost` *(number)* — tokens to consume, default 1; must be positive.

True when the key may proceed, consuming `cost` tokens. A cost above the burst is always denied, before any arithmetic.

**`limiter.tokens(key) -> number`**

- `key` — the key.

The key's current token count.

**`limiter.reset(key?)`**

- `key` — one key to clear; with no key the whole table is cleared.

**`limiter.stats`**

`{ allowed, denied, slots, live, grew, tokensPerSec, burst }` — `live` is the live-slot count, `grew` the number of table doublings (`0` while the limiter is fixed).

```js
import { RateLimiter } from "dyna:net";
const rl = new RateLimiter({ tokensPerSec: 5, burst: 3 });
console.log(rl.allow("a"), rl.allow("a"), rl.allow("a"), rl.allow("a"));
// true true true false
console.log(rl.tokens("a"));                                  // 0
console.log(JSON.stringify(rl.stats));
// {"allowed":3,"denied":1,"slots":1024,"live":1,"tokensPerSec":5,"burst":3}
rl.reset("a");
console.log(rl.allow("a"));                                   // true
try { new RateLimiter({}); } catch (e) { console.log(e.message); }
// RateLimiter: tokensPerSec is required
```

### Metrics

Counters, gauges and histograms with a Prometheus text-format scrape. The registry is fixed at 256 series, so a metric name that comes from a request cannot let a peer allocate server memory by varying a label. A counter, gauge and histogram with the same name and label set is refused (a type clash); a full registry refuses new names with a `RangeError`. A name must match `[a-zA-Z_:][a-zA-Z0-9_:]*` up to 55 bytes, or it is refused; label values are escaped in the exposition. The registry is process-global, so `reset()` empties it for tests.

**`Metrics.counter(name, value?, labels?)`**

- `name` *(string)* — the metric name.
- `value` *(number)* — the increment, default 1; a negative or `NaN` increment is refused.
- `labels` *(object)* — label values, escaped in the exposition.

Increments a counter.

**`Metrics.gauge(name, value, labels?)`**

- `name` *(string)* — the metric name.
- `value` *(number)* — the value to set.
- `labels` *(object)* — label values.

Sets a gauge.

**`Metrics.histogram(name, value, labels?, opts?)`**

- `name` *(string)* — the metric name.
- `value` *(number)* — the observation to record.
- `labels` *(object)* — label values.
- `opts.buckets` *(number[])* — per-series bucket edges in seconds, fixed at registration: 1 to 6 finite, positive, strictly increasing numbers.

Options bags are strict: an unknown key in `opts` throws a `TypeError` naming the key and the valid set (`unknown option "bucketz" (valid: buckets)`). The check runs before registration, so a refused bag never half-creates the series.

Records an observation into the 5 ms / 10 ms / 50 ms / 100 ms / 500 ms / 1 s buckets — or into the edges the series was registered with. A series registered with custom edges never gains the default ones (and vice versa: edges are read once, when the series first exists; later observations ignore them). The count cap keeps the per-series hot state at one cache line, the same bound the default edges live under.

**`Metrics.scrape() -> string`**

The registry as Prometheus text (cumulative buckets, `_sum` and `_count`).

**`Metrics.reset()`**

Empties the registry; intended for tests.

```js
import { Metrics } from "dyna:net";
Metrics.reset();
Metrics.counter("http_requests_total", 2, { method: "GET" });
Metrics.counter("http_requests_total");
Metrics.gauge("queue_depth", 3);
const lines = Metrics.scrape().split("\n")
  .filter(l => l.startsWith("http_requests_total") || l.startsWith("queue_depth"));
console.log(lines.join("|"));
// http_requests_total{method="GET"} 2|http_requests_total 1|queue_depth 3
```

### DNSResolver

**`new DNSResolver({ server?, port?, timeoutMs?, ttl? })`**

- `server` *(string)* — default `"127.0.0.1"`; must be an IPv4 address — there is no DNS bootstrap, so hostnames are refused.
- `port` *(number)* — default 53; a value outside 1–65535 is a `RangeError`.
- `timeoutMs` *(number)* — default 5000; a value below 1 is a `RangeError`.
- `ttl` *(number, optional)* — the answer-cache CAP in seconds; a value above 86400 clamps to 86400. **Absent or 0 disables caching** (the default). When set, a successful non-empty answer is cached under the key `lowercase(name)|type` (DNS names are case-insensitive, RFC 1035 2.3.3, so `Example.test` and `example.test` share an entry) and served for `min(the answer's smallest RR TTL, ttl)` seconds. A TTL-0 answer is never cached, and neither is an empty one — a transient empty reply cannot pin "no records". Cache hits are delivered asynchronously (on a job), so a cached answer is indistinguishable in ordering from a wire answer. There is no single-flight de-duplication: N concurrent lookups of the same uncached name put N queries on the wire (the answer that lands last populates the cache), and each hit hands out a FRESH deep copy — mutating an answer you were handed never poisons another caller's.

A UDP DNS client (RFC 1035) that runs on the JS thread. Construction binds a UDP socket.

Every answer is matched against the outstanding query three ways, and dropping any one of them is the Kaminsky attack: the source address and port (the socket is `connect()`ed, so the kernel drops datagrams from anyone else), the 16-bit query ID (drawn from the OS CSPRNG, not a counter), and the question name and type echoed back. The source port is randomized. A response with the TC bit set is retried once over TCP (RFC 1035 4.2.2), which has no size limit.

**`resolver.query(name, type, callback?)`**

- `name` *(string)* — the name to query; a name longer than 255 bytes is a `RangeError`.
- `type` *(number)* — the query type; A (1) and AAAA (28) are the useful ones.
- `callback` *(function, optional)* — receives `(err, records)`: `err` is a string (timeout, rcode, TCP fallback failure) or `null`; `records` is an array of answer records, or `undefined` on error. Without a callback the result is discarded.

Issues a query. At most 64 queries can be in flight at once; further calls throw. The records array keeps only answers whose owner name matches the question AND whose record type equals the query type (case-insensitive owner match, RFC 1035 2.3.3) — unrelated records a response legally carries, including a same-owner CNAME riding on an A answer (the CNAME-chain shape), never surface in it. A record is `{ name, type, ttl }` plus the payload for its type:

- A / AAAA — `address` (the printable address)
- CNAME / NS / PTR — `target` (the decoded name; RDATA compression pointers are chased)
- MX — `priority` and `exchange`
- TXT — `chunks` (the RFC 1035 character-strings, kept separate; joining them is the caller's call)

```js
import { DNSResolver } from "dyna:net";
try { new DNSResolver({ server: "example" }); } catch (e) { console.log(e.message); }
try { new DNSResolver({ port: 0 }); } catch (e) { console.log(e.message); }
// DNSResolver: server must be an IPv4 address
// DNSResolver: bad port or timeoutMs
```

**`resolver.lookup(name, type?, callback?)` — promise form**

Overloaded by argument shape: without a trailing function it returns a `Promise<DNSRecord[]>` that resolves with the decoded answers or rejects with an `Error` naming the failure (timeout, rcode, …); with a trailing function it is the callback form, delivering `(err, records)` exactly as `query()` does, and returns `undefined`. `type` defaults to A (1).

```js
import { DNSResolver, DNSServer } from "dyna:net";
// A loopback authority so the example runs anywhere.
const dns = new DNSServer({ port: 0, host: "127.0.0.1" });
dns.start((name, type) => name === "host.test" && type === 1 ? "192.0.2.7" : null);
const res = new DNSResolver({ server: "127.0.0.1", port: dns.port, timeoutMs: 3000 });
const recs = await res.lookup("host.test");
console.log(recs[0].address);             // 192.0.2.7
// the callback form still works, both shapes:
res.lookup("host.test", (err, r) => console.log(r.length));
res.lookup("host.test", 28, (err, r) => console.log(r.length));
res.close();
dns.close();
```

**`resolver.resolveCname(name)`, `resolver.resolveNs(name)`** — `Promise<string[]>` of the targets/nameservers.

**`resolver.resolveMx(name)`** — `Promise<{ priority: number, exchange: string }[]>`.

**`resolver.resolveTxt(name)`** — `Promise<string[][]>` — one array of character-string chunks per TXT record.

Each is sugar over `lookup(name, TYPE)` with a shaped result; the answer, its anti-spoofing checks, and the `{ttl}` cache are the shared ones. A synchronous validation failure (bad type, closed resolver) arrives as a REJECTION, never as a throw or a never-settling promise.

<!-- check:skip (needs a live authoritative resolver for MX/TXT) -->
```js
import { DNSResolver } from "dyna:net";
const res = new DNSResolver({ server: "127.0.0.1", port: 53, ttl: 300 });
const mx = await res.resolveMx("gmail.com");
console.log(mx[0].priority, mx[0].exchange);
const txt = await res.resolveTxt("google.com");
console.log(txt[0][0].slice(0, 20));        // first chunk of the first record
res.close();
```

### DNSServer

**`new DNSServer({ port?, host? })`**

- `port` *(number)* — 0 binds an ephemeral port that resolves into `.port`.
- `host` *(string)* — the bind address, default `127.0.0.1`.

A UDP DNS server. It binds in the constructor, so construction fails if the port is taken.

**`server.start(handler)`**

- `handler` *(function)* — `handler(name, type)` returns an address string to answer with, or `null` for no answer.

Starts answering. The answer's TTL is fixed at 60 seconds.

A UDP DNS server is a reflection amplifier by construction — the source address is unverified — so two defences are built in and neither is optional: a response is never sent if it would be more than 4× the query that provoked it (over the cap it sets TC and sends the header alone, which is the RFC's own "retry over TCP" signal), and a per-source token bucket (20 queries/sec) means one forged source cannot flood a victim. The handler runs on the JS thread with the query's name and type; a returning string that does not parse as an address of the requested type is simply not added.

### Redis

**`new Redis({ host?, port?, path?, db?, username?, password?, tls?, ca?, binary?, bigint?, maxReplyBytes?, maxPending?, connectTimeoutMs?, commandTimeoutMs? })`**

- `host` *(string)* — default `"127.0.0.1"`.
- `port` *(number)* — default 6379 (1–65535; a `path` selects an AF_UNIX socket and then `port` may be 0).
- `path` *(string)* — an AF_UNIX socket path.
- `db` *(number)* — default 0 (0–255).
- `username`, `password` *(string)* — used for `AUTH`.
- `tls` *(boolean)* — connect through TLS; the handshake runs on the aio engine (the same machinery as `wss://` and dyna:http).
- `ca` *(string)* — CA certificate PEM file *path* pinned for the TLS handshake; omitted, the system trust store is used.
- `binary` *(boolean)* — default false; with it, bulk replies come back as `Uint8Array`.
- `bigint` *(boolean)* — default false; with it, 64-bit integers and RESP3 big numbers come back as `BigInt`.
- `maxReplyBytes` *(number)* — default 64 MiB.
- `maxPending` *(number)* — default 4096.
- `connectTimeoutMs` *(number)* — default 10000.
- `commandTimeoutMs` *(number)* — default 0 (off).

A Redis client running on the JS thread. With `tls: true` the protocol bytes (HELLO, AUTH, commands) flow encrypted; `ca` pins the trust anchor rather than downgrading verification. A plaintext peer is still reported as such.

The handshake runs `HELLO 3` (with `AUTH` when a password is given, `default` as the username when only a password is configured); a server that refuses HELLO 3 downgrades to RESP2. Commands issued before the handshake settles wait in a queue so nothing overtakes AUTH or SELECT.

**`redis.command(command, ...args) -> Promise`**

- `command` *(string)* — the command name; omitted, it is a `TypeError`.
- `args` *(string | number | Uint8Array | ArrayBuffer)* — the command's arguments; bytes are sent raw.

Sends one command and resolves with its reply. With `binary`, bulk replies come back as `Uint8Array`; with `bigint`, 64-bit integers and RESP3 big numbers come back as `BigInt` (otherwise 64-bit integers past 2^53 stay text — the exact digits, never a rounded number).

**`redis.pipeline(commands) -> Promise<unknown[]>`**

- `commands` *(array)* — an array of command arrays.

One round trip for an array of command arrays; resolves to one reply per command.

**`redis.on("push" | "message" | "error", handler) -> this`**

- `event` *(string)* — `"push"` or `"message"` receives each pushed delivery (RESP3 push frames, and on RESP2 the subscribe notifications); `"error"` receives a connection-level error.
- `handler` *(function)* — the listener.

Registers a listener.

**`redis.protocol`**

The negotiated RESP protocol, 2 or 3.

**`redis.ready`**

True once the handshake settled.

**`redis.pending`**

Commands issued but not yet answered.

Replies arrive in the order commands were sent, and the pending list is a strict FIFO: a malformed reply tears the connection down and rejects everything on it, because guessing which reply belongs to which command is how a client silently returns one key's value for another. Subscribing is tracked by channel count so a delivery can be told from a reply on RESP2; a (P|S)SUBSCRIBE answers once per channel, and a bare `UNSUBSCRIBE` is refused because the reply count depends on server state. While subscribed on RESP2, only PING, QUIT, RESET and the (un)subscribe commands are allowed.

```js
import { Redis } from "dyna:net";
try { new Redis({ tls: true }); } catch (e) { console.log(e.message); }
try { new Redis({ db: 999 }); } catch (e) { console.log(e.message); }
// Redis: TLS is not supported; use a plaintext endpoint or terminate TLS in front of it
// Redis: db must be 0..255
```

### PostgreSQL

**`new PostgreSQL({ host?, port?, path?, user?, password?, database?, applicationName?, tls?, ca?, raw?, bytes?, textResults?, statementCacheSize?, prepareAfter?, bigint?, insecureAuth?, maxMessageBytes?, maxPending?, queryTimeoutMs?, connectTimeoutMs? })`**

- `host` *(string)* — default `"127.0.0.1"`.
- `port` *(number)* — default 5432 (1–65535; a `path` selects an AF_UNIX socket).
- `path` *(string)* — an AF_UNIX socket path.
- `user`, `password`, `database`, `applicationName` *(string)* — connection identity.
- `tls` *(boolean)* — connect through TLS; the startup handshake (including the SCRAM exchange) is encrypted on the aio engine.
- `ca` *(string)* — CA certificate PEM file *path* pinned for the TLS handshake; omitted, the system trust store is used.
- `raw`, `bytes`, `textResults` *(boolean)* — binary results are the default; `textResults: true` forces text decoding.
- `statementCacheSize` *(number)* — default 64; 0 disables the cache entirely, which a connection pooler in transaction mode requires.
- `prepareAfter` *(number)* — default 2.
- `bigint` *(boolean)* — 64-bit integers come back as `BigInt`.
- `insecureAuth` *(boolean)* — permits cleartext password auth when the server offers no SCRAM.
- `maxMessageBytes` *(number)* — default 64 MiB.
- `maxPending` *(number)* — default 1024.
- `connectTimeoutMs` *(number)* — default 10000.
- `queryTimeoutMs` *(number)* — default 0 (off).

A PostgreSQL client (wire protocol 3.0) running on the JS thread. With `tls: true` the session is encrypted before the startup message, so the SCRAM-protected credential never crosses the wire in the clear either.

**`pg.query(sql, params?) -> Promise`**

- `sql` *(string)* — the statement.
- `params` *(array)* — bound parameters; with a params array the extended protocol is used, where every value is bound and never interpolated.

Runs one statement. Without a params array it uses the simple protocol (several semicolon-separated statements allowed). At most 65535 parameters; a statement taking N parameters must be given exactly N. A parameter that is an object is refused (pass a `Uint8Array`/`ArrayBuffer` for `bytea`, `JSON.stringify(v)` for JSON, or an ISO string for a timestamp), because a silently stringified object stores cleanly and wrongly.

The promise resolves to `{ rows, fields, command, rowCount }`: `rows` is one object per row (a column named `__proto__` is a key like any other), `fields` describes each result column, `command` is the server's completion tag (`"SELECT 1"`, `"INSERT 0 3"`), and `rowCount` is the number of rows returned, or — when the statement returns none — the affected-row count from the tag. An `ErrorResponse` rejects with `code` (the SQLSTATE), `severity`, `message` and, where the server sent them, `detail`, `hint`, `position`, `schema`, `table`, `column` and `constraint`.

**`pg.pipeline(statements) -> Promise<Array>`**

- `statements` *(array)* — an array of `[sql]` or `[sql, params]`; at least one, at most 65535.

One round trip for the whole batch: every statement's Parse/Bind/Describe/Execute goes out together with a single Sync at the tail. Resolves to one result per statement, in order; a statement that fails leaves an `Error` in its slot, and the statements after it are each rejected with an error naming them as skipped — the server answers nothing past an `ErrorResponse` until the Sync. The batch counts each member against `maxPending`.

**`pg.queryIter(sql, params?, opts?) -> AsyncIterableIterator<row>`** — native

- `sql` *(string)* — one statement; it is wrapped in `DECLARE <name> CURSOR FOR <sql>`.
- `params` *(array, optional)* — bound parameters, exactly as `query` takes them.
- `opts.batch` *(number)* — rows per `FETCH FORWARD`; default **1000**, clamped to ≥ 1.
- `opts.maxRows` *(number)* — the safety cap on TOTAL rows; default 0 = unbounded. Past the cap the stream stops, the buffered rows are dropped, and the cursor is closed — the same as a `break`.

Streams a huge result with memory held at O(batch): the driver runs `BEGIN`, `DECLARE djqN CURSOR FOR <sql>`, then `FETCH FORWARD <batch>` until a fetch comes back empty, then `CLOSE` and `COMMIT`. The construction is LAZY (async-generator semantics): no IO happens until the first `next()`, and a stream built on a closed connection rejects there rather than at the call. The result is an object with `next()`, `return()` and `[Symbol.asyncIterator]` (there is no C-level async generator in the engine), so `for await` and manual iteration both work; a `break` calls `return()`, whose cleanup is UNCONDITIONAL — buffered rows are dropped, the cursor is closed, and the transaction is committed (rolled back after a failure), so an abandoned stream can never leave the connection inside a transaction. A stream that hits an error rejects with the statement error and runs the same cleanup with `ROLLBACK`; a `next()` after that rejects with the stored error. Concurrent `next()` calls queue and resolve in order, one row per call. The stream drives its own `BEGIN`/`COMMIT`: run it OUTSIDE a transaction you began yourself, or the cleanup's COMMIT/ROLLBACK ends YOUR transaction too (exactly what pool.js's generator did).

<!-- check:skip (needs a live PostgreSQL) -->
```js
import { PostgreSQL } from "dyna:net";
const pg = new PostgreSQL({ host: "127.0.0.1", user: "postgres", database: "postgres" });
let sum = 0;
for await (const row of pg.queryIter("SELECT generate_series(1, 1000000) AS n",
                                     undefined, { batch: 1000 })) {
  sum += row.n;                       // memory stays O(batch) throughout
}
console.log(sum);
await pg.queryIter("SELECT generate_series(1, 10) AS n").return(); // cleanup is explicit too
pg.close();
```

**`pg.cancel()`**

Asks the server to cancel the running query. It goes on a fresh connection (the busy one is not reading it), and the server sends no reply by design.

**`pg.on("notice" | "notification" | "error", handler) -> this`**

- `event` *(string)* — `"notice"` for notices, `"notification"` for `LISTEN` notifications, `"error"` for connection-level errors.
- `handler` *(function)* — the listener.

Registers a listener.

**`pg.ready`**

True once the startup handshake completed.

**`pg.backendPid`**

The server's backend PID.

**`pg.transactionStatus`**

One character: `I` idle, `T` in transaction, `E` failed.

**`pg.parameters`**

The server parameters from startup (`server_version`, `client_encoding`, ...).

**`pg.pending`**

Queries issued but not yet answered.

**`pg.statementCache`**

`{ size, max, prepareAfter, preparedHits, unnamed }`: how full the prepared-statement cache is and which arm each query took, so the selection is observable. A statement is promoted to the named, pre-prepared path after `prepareAfter` sightings; the server keeps the parse and plan.

```js
import { PostgreSQL, SQLite } from "dyna:net";
try { new PostgreSQL({ tls: true }); } catch (e) { console.log(e.message); }
// PostgreSQL: TLS is not supported; SCRAM-SHA-256 protects the credential but not the session,
//              so terminate TLS in front of the server or use a trusted network
try { new SQLite(); } catch (e) { console.log(e.message); }
// new SQLite(path, options?)
```

### SQLite

**`new SQLite(path, { readonly?, bigint? })`**

- `path` *(string)* — the database file.
- `readonly` *(boolean)* — opens without `SQLITE_OPEN_CREATE` when true.
- `bigint` *(boolean)* — 64-bit integers come back as `BigInt`; otherwise integers past 2^53 stay text (exact digits), because a rounded number is worse than either.

A SQLite database handle. Not a network client at all — SQLite is a disk library with no socket — but it lives in this module alongside the others. Opens with `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE` unless `readonly` is true. Parameters are bound, never interpolated, so a string containing a quote is a string and not syntax; there is deliberately no "build the SQL for me" helper. The statement text itself is raw SQL by design: concatenating user input into it is classic SQL injection, so user data reaches a statement only as a bound parameter.

**`db.query(sql, params?) -> object[]`**

Runs exactly one statement and returns one plain object per row; text carrying a second statement (beyond trailing `;` and whitespace) is refused rather than answered with rows whose shape changes partway through. A statement taking N parameters must be given exactly N — an unbound parameter turns a WHERE into a no-op, so a mismatch throws. BLOB columns come back as `Uint8Array`; integers past 2^53 stay text unless `bigint` is set; every other object parameter is refused rather than stringified.

**`db.exec(sql, params?) -> number`**

Like `query`, but runs EVERY semicolon-separated statement and returns the total changed rows; only the first may take parameters, and a later statement declaring any is refused. Repeated single-statement text reuses a prepared-statement cache (reset between uses), so parse+plan are paid once per distinct statement, not once per call.

On a `readonly` handle SQLite's authorizer denies `ATTACH`, which would otherwise open a second database file writable and write through it.

**`db.lastInsertRowId`**

The rowid of the most recent successful INSERT.

**`db.version`**

The linked library's version, read at runtime (macOS ships a different SQLite than Homebrew).

A BLOB binds from a `Uint8Array`/`ArrayBuffer` and comes back as a `Uint8Array`; a boolean binds as an integer; an integral double binds as an integer so a round trip through an `INTEGER` column keeps its type.

<!-- check:skip -->
```js
import { DNSResolver, DNSServer, Redis, PostgreSQL, SQLite } from "dyna:net";
// Live servers and a database; runs only where they are reachable.
const r = new DNSResolver({ server: "8.8.8.8", timeoutMs: 3000 });
r.query("example.com", 1, (err, records) => console.log(err, records));
const dns = new DNSServer({ port: 0, host: "127.0.0.1" });
dns.start((name, type) => name === "test.local" ? "127.0.0.1" : null);
const red = new Redis({ host: "127.0.0.1", port: 6379 });
red.command("SET", "k", "v")
   .then(() => red.command("GET", "k"))
   .then(v => console.log("redis:", v));
const pg = new PostgreSQL({ host: "127.0.0.1", user: "postgres", database: "postgres" });
pg.query("SELECT 1").then(rows => console.log("pg:", rows));
const db = new SQLite("/tmp/dynajs_example.db");
db.exec("CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, name TEXT)");
db.exec("INSERT INTO t (name) VALUES (?)", ["dyna"]);
console.log("sqlite:", db.query("SELECT * FROM t"), db.lastInsertRowId, db.version);
db.close();
```

### TCPServer

**`new TCPServer({ port?, path?, maxConnections?, idleTimeoutMs?, tls?, highWaterMark? })`**

- `port` *(number)* — 0–65535; a port of 0 binds an ephemeral port that resolves into `.port`.
- `path` *(string)* — selects an AF_UNIX endpoint instead of a port. A path containing a NUL byte is refused, because `bind()` would silently use only the part before it.
- `maxConnections` *(number)* — 0–1000000 (0 unbounded, the default).
- `idleTimeoutMs` *(number)* — the idle sweep (0 off); "idle" means the data handler was invoked and returned, never mere byte arrival, so a slowloris that delivers bytes forever without completing anything is still closed.
- `tls` *(object)* — server TLS: `tls: { cert, key, alpn?, ca?, requestCert? }`, all PEM paths. A server-side TLS setup with no `cert`/`key` is refused — there is no self-signed default. `requestCert: true` is mTLS: the client MUST present a certificate chain that verifies against `ca` (PEM path); `requestCert` without `ca` is refused at startup, because a request no certificate could satisfy is worse than no request.
- `highWaterMark` *(number)* — the outbound backpressure cap per connection in bytes, 1..2^30; default **4194304 (4 MiB)**, matching the HTTP surface's outbound bound. See `conn.write` and `conn.bufferedAmount`.

A TCP server (or, via the static `connect`, a TCP client). Both run entirely on the JS thread through the shared io reactor; a handler sees a copy of the received bytes.

**`server.start({ connect?, data?, drain?, close? })`**

- `connect(conn, err)` — fires when a connection lands; on the client side, on failure `conn` is `null` and `err` names the reason.
- `data(conn, bytes)` — delivers a `Uint8Array` copy of each received chunk.
- `drain(conn)`fires exactly ONCE per emptying of the connection's outbound queue, after a write left bytes queued and that queue has since reached zero. The producer's signal to resume after a refusal. A close is not an emptying: a connection closed with bytes queued never fires `drain`.
- `close(conn)` — fires on teardown.

Binds and accepts; idempotent.

**`conn.write(data)`**

- `data` *(string | bytes)* — the payload.

Writes a string or bytes to the peer. On a TLS connection, a write before the handshake completed is refused — write from the `connect` handler, which fires exactly when writing becomes legal.

**Backpressure:** `dyn_aio_send` copies whatever the kernel will not take, and a fast producer writing to a peer that reads slowly used to queue without bound. Now: when a write would push the connection's queued bytes past its `highWaterMark`, the write is REFUSED with a `TypeError` naming the queued bytes, the refused size and the cap — never silently dropped, never queued unbounded. The check runs before any byte moves, so a refused write leaves `bufferedAmount` untouched. When the queue later empties, the `drain` handler fires exactly once; writing is legal the whole time (small writes that fit under the cap are never refused). A single write LARGER than the cap is refused even on an empty queue — chunk it; the cap bounds what the process is willing to hold for a peer that will not read, and one giant write would defeat it. On a TLS connection the same cap bounds the queued ciphertext.

**`conn.bufferedAmount`** *(number, readonly)*

Bytes queued for this connection that the kernel has not yet taken — the active send remainder plus the write queue. Grows as a fast producer outruns a slow reader, `0` on a closed connection. Together with `drain` it is the backpressure protocol: write until refused (or until `bufferedAmount` is high), stop, resume on `drain`.

```js
import { TCPServer } from "dyna:net";
const srv = new TCPServer({ port: 0, highWaterMark: 256 * 1024 });
srv.start({
  connect: (conn) => {
    try {
      for (let i = 0; i < 1000; i++) conn.write(new Uint8Array(64 * 1024));
    } catch (e) {
      console.log(e.message);   // TCPConn: write refused: 262144 bytes queued + ...
    }
  },
  drain: (conn) => conn.write("resume"),   // the queue emptied: go again
  data: (conn, bytes) => conn.write(bytes),
});
srv.close();
```

**`conn.close()`**

Closes the connection.

**`TCPServer.connect({ host?, port?, path?, connectTimeoutMs?, maxConnections?, idleTimeoutMs?, tls?, highWaterMark? }, { connect?, data?, drain?, close? }) -> TCPServer`**

- `host` *(string)* — the server host.
- `port` *(number)* — 1–65535.
- `path` *(string)* — an AF_UNIX endpoint.
- `connectTimeoutMs` *(number)* — an expiry reports `connect timed out` through the same `connect` handler.
- `maxConnections`, `idleTimeoutMs` *(number)* — as for the server constructor.
- `tls` *(boolean | object)* — client TLS: `true` or `{ ca?, servername?, alpn?, minVersion?, rejectUnauthorized?, cert?, key? }`; the name verified against the certificate defaults to the host asked for, and insecure verification is an explicit opt-out, never a default. `cert` and `key` (PEM paths, always together) are what the client PRESENTS when the peer requests a client certificate (mTLS); offered to a peer that does not ask, they are simply unused. Repeated connections to the same host reuse the TLS session automatically, so subsequent handshakes are resumptions and cost noticeably less than the first.
- `connect(conn, err)`, `data(conn, bytes)`, `close(conn)` — the same handlers as `server.start`.

The client entry point.

```js
import { TCPServer, UDPSocket, DNSServer, TCPProxy } from "dyna:net";
try { new TCPServer({ port: 70000 }); } catch (e) { console.log(e.message); }
try { new TCPServer({ path: "/tmp/x\u0000evil.sock" }); } catch (e) { console.log(e.message); }
try { new UDPSocket({ port: 99999 }); } catch (e) { console.log(e.message); }
try { new DNSServer({ port: -5 }); } catch (e) { console.log(e.message); }
try { new TCPProxy({ port: 9000 }); } catch (e) { console.log(e.message); }
// TCPServer: port must be 0..65535
// path contains a NUL byte: bind() would use only the part before it
// UDPSocket: port must be 0..65535
// DNSServer: port must be 0..65535
// TCPProxy needs an upstream
```

<!-- check:skip -->
```js
import { TCPServer, connectHappy, UDPSocket } from "dyna:net";
// A live echo server and its clients; runs only where sockets are allowed.
const srv = new TCPServer({ port: 0, maxConnections: 128, idleTimeoutMs: 30000 });
srv.start({
  connect: (conn) => console.log("peer connected"),
  data: (conn, bytes) => conn.write(bytes),     // echo
  close: (conn) => console.log("peer left")
});
console.log("echo server on", srv.port);
const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port },
  { connect: (conn, err) => { if (conn) conn.write("hi"); },
    data: (conn, bytes) => { console.log("echoed:", bytes.length); conn.close(); } });
connectHappy("127.0.0.1", srv.port, { fallbackMs: 250 },
  { connect: (conn, err) => { if (conn) console.log("happy eyeballs won"); } });
const u = new UDPSocket({ port: 0, host: "127.0.0.1" });
u.start({ message: (data, from) => console.log("datagram from", from.address, from.port) });
console.log("sent", u.send(new Uint8Array([1, 2, 3]), "127.0.0.1", u.port), "bytes");
srv.close();
```

### UDPSocket

**`new UDPSocket({ port?, host? })`**

- `port` *(number)* — 0 binds an ephemeral port that resolves into `.port`.
- `host` *(string)* — default all interfaces.

A bound UDP socket. It binds in the constructor, so construction fails if the port is taken.

**`socket.start({ message? })`**

- `message(data, from)` — receives a `Uint8Array` copy of each datagram, with `from` as `{ address, port }`.

Arms the receive path.

**`socket.send(data, host, port) -> number`**

- `data` *(Uint8Array)* — the datagram.
- `host` *(string)* — must be an IPv4 address — there is no DNS lookup.
- `port` *(number)* — 1–65535.

Sends a datagram and returns the bytes sent.

**`socket.port`**

The bound port.

Datagrams are message-boundaries, not streams: a zero-length datagram is delivered as an empty `Uint8Array`, not as an end of input.

### TCPProxy

**`new TCPProxy({ port, upstream, maxConns?, idleTimeoutMs?, connectTimeoutMs? })`**

- `port` *(number)* — the local port; a port of 0 resolves into `.port`.
- `upstream` *(object | array)* — one `{ host?, port }` (host defaults to `127.0.0.1`) or an array taken round-robin; at least one upstream is required, and an upstream port must be 1–65535.
- `maxConns` *(number)* — caps live pairs (0 unbounded).
- `idleTimeoutMs`, `connectTimeoutMs` *(number)* — run on the shared drain hook so a quiet proxy still sweeps.

An L4 (byte) reverse proxy. It never parses the payload: accept, connect upstream, forward bytes both ways. No JS runs on the data path — a forwarded byte goes recv to send inside the reactor callback, so a proxied connection costs no JS calls at all. Forwarding is back-pressured with high/low watermarks: when one direction's queue crosses 256 KiB the source is paused, and reading resumes when it drains below 64 KiB.

**`proxy.start()`**

Binds and starts forwarding; a port of 0 resolves into `.port`.

**`proxy.stats() -> { live, accepted, refused, idleClosed, connectFailed, bytesUp, bytesDown }`**

Live connection statistics.

**`proxy.port`**

The bound port.

A FIN on one side closes only that direction, so a client that shuts down its write half does not truncate the reply still in flight.

### connectHappy

**`connectHappy(host, port, { fallbackMs? }, { connect?, data?, close? }) -> TCPServer`**

- `host` *(string)* — the host to connect to.
- `port` *(number)* — 1–65535.
- `fallbackMs` *(number)* — default 250, at least 1; the deadline for the whole race.
- `connect(conn, err)`, `data(conn, bytes)`, `close(conn)` — the same client handlers as `TCPServer.connect`.

RFC 6555 Happy Eyeballs as a parallel race: both address families are resolved and connect at once, the first success wins and the loser is closed. DNS failure and "no usable addresses" throw synchronously; a deadline expiry reports `connectHappy: timed out` through the `connect` handler. The returned resource is the same client resource `TCPServer.connect` returns, and the first winner's `connect(conn, err)` fires once the race lands. Requires a reachable host, so it is shown without a runnable example.

### The shared HTTP surface

`dyna:net` re-exports the entire `dyna:http` module: `fetch`, `Request`, `Response`, `Headers`, `FormData`, `AbortController`/`AbortSignal`, `HTTPClient`, `HTTPServer`, `HTTPServerAsync`, `App`, `WsClient`, and the message codecs (`ContentTypeParse`, `ContentTypeFormat`, `CookieParse`, `CookieSerialize`, `ETagMatch`, `Negotiate`, `NegotiateToken`, `RangeParse`, `MultipartParse`, `MultipartFormat`). Each is the same class or function as its `dyna:http` name, re-exported here; the full treatment — constructors, options, defaults, bounds and refusals — lives in the `dyna:http` section of this reference. `fetch` here is the same implementation `dyna:http` documents; both modules can be imported in the same process, in either order (verified).

Showcase — IP/CIDR math, a compiled Prefix, and rate limiting (no sockets needed):

```js
import { parseAddr, parsePrefix, Prefix, masked, canonical, isValid, contains, RateLimiter } from "dyna:net";

// parse and canonicalise
const a = parseAddr("192.168.1.1");
print(a.is4, a.is6, a.string, a.bytes.length);               // true false 192.168.1.1 4
print(JSON.stringify(parsePrefix("2001:db8::/32")));         // {"addr":"2001:db8::","bits":32}
print(masked("192.168.1.55/24"), canonical("0:0:0:0:0:0:0:1")); // 192.168.1.0 ::1
print(isValid("1.2.3.4"), isValid("nope"));                  // true false
print(contains("10.0.0.0/8", "10.1.2.3"), contains("10.0.0.0/8", "11.1.2.3")); // true false

// the compiled form: parse/mask once, test many
const p = new Prefix("10.0.0.0/8");
print(p.contains("10.1.2.3"), p.contains("11.0.0.1"), p.contains("not-an-ip"));
// true false false
print(p.bits, p.masked, p.isIPv4);                           // 8 10.0.0.0 true
print(p.overlaps(new Prefix("10.5.0.0/16")), p.overlaps(new Prefix("192.168.0.0/16"))); // true false

// token bucket — the fixed-table shape that cannot be exhausted by forged keys
const rl = new RateLimiter({ tokensPerSec: 5, burst: 3 });
print(rl.allow("a"), rl.allow("a"), rl.allow("a"), rl.allow("a")); // true true true false
print(rl.tokens("a"));                                        // 0
print(JSON.stringify(rl.stats));                              // {"allowed":3,"denied":1,...}
rl.reset("a");
print(rl.allow("a"));                                         // true (refilled bucket)
```

Showcase — Prometheus-style metrics from one process-global registry:

```js
import { Metrics } from "dyna:net";

Metrics.reset();                                   // intended for tests; repeatable runs
Metrics.counter("http_requests", 2, { method: "GET" });
Metrics.gauge("live_conns", 5);
Metrics.histogram("request_latency", 0.05);
const text = Metrics.scrape();
print(text.includes("http_requests"), text.includes("live_conns"), text.includes("request_latency"));
// true true true
try { Metrics.counter("http_requests", -1); } catch (e) { print(e.name); } // RangeError: negative increment refused
```

### remaining exports

- Every resource class (`TCPServer`, `UDPSocket`, `DNSResolver`, `DNSServer`, `Redis`, `PostgreSQL`, `SQLite`, `TCPProxy`) gains `close()`, `dispose()`, `closed`, and `[Symbol.dispose]` from the shared resource wrapper; `close()` and `dispose()` are the same teardown, and a connection's pending queries reject with `client closed` when it runs.
- The shared HTTP classes carry their own residual members, all documented under `dyna:http`: `AbortSignal.prototype._abort`, `AbortSignal.prototype.onabort`, `Request.prototype.signal`, `Response.prototype.url`.

@@NET_END@@

# dyna:bytes

Byte-buffer construction, slicing, search, fixed-width read/write accessors, and text interpretation (Bytes and Text).

`import { Bytes, Text, bytesOf, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, fill, toUtf8, fromUtf8, isValidUtf8, isValidUtf16, countUtf8, countUtf16, latin1ToUtf8, utf8ToLatin1, utf8ToUtf16, utf16ToUtf8, decode, encode, encodingExists, encodings } from "dyna:bytes";`

### Bytes

**`new Bytes(data)`**

- `data` — a string (encoded as UTF-8, the interpretation every codec here assumes), a byte-addressed view (Uint8Array, Int8Array, Uint8ClampedArray, DataView), or an ArrayBuffer.

The bytes are **copied**, never aliased, so a later write through the source cannot invalidate the cached `isAscii`/`isValidUtf8` flags.

```js
import { Bytes } from "dyna:bytes";

const b = new Bytes(new Uint8Array([0xDE, 0xAD, 0xBE, 0xEF]));
const s = new Bytes("héllo");      // string -> UTF-8 bytes
const zeroed = Bytes.alloc(16);    // zero-filled buffer
```

**`Bytes.alloc(n) -> Bytes`**

- `n` *(number)* — the length in bytes, up to 2^31.

Creates a zero-filled handle. Lengths above 2^31 throw `RangeError`.

**`Bytes.isBytes(v) -> boolean`**

- `v` — any value.

True when `v` is a Bytes handle.

**`Bytes.concat(list [, ...more]) -> Bytes`**

- `list` — an array of byte-addressed views.
- `more`further byte-addressed views, appended after the list's elements (`Bytes.concat(a, b, c)` is the variadic form, `Bytes.concat([a, b])` the array form, and `Bytes.concat([a], b)` mixes them).

One allocation, sized in a first pass. Throws if the list changes during concatenation (a getter or Proxy trap is not a licence to reallocate).

**`get length -> number`**

The byte count.

**`get isAscii -> boolean`**

True when no byte has the high bit set, computed once at construction.

**`get isValidUtf8 -> boolean`**

True when the bytes are well-formed UTF-8, computed at construction. A slice of an ASCII buffer inherits both flags without rescanning.

**`Bytes.prototype.slice(start, end) -> Bytes`**

- `start` *(number)* — start offset; negative counts from the end.
- `end` *(number)* — end offset; negative counts from the end.

Returns a new Bytes handle that is a **view** sharing the owner's ArrayBuffer (a slice keeps its whole owner alive). Bounds are clamped; `end < start` yields an empty view.

**`compare(other) -> number`**

- `other` — a byte view.

Lexicographic byte comparison against `other`; returns -1, 0, or 1.

**`equals(other) -> boolean`**

- `other` — a byte view.

True when the other view has identical length and bytes.

**`indexOf(needle[, fromIndex]) -> number`**

- `needle` *(number | byte view)* — a byte value (0..255) or a byte view.
- `fromIndex` *(number, default 0)* — where the search starts; negative clamps to 0.

Returns the first position of the needle at or after `fromIndex`; the empty needle matches at the clamped `fromIndex` (0 by default); -1 when absent.: a successive-match loop can walk `indexOf(n, pos)` instead of allocating a `slice()` per hit.

**`lastIndexOf(needle[, fromIndex]) -> number`**

- `needle` *(number | byte view)* — a byte value (0..255) or a byte view.
- `fromIndex` *(number, default `length`)* — the match must START at or before it (the search runs backward); negative clamps to 0.

Returns the last position of the needle; the empty needle matches at the clamped `fromIndex` (`length` by default).

**`includes(needle) -> boolean`**

- `needle` *(number | byte view)* — a byte value (0..255) or a byte view.

True when the needle occurs.

**`count(needle[, fromIndex]) -> number`**

- `needle` *(number | byte view)* — a byte value (0..255) or a byte view.
- `fromIndex` *(number, default 0)* — where the scan starts; negative clamps to 0.

Returns the number of **non-overlapping** occurrences in the remaining window; the empty needle counts `length - fromIndex + 1` (`length + 1` by default).

**`indexOfAny(chars[, fromIndex]) -> number`**

- `chars` — a byte view.
- `fromIndex` *(number, default 0)* — where the scan starts; negative clamps to 0.

Returns the first position at or after `fromIndex` holding any byte of the `chars` view, or -1.

**`startsWith(needle[, fromIndex]) -> boolean`** / **`endsWith(needle[, end]) -> boolean`**

- `needle` *(number | byte view)* — a byte value (0..255) or a byte view.
- `fromIndex` *(number, default 0)* — where the prefix must begin.
- `end` *(number, default `length`)* — the exclusive position the suffix must end at — a match must END there, not merely fit before it.

The `String.prototype` predicates over bytes: `startsWith` compares at `fromIndex`, `endsWith` compares at `end - needle.length`.

**`readBytes(off, len) -> Uint8Array`**

- `off` *(number)* — the first byte to read.
- `len` *(number)* — how many bytes to read.

Returns a **fresh** Uint8Array copying `buf[off .. off+len)` — a copy by design, because the class is a copied byte buffer (`.slice` is the sanctioned view). Throws `RangeError` when the window passes the end.

**`writeBytes(off, src) -> number`**

- `off` *(number)* — where to write.
- `src` — a byte view (including a `Bytes` handle or a subarray of this very buffer).

Copies `src` at `off`, overlap-safe (`memmove`), and returns the number of bytes written. Throws `RangeError` if any part of the write would land past the end — nothing is written partially.

**`fill(val, start, end) -> Bytes`**

- `val` *(number)* — the value, written as its low 8 bits.
- `start` *(number)* — the start of the range.
- `end` *(number)* — the end of the range.

Sets `buf[start..end)` to the low 8 bits of `val` and returns **the handle** — chainable (use `.array` when you need the underlying Uint8Array, or the free `fill(buf, ...)`, which returns the view). Out-of-bounds bounds throw `RangeError`.

**`toUtf8() -> string`**

Decodes the raw bytes as UTF-8; lone or invalid sequences become U+FFFD. This is not validated UTF-8, so use `isValidUtf8` first if that matters. `toString()` is an alias.

```js
import { Bytes } from "dyna:bytes";

const r = new Bytes(new Uint8Array([1, 2, 3, 4]));
r.writeUint32BE(0, 0x01020304);            // write(value, offset)
const b1 = new Bytes(new Uint8Array(8));
b1.writeBigInt64LE(0, -2n);                // 64-bit forms take BigInt
```

Showcase — slicing, searching and filling without copies:

```js
import { Bytes, fromUtf8 } from "dyna:bytes";

const hay = new Bytes("hello world, hello bytes");
print(hay.length, hay.isAscii, hay.isValidUtf8); // 24 true true
print(hay.indexOf(fromUtf8("world")), hay.lastIndexOf(fromUtf8("hello")), hay.count(108)); // 6 13 5
print(hay.includes(fromUtf8("bytes")), hay.indexOfAny(new Uint8Array([44, 33]))); // true 11 (',')
const mid = hay.slice(6, 11);             // view, not copy
print(mid.toString(), mid.compare(new Uint8Array([119,111,114,108,100])) === 0); // world true
mid.fill(0x2A);                           // fills the shared ArrayBuffer in place
print(hay.indexOf(42) === 6);             // true — view aliases owner
print(new Bytes("abcabc").count(new Uint8Array(0))); // 7 (empty needle: length+1)
```

Showcase —: windowed search without slices, bulk read/write, chainable fill:

```js
import { Bytes } from "dyna:bytes";

const b = new Bytes("a,b,,c");
let pos = -1, hits = 0;
while ((pos = b.indexOf(44, pos + 1)) !== -1) hits++;   // walk ',' without slice()
print(hits, b.count(44), b.indexOfAny(new Uint8Array([44]), 3)); // 3 3 4
print(b.startsWith(new Uint8Array([97, 44])), b.endsWith(99), b.endsWith(44, 1)); // true true true
const dst = Bytes.alloc(4).fill(0);
print(dst.writeBytes(1, b.readBytes(0, 3)), dst.readBytes(1, 3).toString()); // 3 "a,b"
print(dst.writeBytes(0, dst.array.subarray(1, 4)), dst.readBytes(0, 3).toString()); // 3 "a,b" (overlap-safe)
print(b.fill(46).toString());             // "....." — fill returns the handle
```

Showcase — binary protocol round-trip (mixed endian + float):

```js
import { Bytes } from "dyna:bytes";

const pkt = new Bytes(new Uint8Array(26));
let off = 0;
off = pkt.writeUint16BE(off, 0xCAFE);
off = pkt.writeUint32LE(off, 123456);
off = pkt.writeBigUint64BE(off, 0x0102030405060708n);
off = pkt.writeFloatLE(off, 3.14);
off = pkt.writeDoubleBE(off, -2.718281828);
print(off === 26);                         // true (2+4+8+4+8)
print(pkt.readUint16BE(0).toString(16));   // cafe
print(pkt.readUint32LE(2));                // 123456
print(pkt.readBigUint64BE(6).toString(16));// 102030405060708
print(Math.abs(pkt.readFloatLE(14) - 3.14) < 1e-6); // true
```

**`readUint8(off) -> number` / `readInt8(off) -> number` / `readUint16LE(off) -> number` / `readUint16BE(off) -> number` / `readInt16LE(off) -> number` / `readInt16BE(off) -> number` / `readUint32LE(off) -> number` / `readUint32BE(off) -> number` / `readInt32LE(off) -> number` / `readInt32BE(off) -> number` / `readBigUint64LE(off) -> bigint` / `readBigUint64BE(off) -> bigint` / `readBigInt64LE(off) -> bigint` / `readBigInt64BE(off) -> bigint` / `readFloatLE(off) -> number` / `readFloatBE(off) -> number` / `readDoubleLE(off) -> number` / `readDoubleBE(off) -> number`**

- `off` *(number)* — the byte offset to read from.

Fixed-width reads of 1, 2, 4, or 8 bytes with explicit endianness and signedness. Every read throws `RangeError` when `offset + width` exceeds the buffer.

**`writeUint8(off, val) -> number` / `writeInt8(off, val) -> number` / `writeUint16LE(off, val) -> number` / `writeUint16BE(off, val) -> number` / `writeInt16LE(off, val) -> number` / `writeInt16BE(off, val) -> number` / `writeUint32LE(off, val) -> number` / `writeUint32BE(off, val) -> number` / `writeInt32LE(off, val) -> number` / `writeInt32BE(off, val) -> number` / `writeBigUint64LE(off, val) -> number` / `writeBigUint64BE(off, val) -> number` / `writeBigInt64LE(off, val) -> number` / `writeBigInt64BE(off, val) -> number` / `writeFloatLE(off, val) -> number` / `writeFloatBE(off, val) -> number` / `writeDoubleLE(off, val) -> number` / `writeDoubleBE(off, val) -> number`**

- `off` *(number)* — the byte offset to write at.
- `val` — the value; the 64-bit forms accept a BigInt, the float forms accept a JS number.

The 18 name/endian/sign shapes mirror the reads, taking `(offset, value)`. Writes one value and returns the offset after it. Writes throw `RangeError` out of bounds.

### Text

**`new Text(s)`**

- `s` *(string)* — the wrapped JS string.

Wraps a JS string and caches `isWide` (whether any code unit is above U+00FF) in one scan at construction. Bytes is storage; Text is a reading of it, and the two classes share the module.

**`get isWide -> boolean`**

True when any code unit is above U+00FF.

**`get value -> string`**

The wrapped string.

**`get isValidUtf8 -> boolean`**

True when the string's UTF-8 encoding is well-formed. A JS string is ill-formed only through a lone surrogate (a paired surrogate encodes to valid UTF-8); lone surrogates answer false.

note: this member used to be a METHOD (`text.isValidUtf8()`); it is a GETTER now, the same predicate as `bytes.isValidUtf8` and the free `isValidUtf8(data)` — one shared scan, three spellings. The method spelling no longer applies (a property is one slot per name; calling the boolean throws "not a function", exactly like `bytes.isValidUtf8()` always has).

**`isValidUtf16() -> boolean`**

True when the string has no lone surrogate.

**`countUtf8() -> number`**

Returns the UTF-8 code points of the string.

**`countUtf16() -> number`**

Returns the code points, surrogate pairs counted once.

**`toUtf8() -> Uint8Array`**

The string's UTF-8 bytes as a Uint8Array.

**`latin1ToUtf8() -> Uint8Array`**

The string's own bytes, each taken as a Latin-1 code point re-encoded to UTF-8 (0x80..0xFF expand to two bytes).

**`utf8ToLatin1() -> Uint8Array`**

Throws `RangeError` on invalid UTF-8 or any code point above 0xFF.

**`utf8ToUtf16() -> Uint8Array`**

Returns UTF-16LE bytes from the string's UTF-8, throwing `RangeError` on malformed input.

**`utf16ToUtf8() -> Uint8Array`**

Strict and lossless. On a Text this is the string's own UTF-8.

**`toBytes() -> Bytes`**

The string as a Bytes handle.

**`toJSON() -> string` / `toString() -> string`**

The wrapped string.

```js
import { Text, utf8ToUtf16 } from "dyna:bytes";

const t = new Text("héllo");
t.isWide;                      // true
const u16 = utf8ToUtf16("a");  // UTF-16LE bytes: [97, 0]
```

### Free functions

**`bytesOf(view) -> Uint8Array`**

- `view` — a byte-addressed view, a wider view (Uint16Array, Float64Array), a DataView, or an ArrayBuffer.

A Uint8Array **aliasing** exactly the bytes `view` spans; an ArrayBuffer is taken for the whole buffer. Writes through the alias are visible through the original. This is the only function in the module that does not copy.

**`compare(a, b) -> number`**

- `a` — a byte view.
- `b` — a byte view.

-1, 0, or 1 over the two views.

**`equal(a, b) -> boolean`**

- `a` — a byte view.
- `b` — a byte view.

True for identical bytes.

**`indexOf(buf, needle) -> number` / `lastIndexOf(buf, needle) -> number` / `contains(buf, needle) -> boolean` / `count(buf, needle) -> number`**

- `buf` — the byte view to search.
- `needle` *(number | byte view)* — a byte value (0..255) or a byte view.

Search with the same needle rules and empty-needle conventions as the Bytes methods.

**`concat(list) -> Uint8Array`**

- `list` — an array of byte views.

Concatenates the views into one Uint8Array.

**`copy(dst, src, dstOff = 0, srcOff = 0, len = min(dst.length - dstOff, src.length - srcOff)) -> number`**

- `dst` — the destination view.
- `src` — the source view.
- `dstOff` *(number, default 0)* — the destination offset.
- `srcOff` *(number, default 0)* — the source offset.
- `len` *(number)* — defaults to `min(dst.length - dstOff, src.length - srcOff)`.

Overlap-safe (memmove) byte copy returning the number of bytes copied. Offsets out of bounds throw `RangeError`.

**`fill(buf, val, start = 0, end = buf.length) -> Uint8Array`**

- `buf` — the byte view to fill.
- `val` *(number)* — the value, written as its low 8 bits.
- `start` *(number, default 0)* — the start of the range.
- `end` *(number, default buf.length)* — the end of the range.

Sets `buf[start..end)` to the low 8 bits of `val` and returns `buf`.

**`toUtf8(buf) -> string`**

- `buf` — a byte view.

Decodes raw bytes as UTF-8 (invalid sequences become U+FFFD). The inverse of `fromUtf8(str)`.

**`fromUtf8(str) -> Uint8Array`**

- `str` *(string)* — the text to encode.

Encodes the string to a fresh Uint8Array.

**`isValidUtf8(data) -> boolean`**

- `data` — a string or a byte view.

Well-formed UTF-8 check.

**`isValidUtf16(u16bytes) -> boolean`**

- `u16bytes` — a byte view.

Well-formed UTF-16LE check (even length, paired surrogates).

**`countUtf8(data) -> number`**

- `data` — a string or a byte view.

Returns the code points (assumes valid UTF-8).

**`countUtf16(u16bytes) -> number`**

- `u16bytes` — a byte view.

Returns the code points, pairs counted once, no validation.

**`latin1ToUtf8(bytes) -> Uint8Array`**

- `bytes` — a byte view.

Each input byte as a Latin-1 code point re-encoded to UTF-8.

**`utf8ToLatin1(bytes) -> Uint8Array`**

- `bytes` — a byte view.

Throws `RangeError` on invalid UTF-8 or code points above 0xFF.

**`utf8ToUtf16(bytesOrString) -> Uint8Array`**

- `bytesOrString` — a byte view or a string.

Strict; throws on malformed UTF-8.

**`utf16ToUtf8(u16bytes) -> Uint8Array`**

- `u16bytes` — a byte view.

Strict; throws on an odd byte length or an ill-formed surrogate.

### Charsets

**`decode(bytes, label) -> string`**

- `bytes` — a byte view.
- `label` *(string)* — a legacy single-byte charset name; `utf-8` is accepted.

Decodes the byte view from the charset into a string. Undefined bytes become U+FFFD, and `us-ascii` maps every high byte to U+FFFD. An unknown label throws `RangeError`.

**`encode(text, label) -> Uint8Array`**

- `text` *(string)* — the text to encode.
- `label` *(string)* — a legacy single-byte charset name.

Encodes the string into a byte view; a code point the charset cannot express becomes `?`, the substitution every legacy encoder uses. Both arguments must be strings.

**`encodingExists(label) -> boolean`**

- `label` *(string)* — a charset name.

True for every built label, plus `utf-8`/`utf8`; false for the CJK multi-byte families (gbk, big5, shift_jis, euc-jp, euc-kr), which are not built, so a caller can find that out without a throw. Label matching is ASCII case-insensitive with leading/trailing space ignored, per the Encoding Standard.

This is a deliberate boundary, not a gap that will close silently: the built decoders are the
single-byte families plus UTF-8/16, and multi-byte CJK transcoding is out of scope for this
module (the table weight is a whole other dependency). `DetectEncoding` may still NAME a CJK
charset as its verdict; converting the bytes is the caller's job, upstream of or beside DynaJS.

**`encodings() -> string[]`**

The array of every label this build can decode, beginning with `utf-8`.

```js
import { decode, encode, encodingExists } from "dyna:bytes";

const dec = decode(new Uint8Array([0xE9]), "latin1");   // "é"
const enc = encode("é", "latin1");                       // [233]
encodingExists("gbk");                                    // false
```

Showcase — zero-copy views, bulk ops and transcoding:

```js
import { Bytes, Text, bytesOf, compare, equal, concat, copy, fromUtf8, toUtf8, isValidUtf8, latin1ToUtf8, utf8ToLatin1 } from "dyna:bytes";

const view = new Uint16Array([0x4142, 0x4344]); // bytes [66,65,67,68] LE
const alias = bytesOf(view);                  // no copy
print(alias.length, alias[0], alias[1]);      // 4 66 65 (0x42,0x41)
alias[0] = 0x58; print(view[0].toString(16)); // 4158 — writes visible

const a = new Bytes("hello");
const b = new Bytes("hello");
print(equal(a.array, b.array), compare(a.array, b.array) === 0); // true true
print(concat([a.array, new Uint8Array([32]), b.array]).length);  // 11

const dst = new Uint8Array(10);
print(copy(dst, new Uint8Array([1,2,3,4,5]), 2, 1, 3)); // 3
print(dst[2], dst[4]); // 2 4 (memmove-safe even overlapped)

print(isValidUtf8("héllo"), isValidUtf8(new Uint8Array([0xFF]))); // true false
print(fromUtf8("hi").length, toUtf8(fromUtf8("hi")) === "hi");    // 2 true

const t = new Text("café");
print(t.isWide, t.countUtf8(), t.countUtf16()); // false 4 4 (code points; é is one)
print(latin1ToUtf8(new Uint8Array([0xE9])).length); // 2 (0xC3 0xA9)
print(utf8ToLatin1(new Uint8Array([0xC3,0xA9]))[0]); // 233
try { utf8ToLatin1(new Uint8Array([0xC3,0x28])); } catch(e){ print(e.name); } // RangeError
```

---

# dyna:encoding

Charset detection, Base64/Base32/BaseX/Base58/Base85, hex, LEB128 varints, JSON5, stable JSON, JSONPath, and QR codes. Encoders take bytes (a view, an ArrayBuffer, or a string as UTF-8) and return a string; decoders reverse that and **throw on malformed input** rather than substituting.

`import { HexEncode, HexDecode, hexEncodeInto, hexDecodeInto, Base64Encode, Base64Decode, base64EncodeInto, base64DecodeInto, Base64URLEncode, Base64URLDecode, base64UrlEncodeInto, base64UrlDecodeInto, Base32Encode, Base32Decode, base32EncodeInto, base32DecodeInto, Base32HexEncode, Base32HexDecode, base32HexEncodeInto, base32HexDecodeInto, Base85Encode, Base85Decode, base85EncodeInto, base85DecodeInto, Base58Encode, Base58Decode, base58EncodeInto, base58DecodeInto, Base58CheckEncode, Base58CheckDecode, base58CheckEncodeInto, base58CheckDecodeInto, BaseXEncode, BaseXDecode, PutUvarint, Uvarint, PutVarint, Varint, appendUvarint, uvarintAt, varintAt, DetectEncoding, detectEncoding, JSON5Parse, JSON5Stringify, StableStringify, JSONPath, QREncode, QRToString } from "dyna:encoding";`

### Encoding Utilities

```js
import { Base64Encode, Base64Decode, JSON5Parse } from "dyna:encoding";

const b64 = Base64Encode(new Uint8Array([1, 2, 3]));
const raw = Base64Decode(b64);
const j5 = JSON5Parse("{ unquoted: 'string', trailing: 123, }");
```

### Hex, Base64, Base64URL

**`HexEncode(data) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).

Returns the lowercase hex string, SIMD-accelerated.

**`HexDecode(text) -> Uint8Array`**

- `text` *(string)* — a hex string.

Returns a Uint8Array; throws `SyntaxError` on an odd-length string or an invalid digit.

**`hexDecodeInto(text, out) -> number`**

- `text` *(string)* — a hex string.
- `out` — a byte view the decoded bytes are written into.

Returns the number of bytes written. Throws `RangeError` when `out` holds fewer than `text.length / 2` bytes (checked BEFORE decoding, so nothing is written partially); the same `SyntaxError`s as `HexDecode` on malformed text. This is the output-buffer form — the hot direction, since every ENCODER's output is a JS string and a string cannot be written into a buffer in JS, so no `*EncodeInto` exists for the text codecs.

**`Base64Encode(data) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).

Standard RFC 4648 base64 (`+/` alphabet, `=` padded). WARNING (a data-corruption trap, not a style choice): this is a BYTE-level codec, while the global `atob`/`btoa` are WHATWG latin-1 STRING codecs — the two give different answers for the same input (`btoa("é")` is `6Q==`, one raw byte; `Base64Encode` of the UTF-8 bytes is `w6k=`). Which base64/percent door for which payload:

| Payload | Door |
|---|---|
| raw bytes to base64 text | `Base64Encode`/`Base64Decode` (here) |
| URL-safe base64 (JWT, PKCE) | `Base64URLEncode`/`Base64URLDecode` (here) |
| JS string to base64 (UTF-8) | `String.prototype.encodeBase64`/`decodeBase64` |
| latin-1 web-compat strings | global `atob`/`btoa` (NOT UTF-8 — see the globals section) |
| form/query percent-encoding | `dyna:url` `formEncode`/`formDecode` |
| component percent-encoding | global `encodeURIComponent` / `url.encodeURIComponentStrict` |

**`Base64Decode(text) -> Uint8Array`**

- `text` *(string)* — a base64 string.

Throws `SyntaxError` on invalid base64.

**`base64DecodeInto(text, out) -> number`**

- `text` *(string)* — a base64 string.
- `out` — a byte view.

Writes the decoded bytes at `out`'s offset 0 and returns the count. `out` needs at least `3 * (text.length / 4)` bytes or `RangeError`. Whitespace is rejected exactly as `Base64Decode` rejects it.

**`Base64URLEncode(data) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).

RFC 4648 section 5 (`-`/`_` instead of `+`/`/`, no padding).

**`Base64URLDecode(text) -> Uint8Array`**

- `text` *(string)* — a base64url string.

A length of `4k+1` throws `SyntaxError` (no byte count encodes to that many characters), and a stray `+`/`/` is rejected.

**`base64UrlDecodeInto(text, out) -> number`**

Same contract, `out` needs at least `3 * ((text.length + 3) / 4)` bytes or `RangeError`.

**`base32DecodeInto(text, out) -> number`** / **`base32HexDecodeInto(text, out) -> number`**

The base32 decoders' buffer forms. The codec requires a padded string whose length is a multiple of 8, so `out` needs exactly `(text.length / 8) * 5` bytes or `RangeError`.

**`base85DecodeInto(text, out) -> number`**

The ascii85 decoder's buffer form; `out` needs at least `4 * text.length` bytes (the bound that survives whitespace-skipping input) or `RangeError`.

**`base58DecodeInto(text, out) -> number`** / **`base58CheckDecodeInto(text, out) -> number`**

The base58 decoders' buffer forms; `out` needs at least `text.length` bytes or `RangeError`. There is deliberately NO `baseXDecodeInto`: the division codec allocates its workspace internally anyway, so the buffer form would only relocate one `memcpy` while adding a second capacity contract.

**`hexEncodeInto(data, out) -> number`** / **`base64EncodeInto(data, out) -> number`** / **`base64UrlEncodeInto(data, out) -> number`** / **`base32EncodeInto(data, out) -> number`** / **`base32HexEncodeInto(data, out) -> number`** / **`base85EncodeInto(data, out) -> number`** / **`base58EncodeInto(data, out) -> number`** / **`base58CheckEncodeInto(data, out) -> number`**

The ENCODE mirror of the `*DecodeInto` contract: the encoded text is ASCII bytes written at `out`'s offset 0 and the count of bytes written is returned -- hot loops composing output into a caller-owned buffer allocate nothing per field. Same rules as the decode side: the capacity is checked BEFORE anything is written and a too-small `out` throws `RangeError` naming the shortfall; the one-shots and the Into forms call the same core codecs, so their bytes are identical; and input and output buffers that OVERLAP throw `TypeError` (in-place encoding would read bytes already overwritten). Capacity bounds -- the same formulas the one-shots allocate: hex `2*n`; base64/base64url `4*ceil(n/3)`; base32/base32hex `((n+4)/5)*8`; base85 `((n+3)/4)*5`; base58 `(n*8)/5+1` (0 for empty input); base58check `((n+4)*8)/5+1`. `base58EncodeInto` is ZERO-allocation (the buffer is its own workspace) and keeps the family's 4096-byte input cap. `base64UrlEncodeInto` shares the base64 core, which compacts in place: up to 3 pad bytes past the returned count are scratch (never past the bound). There is deliberately no `baseXEncodeInto`, matching the decode side.

```js
import { hexEncodeInto, hexDecodeInto, base64DecodeInto } from "dyna:encoding";

const buf = new Uint8Array(8).fill(0xAA);
const n = hexDecodeInto("deadbeef", buf);        // writes at offset 0
print(n, Array.from(buf.subarray(0, 4)));        // 4 [222, 173, 190, 239]
print(Array.from(buf.subarray(4)));              // [170, 170, 170, 170] — untouched tail
try { hexDecodeInto("abcdef", new Uint8Array(2)); } catch (e) { print(e instanceof RangeError); } // true
```

```js
import { HexEncode, Base64URLEncode } from "dyna:encoding";

HexEncode(new Uint8Array([0xDE, 0xAD]));        // "dead"
Base64URLEncode(new Uint8Array([0xFB, 0xFF]));  // "-_8"
```

### Base32 and Base32Hex

**`Base32Encode(data) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).

RFC 4648 base32, `=` padded.

**`Base32Decode(text) -> Uint8Array`**

- `text` *(string)* — a base32 string.

Throws `SyntaxError` on invalid input.

**`Base32HexEncode(data) -> string` / `Base32HexDecode(text) -> Uint8Array`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).
- `text` *(string)* — a base32hex string.

The extended-hex alphabet, otherwise identical.

### Base85

**`Base85Encode(data) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).

Adobe-less ascii85 (`!`..`u`, no `<~` delimiters): 4 input bytes to 5 characters, with the `z` shorthand for an all-zero group. A trailing partial group of 1-3 bytes encodes as `count + 1` characters.

**`Base85Decode(text) -> Uint8Array`**

- `text` *(string)* — ascii85 text.

Skips whitespace (space/tab/CR/LF/VT/FF) for line-wrapped formats. `z` is recognized only at a group boundary, so a stray `z` throws `SyntaxError`. A full group may name at most 2^32-1 (`s8W-!`, the largest accepted); a group naming more (`s8W-"`, `uuuuu`) is one no encoder emits and throws `SyntaxError`, matching Python's `Ascii85 overflow`. Verified byte-for-byte against Python's `base64.a85encode/a85decode(adobe=False)`.

### Base58 and Base58Check

**`Base58Encode(data) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).

The Bitcoin alphabet (`123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz`), with each leading zero byte as a leading `1`.

**`Base58Decode(text) -> Uint8Array`**

- `text` *(string)* — base58 text.

Throws `SyntaxError` on a character outside the alphabet.

**`Base58CheckEncode(data) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).

Appends the first 4 bytes of the **double** SHA-256 as a checksum, per the spec.

**`Base58CheckDecode(text) -> Uint8Array`**

- `text` *(string)* — base58check text.

Throws `SyntaxError` when the checksum does not match or the input is too short to carry one.

Base58 and Base58Check are division codecs (O(n²)); input is capped at 4096 bytes and exceeding it throws `RangeError`.

### BaseX

**`BaseXEncode(data, alphabet) -> string`**

- `data` — a byte view, an ArrayBuffer, or a string (as UTF-8).
- `alphabet` *(string)* — 2 to 255 distinct characters.

Encodes bytes in the caller-supplied alphabet; every leading zero byte becomes the alphabet's first character. Input is capped at 4096 bytes.

**`BaseXDecode(text, alphabet) -> Uint8Array`**

- `text` *(string)* — text in the alphabet.
- `alphabet` *(string)* — the same alphabet used to encode.

The inverse, throwing `SyntaxError` on a character outside the alphabet. A repeated alphabet character throws `RangeError` at the call (a duplicate silently corrupts decoding). Input is capped at 4096 bytes.

```js
import { BaseXEncode, BaseXDecode } from "dyna:encoding";

BaseXEncode(new Uint8Array([0xFF]), "0123456789abcdef");  // "ff"
BaseXDecode("ff", "0123456789abcdef");                    // Uint8Array [255]
```

### Varints

**`PutUvarint(value) -> Uint8Array`**

- `value` — a non-negative safe integer or a BigInt.

LEB128 encoding of a non-negative value, at most 10 bytes. A fractional, negative, or unsafe number throws `RangeError`.

**`PutVarint(value) -> Uint8Array`**

- `value` — a safe integer or a BigInt.

Zigzag-encoded signed LEB128. A fractional or unsafe number throws `RangeError`.

**`Uvarint(buf) -> [value, bytesRead]` / `Varint(buf) -> [value, bytesRead]`**

- `buf` — a byte view.

Decode, returning `[value, bytesRead]`. The magnitude comes back as a Number when it fits exactly (≤ 2^53 − 1) and as a BigInt otherwise, so a caller never sees silent precision loss; a truncated buffer returns `[0, 0]`. An encoding whose value would need more than 64 bits (an 11th continuation byte, or a final byte above the 64-bit boundary) throws `RangeError`.

**`appendUvarint(buf, value[, offset]) -> number`**

- `buf` — the Uint8Array written into.
- `value` — a non-negative safe integer or a BigInt (same coercion as `PutUvarint`).
- `offset` *(number, default 0)* — where to write.

Writes the Varint at `offset` and returns the NEW END offset (`offset + bytesWritten`). NO reallocation, ever: the caller owns capacity, and a buffer too small at `offset` throws `RangeError` BEFORE anything is written (the value is encoded into stack scratch first). This plus `uvarintAt` is the Varint-STREAM pair — write a sequence, then walk it back without a tuple allocation per field.

**`uvarintAt(buf[, offset]) -> number | bigint`** / **`varintAt(buf[, offset]) -> number | bigint`**

- `buf` — a byte view.
- `offset` *(number, default 0)* — where to start reading.

Returns the decoded VALUE alone — Number when it fits exactly (≤ 2^53 − 1), BigInt above. A truncated stream returns 0 (the value half of `Uvarint`'s `[0, 0]`); a 64-bit overflow throws the same `RangeError`; an offset beyond the end is `RangeError` (an argument bug, not a short stream), while offset == length is an empty stream and returns 0. `varintAt` is the zigzag signed form.

```js
import { PutUvarint, Uvarint, PutVarint, appendUvarint, uvarintAt, varintAt } from "dyna:encoding";

PutUvarint(300);                 // Uint8Array [172, 2]
Uvarint(new Uint8Array([0xAC, 0x02]));  // [300, 2]
PutVarint(-2);                   // Uint8Array [3]

const buf = new Uint8Array(16);
let off = appendUvarint(buf, 300);     // 2
off = appendUvarint(buf, 2n**53n, off); // 10 (5 bytes); BigInt crosses the safe boundary
print(uvarintAt(buf, 0), uvarintAt(buf, 2) === 2n**53n, varintAt(PutVarint(-2))); // 300 true -2
try { appendUvarint(new Uint8Array(1), 300); } catch (e) { print(e instanceof RangeError); } // true
```

### Charset detection

**`DetectEncoding(data[, { fallback, allowList }]) -> string`**

- `data` — a byte view.
- `fallback` *(string, default "utf-8")* — the label used when detection is undecided.
- `allowList` *(string[], optional)* — an allowed verdict list, matched case-insensitively.

Deterministic BOM probe first (utf-32be/le, utf-8, utf-16be/le), then a fast SIMD UTF-8 validity check, then a statistical CJK distribution probe (gbk, big5, shift_jis, euc-jp, euc-kr) once the input is at least 16 bytes. When `allowList` is given, the verdict must be a member or the `fallback` must be; otherwise the call throws `TypeError`.

**`detectEncoding(data[, { fallback, allowList }]) -> string`**

The same function under its lowercase name.
### JSON5

**`JSON5Parse(text) -> any`**

- `text` *(string)* — JSON5 source.

The JSON5 superset: unquoted keys, single quotes, trailing commas, comments, hex and infinity literals. A depth cap of 256 is checked before descending, so a nest bomb is refused rather than exhausting the stack. Malformed input throws `SyntaxError` naming the offset.

**`JSON5Stringify(value[, { indent }]) -> string`**

- `value` — the value to serialize.
- `indent` *(number, default 0)* — clamped to 0-10.

JSON5 output with unquoted keys and `NaN`/`Infinity` literals. Cycles are detected against the ancestor chain (a repeated node in a DAG is legal) and throw `TypeError`.

**`StableStringify(value, opts?) -> string`**

- `value` — the value to serialize.
- `opts` *(object, optional)* — `indent` is accepted for symmetry with `JSON5Stringify` and ignored: the canonical form is always compact.

RFC 8785 canonical form: keys sorted by UTF-16 code unit, no whitespace, quoted keys, `-0` as `0`. `NaN`/`Infinity` are rejected with `TypeError` since they have no canonical form. Both stringify forms reject nesting deeper than 256.

```js
import { JSON5Parse, StableStringify } from "dyna:encoding";

JSON5Parse("{ a: 1, b: 'x', }").a;   // 1
StableStringify({ b: 1, a: 2 });     // {"a":2,"b":1}
```

### JSONPath

**`new JSONPath(expression)`**

- `expression` *(string)* — an RFC 9535 JSONPath expression.

Compiles the expression once and reuses it; a syntax error or an expression longer than 4096 bytes throws. Queries are **data-only**: an accessor property is skipped, never invoked, so a query cannot run user code.

**`jsonpath.all(value) -> array`**

- `value` — the value to query.

Array of every match.

**`jsonpath.first(value) -> any`**

- `value` — the value to query.

The first match, or `undefined` when none.

**`jsonpath.paths(value) -> string[]`**

- `value` — the value to query.

Array of normalized path strings like `$['store']['book'][0]['author']`.

A query that visits more than 2^20 nodes throws `RangeError`; filter nesting is capped at 32.

```js
import { JSONPath } from "dyna:encoding";

const jp = new JSONPath("$.store.book[*].author");
jp.all({ store: { book: [{ author: "a" }, { author: "b" }] } });  // ["a", "b"]
jp.paths({ store: { book: [{ author: "a" }] } });
```

### QR codes

**`QREncode(text[, { ecc, version, mask }]) -> { version, size, modules }`**

- `text` *(string)* — the payload.
- `ecc` *(string, default "M")* — `"L"`, `"M"`, `"Q"` or `"H"`.
- `version` *(number, default: smallest that fits)* — 1-40.
- `mask` *(number, default: automatic)* — 0-7.

Renders a QR symbol, returning `{ version, size, modules }` where `modules` is a `size × size` Uint8Array of dark-module flags. A symbol holds at most 2953 bytes; overflow throws `RangeError`.

**`QRToString(text[, opts]) -> string`**

- `text` *(string)* — the payload.
- `opts` — the same options as `QREncode` (`ecc`, `version`, `mask`).

The same symbol as text with two half-blocks per cell and a two-module quiet zone.

---

# dyna:decimal

Exact arbitrary-precision decimal arithmetic, and an integral money type. The default context is IEEE 754-2008 decimal128 — 34 significant digits, half-even rounding — the same standard Python's `decimal`, Java's `BigDecimal` and SQL `NUMERIC` speak.

`import { Decimal, Money } from "dyna:decimal";`

### Decimal

**STRICT options:** every Decimal options bag is `{precision, rounding}` and unknown KEYS throw a `TypeError` naming the key and the valid set. Known-but-ignored keys stay accepted: add/sub/mul/mod are exact, so a `precision` there is the documented accepted-and-ignored form; sqrt/exp/ln/log10 likewise ignore `rounding`; divmod checks the bag's shape even though it reads neither key.

**`new Decimal(value)`**

- `value` — a decimal string, a JS number, or another Decimal.

A JS number is taken through its own shortest round-trip text, so `new Decimal(0.1)` is the double 0.1 exactly as JS prints it, not the binary value's 55-digit expansion. NaN and Infinity throw `RangeError`; a malformed string throws `SyntaxError`.

```js
import { Decimal } from "dyna:decimal";

new Decimal("0.1").add("0.2").toString();          // "0.3"
new Decimal("1").div("3", { precision: 10 });      // 0.3333333333
```

**`add(x[, { precision, rounding }]) -> Decimal` / `sub(x[, { precision, rounding }]) -> Decimal` / `mul(x[, { precision, rounding }]) -> Decimal` / `mod(x[, { precision, rounding }]) -> Decimal`**

- `x` — a Decimal, a string, or a number.
- `precision` *(number, default 34)* — 1-5000.
- `rounding` *(string, default "halfEven")* — `up | down | ceil | floor | halfUp | halfDown | halfEven | halfOdd`.

Exact; addition and multiplication are not rounded to the context, because rounding a sum that fits is how a ledger loses a cent. A result exceeding 100000 digits, or a multiply past the 2^26 digit-pair cell cap, throws `RangeError`.

**`div(x, { precision, rounding }) -> Decimal`**

- `x` — a Decimal, a string, or a number.
- `precision` *(number, default 34)* — 1-5000.
- `rounding` *(string, default "halfEven")* — `up | down | ceil | floor | halfUp | halfDown | halfEven | halfOdd`.

The only arithmetic that rounds. Division by zero throws `RangeError`; a result exceeding 100000 digits throws `RangeError`.

**`pow(n[, { precision, rounding }]) -> Decimal`**

- `n` *(number)* — an integer in −10000..10000.
- `precision` *(number, default 34)* — 1-5000.
- `rounding` *(string, default "halfEven")* — `up | down | ceil | floor | halfUp | halfDown | halfEven | halfOdd`.

Integer exponentiation with binary powering; a negative exponent takes the reciprocal to the given precision.

**`sqrt([{ precision, rounding }]) -> Decimal`** / **`exp([{ precision, rounding }]) -> Decimal`** / **`ln([{ precision, rounding }]) -> Decimal`** / **`log10([{ precision, rounding }]) -> Decimal`**

- `precision` *(number, default 34)* — 1-5000.
- `rounding` — accepted and **ignored**: these are correctly rounded, always half-even (IEEE's statistically-neutral default; python's decimal ignores its context mode for these ops too).

The first correctly rounded nonlinear operations (add/mul are exact). `sqrt` of a negative, and `ln`/`log10` of zero or a negative, throw `RangeError` — this module has no Infinity to return. `exp` overflows (RangeError) when the ideal result passes `Emax = 999999` — exactly `x ≥ 10^6·ln10`, the same boundary as python decimal's default context, decided against the threshold carried at `precision + 40` digits (the threshold is irrational, so a finite input can never sit exactly on it; an input would need more than `precision + 40` significant digits to land between the carried constant and the true value). A result the context cannot represent — adjusted exponent below `Etiny = −999999 − precision + 1` — is `0` rather than an error (this module prints that zero as `0`; python prints a subnormal `0E-…`), and the subnormal band just above Etiny is quantized to the Etiny exponent, python-style. Between those bounds even a `1e-999998`-scale value is computed in full. `log10` of an exact power of ten is the exact integer. The series core is schoolbook, so `exp`/`ln`/`log10` refuse precisions past a few hundred digits with a clear budget error; `sqrt` (a digit-by-digit integer root) has no such cap.

```js
import { Decimal } from "dyna:decimal";

new Decimal("2").sqrt().toString();                 // 1.414213562373095048801688724209698
new Decimal("2.1025").sqrt().toString();            // 1.45 exactly
new Decimal("1e100").log10().toString();            // 100 exactly
new Decimal("0").exp().toString();                  // 1
new Decimal("-1").exp().toString();                 // 0.3678794411714423215955237701614609
```

**`floor() -> Decimal`** / **`ceil() -> Decimal`** / **`trunc() -> Decimal`**

The integer toward −Infinity / +Infinity / zero. Exact by definition — no context, no rounding option. A zero result is `0`, never `-0` (the module convention, like `neg`).

```js
new Decimal("-2.5").floor().toString();   // -3
new Decimal("-2.5").ceil().toString();    // -2
new Decimal("-2.5").trunc().toString();   // -2
new Decimal("-0.5").ceil().toString();    // 0
```

**`divmod(x) -> [Decimal, Decimal]`**

The exact truncated-division pair: the quotient is toward zero, the remainder takes the dividend's sign (like JS `%`), and `a == q*b + r` holds on the nose. Division by zero throws `RangeError`. `opts` are accepted and ignored: both halves are exact.

```js
new Decimal("-7").divmod("2");   // [-3, -1]
new Decimal("7.5").divmod("2");  // [3, 1.5]
```

**`toBigInt() -> bigint`**

Truncates toward zero — but a value with a fractional part throws `RangeError` instead of silently truncating. A magnitude at or past 2^63 also throws: BigInt itself is arbitrary-precision, but the engine's C bridge hands one over as an int64, and that documented bound is what `toBigInt` returns within.

```js
new Decimal("42").toBigInt();     // 42n
try { new Decimal("-42.9").toBigInt(); }
catch (e) { console.log(e.message); }   // Decimal.toBigInt: the value has a fractional part
try { new Decimal("1e100").toBigInt(); }
catch (e) { console.log(e.message); }   // ...the magnitude exceeds the int64 range...
```

**`Decimal.ZERO` / `Decimal.ONE` / `Decimal.TWO` / `Decimal.TEN` / `Decimal.NEG_ONE`**

The constants as ordinary Decimals — every method works on them.

```js
Decimal.TEN.mul(Decimal.TEN).toString();          // 100
Decimal.NEG_ONE.add(Decimal.ONE).isZero();        // true
new Decimal("19.99").add(Decimal.ZERO).toString(); // 19.99
```

**`abs() -> Decimal`**

The magnitude.

**`neg() -> Decimal`**

The negation; `-0` is `0`.

**`cmp(x) -> number`**

- `x` — a Decimal, a string, or a number.

−1, 0, or 1.

**`equals(x) -> boolean`**

- `x` — a Decimal, a string, or a number.

True when the values are equal (1.5 equals 1.50).

**`round(dp = 0[, rounding]) -> Decimal`**

- `dp` *(number, default 0)* — decimal places, −1000..1000.
- `rounding` *(string, optional)* — a rounding mode name; may also be given as a string.

A new Decimal rounded to `dp` decimal places.

**`toFixed(dp?, rounding?) -> string`**

- `dp` *(number, optional)* — digits after the point, default 0.
- `rounding` *(string, optional)* — a rounding-mode name, same set as `round`: `"up"`, `"down"`, `"ceil"`, `"floor"`, `"halfUp"` (default), `"halfDown"`, `"halfEven"`, `"halfOdd"`.

A string with exactly `dp` digits after the point, printing `-0.00` for a negative value that rounds to zero magnitude.

**`toString() -> string`**

The exact decimal text.

**`toJSON() -> string`**

The same as `toString()` (JSON round-trips exactly).

**`toNumber() -> number`**

The one place a Decimal may become approximate: the engine's ToNumber over the exact text, a single correctly-rounded conversion.

**`isZero() -> boolean`**

True for zero.

**`sign() -> number`**

−1, 0, or 1.

**`digits() -> number`**

The number of significant digits.

```js
import { Decimal } from "dyna:decimal";

new Decimal("2.567").round(1, "halfUp").toString();  // "2.6"
new Decimal("-0.001").toFixed(2);                    // "-0.00"
new Decimal("123.45").digits();                      // 5
```

### Money

**`new Money(minorUnits, currency[, { minorDigits }])`**

- `minorUnits` *(number)* — an integer count of the smallest unit; a fractional cent throws `RangeError`.
- `currency` *(string)* — a 3-letter currency tag, upper-cased (`"usd"` becomes `"USD"`); anything that is not 3 letters throws.
- `minorDigits` *(number, default 2)* — 0-6; overridden per currency (JPY, KRW, VND, CLP, and friends are 0; BHD, IQD, JOD, KWD, LYD, OMR, TND are 3) or set explicitly.

`new Money(1999, "USD")` is $19.99. Money is integer arithmetic with a unit, not float-shaped arithmetic. The options bag is STRICT: `{ minorDigits }` is the whole of it, an unknown key throws a `TypeError` naming the key and the valid set, and a non-object in the options position (a string, a number, …) throws `TypeError` instead of being silently ignored (`null`/`undefined` mean no options). The same bag rules govern all three factories below.

**`Money.fromString(str, currency[, { minorDigits }]) -> Money`**

- `str` *(string)* — the human decimal form: `[-]digits[.digits]` with optional `,` grouping.
- `currency` *(string)* — the 3-letter code, as the constructor takes it.

The brief's example: `Money.fromString("-1,234.56", "USD")` is `new Money(-123456, "USD")`. Parsing is EXACT — the text is accumulated digit by digit and the stored value never touches a double, so `"92,233,720,368,547,758.07"` is one `Money` and one cent more throws `RangeError` (the amount is a count of minor units, so the type's ceiling is INT64 minor units). Grouping is structural, not stripped: after the first group of 1-3 digits every `,` must introduce exactly 3 — `"1,234.56"` and `"1234.56"` are one value, while `"12,34"` is a typo that throws instead of becoming a silently wrong amount. A value FINER than the currency's minor unit throws `RangeError` — a fractional cent is not money, and rounding it away is the bug this type exists to prevent (`"1.2345"` and `"1.005"` on USD throw; on `minorDigits: 4` the first is fine). Trailing zeros are TEXT, not finer value: `"1.500"` on USD is exactly 150 minor units and is admitted — the same VALUE rule `fromDecimal` follows, so all three factories agree (`"5.0"` on JPY is 5).

**`Money.fromDecimal(d, currency[, { minorDigits }]) -> Money`**

- `d` *(Decimal)* — an exact Decimal.

`d * 10^minorDigits` must be a whole number of minor units: `new Decimal("1234.56")` is 123456 cents, `new Decimal("12.3")` is 1230, and `new Decimal("0.001")` on USD throws rather than snapping to zero or rounding. Zero is admitted at any scale, and trailing zeros at or past the scale are exact value, not finer value (`new Decimal("1.500")` is 150) — the same rule `fromString` and `fromMinor` apply to their text forms.

**`Money.fromMinor(n, currency[, { minorDigits }]) -> Money`**

- `n` *(number | bigint | string)* — the raw minor-unit count.

The exact spellings of a raw count: an integer `number`, a `bigint`, or an amount string. The string takes the same amount grammar as `fromString` (optional `,` grouping, optional `.` fraction) with the value required to be whole minor units — `"1,234"` and `"5.0"` are 1234 and 5, `"5.5"` throws. The bigint and string forms are exact past 2^53 — `Money.fromMinor(9007199254740993n, "USD").toString()` is `"90071992547409.93"` — and both refuse past INT64 instead of wrapping (`2n ** 63n` throws; `Number` input keeps the constructor's integer check).

**`add(x) -> Money` / `sub(x) -> Money`**

- `x` — a Money with the **same** currency code.

A cross-currency combine throws `TypeError` (adding USD to EUR is a missing exchange rate, not arithmetic). Result overflows throw `RangeError`.

**`cmp(x) -> number`**

- `x` — a Money.

−1, 0, 1 by amount.

**`equals(x) -> boolean`**

- `x` — a Money.

True when the amounts are equal (same-currency enforced).

**`mul(n) -> Money`**

- `n` *(number)* — an integer multiplier.

Scales the amount by the integer; a fractional multiplier throws `RangeError`, so use `allocate()` to split.

**`allocate(shares) -> Money[]`**

- `shares` *(number[])* — non-negative integer weights summing to at least 1; 1-100000 shares.

Splits the amount into shares whose sum is exactly the original: each share is `floor(amount × w / total)` and the remainder goes one minor unit at a time to the earliest shares, so nothing is created and nothing is lost.

**`toString() -> string`**

The decimal amount with the currency's minor digits (`"19.99"`).

**`toJSON() -> string`**

The same as `toString()`.

**`amount() -> number`**

The minor-unit integer as a `Number`: exact through 2^53 minor units, and past that the double rounds (9007199254740993 minor units read back as 9007199254740992) — the STORE does not round, so read an exact value with `toString()` or `toDecimal()`.

**`currency() -> string`**

The 3-letter code.

**`format() -> string`**

`"$19.99"` for USD/EUR/GBP/JPY/CNY/INR/KRW/CAD/AUD and `"19.99 USD"` otherwise.

**`toDecimal() -> Decimal`**

The amount as an exact Decimal.

```js
import { Money } from "dyna:decimal";

const m = new Money(1999, "USD");
m.add(new Money(1, "USD")).toString();              // "20.00"
m.allocate([1, 1, 1]).map(x => x.amount());         // [667, 666, 666]
new Money(100, "JPY").format();                     // "¥100"
```

Showcase — exact arithmetic, rounding and a ledger-safe split:

```js
import { Decimal, Money } from "dyna:decimal";

// the binary-float surprise, settled
print(new Decimal("0.1").add("0.2").toString());    // 0.3
print(new Decimal("19.99").mul("3").toString());    // 59.97
print(new Decimal("10").div("3", { precision: 12 }).toString()); // 3.33333333333

// rounding is explicit: the mode is a name, not a surprise
print(new Decimal("1.005").round(2, "halfUp").toString());   // 1.01
print(new Decimal("1.005").toFixed(2, "halfEven"));          // 1.00
print(new Decimal("-1234.5678").toFixed(2));                 // -1234.57
print(new Decimal("2.50").equals("2.5"), new Decimal("2.50").cmp("2.5")); // true 0
print(new Decimal("123.45").digits(), new Decimal("1e100").pow(2).toString().slice(0, 8)); // 5 10000000
print(new Decimal("100").toNumber(), new Decimal("-0").sign(), new Decimal("0").isZero()); // 100 0 true

// money is integer arithmetic with a unit — no float shape anywhere
const m = new Money(1999, "USD");
print(m.toString(), m.format(), m.currency(), m.amount()); // 19.99 $19.99 USD 1999
print(m.add(new Money(1, "USD")).toString(), m.sub(new Money(100, "USD")).toString()); // 20.00 18.99
print(m.mul(2).toString());                                // 39.98
print(m.allocate([1, 1, 1]).map(x => x.amount()).join(",")); // 667,666,666 (sum preserved)
print(new Money(100, "JPY").format(), new Money(500, "BHD").toString()); // ¥100 0.500
print(m.toDecimal().toString());                           // 19.99 — exact bridge to Decimal
try { m.add(new Money(1, "EUR")); } catch (e) { print(e.name); } // TypeError: cross-currency is a missing exchange rate, not arithmetic
```

---

# dyna:hash

One-shot and streaming message digests, CRC-32 checksums, and non-cryptographic hashes. Every function accepts a string (UTF-8 bytes), a byte view, or an ArrayBuffer; the `*Hex` forms return a lowercase hex string, the others a fresh Uint8Array. SHA-1/2, MD5 and CRC-32 are the cores verified against FIPS 180-4, RFC 1321/4231 and IEEE 802.3 vectors.

`import { MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex, SHA256, SHA256Hex, SHA384, SHA384Hex, SHA512, SHA512Hex, CRC32, CRC32C, XXHash32, XXHash64, XXH3_64, SHA3_224, SHA3_224Hex, SHA3_256, SHA3_256Hex, SHA3_384, SHA3_384Hex, SHA3_512, SHA3_512Hex, Keccak256, Keccak256Hex, SHAKE128, SHAKE128Hex, SHAKE256, SHAKE256Hex, BLAKE3, BLAKE3Hex, BLAKE2b, BLAKE2bHex, BLAKE2s, BLAKE2sHex, Murmur3_128, Murmur3_128Hex, Hasher } from "dyna:hash";`

### One-shot digests

**`MD5(data) -> Uint8Array` / `MD5Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

128-bit, RFC 1321.

**`SHA1(data) -> Uint8Array` / `SHA1Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

160-bit, FIPS 180-4.

**`SHA224(data) -> Uint8Array` / `SHA224Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

224-bit.

**`SHA256(data) -> Uint8Array` / `SHA256Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

256-bit, the engine's default digest.

**`SHA384(data) -> Uint8Array` / `SHA384Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

384-bit.

**`SHA512(data) -> Uint8Array` / `SHA512Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

512-bit.

```js
import { SHA256Hex } from "dyna:hash";

SHA256Hex("abc");
// ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
```

### CRC

**`CRC32(data) -> number`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

IEEE 802.3 CRC-32 as a non-negative number. Uses the SSE4.2 `crc32` instruction on x86-64 when available.

**`CRC32C(data) -> number`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

The Castagnoli polynomial, same shape.

### SHA-3, Keccak and SHAKE

**`SHA3_224(data) -> Uint8Array` / `SHA3_224Hex(data) -> string` / `SHA3_256(data) -> Uint8Array` / `SHA3_256Hex(data) -> string` / `SHA3_384(data) -> Uint8Array` / `SHA3_384Hex(data) -> string` / `SHA3_512(data) -> Uint8Array` / `SHA3_512Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

224/256/384/512-bit FIPS 202 digests.

**`Keccak256(data) -> Uint8Array` / `Keccak256Hex(data) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

The original Keccak padding, the form Ethereum uses.

**`SHAKE128(data, length = 32) -> Uint8Array` / `SHAKE128Hex(data, length = 32) -> string` / `SHAKE256(data, length = 32) -> Uint8Array` / `SHAKE256Hex(data, length = 32) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `length` *(number, default 32)* — output length, 1..2^20 bytes.

Extensible output. All are one Keccak-f[1600] permutation with different rate and padding.

### BLAKE and Murmur

**`BLAKE3(data, length = 32) -> Uint8Array` / `BLAKE3Hex(data, length) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `length` *(number, default 32)* — output length, 1..2^20 bytes.

A Merkle tree over 1 KiB chunks, not a serial hash.

**`BLAKE2b(data, length = 64 | opts) -> Uint8Array` / `BLAKE2bHex(data, length | opts) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `length` *(number, default 64)* — 1..64 bytes.
- `opts` *(object, optional)* — `{ key?, length? }`. `key` (bytes or string, 0–64 bytes) runs the RFC 7693 keyed mode — a MAC without the HMAC construction. An EMPTY key is legal and means unkeyed: it is the parameter block's `key_length` field, not a flag. A key over 64 bytes refuses.

**`BLAKE2s(data, length = 32 | opts) -> Uint8Array` / `BLAKE2sHex(data, length | opts) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `length` *(number, default 32)* — 1..32 bytes.
- `opts` *(object, optional)* — `{ key?, length? }`, the BLAKE2s twin: key 0–32 bytes, empty = unkeyed.

**`BLAKE3(data, length = 32 | opts) -> Uint8Array` / `BLAKE3Hex(data, length | opts) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `length` *(number, default 32)* — 1..1048576 bytes (the tree's extendable output).
- `opts` *(object, optional)* — one of two modes, never both:
  - `{ key }` — keyed mode. The key is EXACTLY 32 bytes (BLAKE3 has no variable-length key; an empty key is not "unkeyed" here — it refuses).
  - `{ context }` or `{ deriveKey }` (the same option, two spellings) — derive-key mode: the context string (non-empty; it is domain separation) is hashed under `DERIVE_KEY_CONTEXT`, and that 32-byte root hash keys a `DERIVE_KEY_MATERIAL` pass over `data`. The output is exactly 32 bytes — it keys the next primitive — so `length` is refused.

Vectors: RFC 7693's keyed appendix for BLAKE2b/2s and the official BLAKE3 test vectors for keyed and derive-key are pinned in `tests/test_hash_keyed.js` — the derive-key flag placement (material WITHOUT the keyed bit) is the one the reference implementation decides.

**`Murmur3_128(data, seed = 0) -> Uint8Array` / `Murmur3_128Hex(data, seed = 0) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `seed` *(number, default 0)* — the seed, not a length.

Non-cryptographic 128-bit hash. Each of the above has a `*Hex` form.

```js
import { BLAKE2bHex, Murmur3_128Hex } from "dyna:hash";

BLAKE2bHex("abc");   // 128 hex chars (64 bytes, the default length)
Murmur3_128Hex("abc", 7);   // seed 7
```

### xxHash

**`XXHash32(data, seed = 0) -> number`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `seed` *(number, default 0)* — the seed.

A non-cryptographic 32-bit hash returned as a number. The seed must be a finite number in the int64 range.

**`XXHash64(data, seed = 0) -> string`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `seed` *(number, default 0)* — the seed.

A non-cryptographic 64-bit hash returned as a **16-character hex string** by default: a JS number carries only 53 exact bits, and a silently-rounded 64-bit hash would collide in ways the algorithm does not. `opts.as` selects the exact-value forms — `"bigint"` (the full 64 bits) or `"bytes"` (8 bytes, little-endian, the format's own byte order). The seed must be a finite number in the int64 range.

**`XXH3_64(data, seed = 0, opts?) -> string | bigint | Uint8Array`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.
- `seed` *(number, default 0)* — the seed.
- `opts` *(object, optional)* — `{ as?: "hex" | "bigint" | "bytes" }`, exactly like `XXHash64`.

XXH3-64 from the xxHash 0.8 specification with the default secret — the modern one-shot cousin of `XXHash64`, implemented from the published stripe constants and verified byte-exact against the reference implementation. Same shapes and the same caveat: **not a security primitive** (seed-recoverable, trivially collidable).

```js
import { XXHash64, XXH3_64 } from "dyna:hash";

print(XXH3_64("hello world").length);                     // 16 (hex chars, the default)
print(XXH3_64("hello world", 7, { as: "bigint" }));       // the exact 64-bit value as a bigint
print(XXH3_64("hello world", 7, { as: "bytes" }).length); // 8 little-endian bytes
print(XXHash64("hello world", 0, { as: "bigint" }) > 0n); // XXHash64 gained the same opts
```

### Hasher

**`new Hasher(algorithm, opts?)`**

- `algorithm` *(string)* — any one-shot digest name this module exports: `md5 | sha1 | sha224 | sha256 | sha384 | sha512 | sha3_224 | sha3_256 | sha3_384 | sha3_512 | keccak256 | shake128 | shake256 | blake2b | blake2s | blake3` (the same strcmp names as the functions); an unknown name throws `TypeError`.
- `opts` *(object, optional)* — `{ length }` sizes the extensible-output algorithms: `shake128`/`shake256`/`blake3` 1..1048576 bytes, `blake2b` 1..64, `blake2s` 1..32 (their one-shot length bounds). The fixed-digest algorithms refuse it with a RangeError; `digestSize` reports the effective length either way.

A streaming digest. The state is malloc'd native memory, so `Hasher` carries the module family's full resource surface: `close()` (alias `dispose()` and `[Symbol.dispose]`) releases it at a moment of your choosing, `closed` reports the state, and the GC finalizer remains the backstop. `close()` is idempotent; every method and getter throws `TypeError` on a closed hasher.

**`hasher.update(data) -> Hasher`**

- `data` — a string (UTF-8 bytes), a byte view, or an ArrayBuffer.

Absorbs bytes; returns `this` for chaining.

**`hasher.digest() -> Uint8Array`**

Finalizes a **copy** so the stream stays usable: further `update()`s and repeated digests are well-defined.

**`hasher.digestHex() -> string`**

The same as a hex string.

**`hasher.digestInto(buf) -> number`**

- `buf` *(Uint8Array)* — a buffer the CALLER owns; no allocation happens in the call.

Finalizes a copy (exactly like `digest()`, so the hasher stays usable) and writes the digest bytes into `buf`, returning the byte count written. A `buf.byteLength` below `digestSize` throws `RangeError` — a truncated digest is indistinguishable from a real one, so it is refused rather than partially written — and the bytes land at the view's own data pointer, so a `subarray` writes into its slice and nowhere else. The buffer is coerced before the native handle is resolved, so a `valueOf` that closes the hasher mid-call yields the clean "closed" `TypeError`.

**`hasher.reset() -> Hasher`**

Returns the hasher to its initial state for reuse without reallocation.

**`get algorithm -> string`**

The algorithm name.

**`get digestSize -> number`**

The digest length in bytes.

**`close()` / `dispose()` / `[Symbol.dispose]`**

Release the native state early. Idempotent; after closing, every method and getter throws `TypeError` and `closed` reads `true`.

**`get closed -> boolean`**

`true` once the hasher has been closed (explicitly or by the finalizer).

```js
import { Hasher } from "dyna:hash";

const h = new Hasher("sha256");
h.update("a").update("bc");
h.digestHex();          // ba7816bf...f20015ad
h.digest().length;      // 32
```

Showcase — one-shot digests, checksums, and the streaming Hasher (FIPS 180-4 vector pinned):

```js
import { SHA256Hex, SHA512Hex, SHA3_256Hex, Keccak256Hex, BLAKE3Hex, BLAKE2bHex, Murmur3_128Hex, XXHash32, XXHash64, CRC32, CRC32C, Hasher } from "dyna:hash";

// the FIPS 180-4 "abc" vector — pinned, not measured
print(SHA256Hex("abc") === "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); // true
print(SHA512Hex("abc").slice(0, 16));       // ddaf35a193617aba
print(SHA3_256Hex("abc").slice(0, 16));     // 3a985da74fe225b2
print(Keccak256Hex("").slice(0, 16));       // c5d2460186f7233c (the empty Keccak digest)

// non-cryptographic hashes: numbers for 32-bit, hex strings for 64-bit
print(XXHash32("abc"));                     // 852579327
print(XXHash64("abc").length);              // 16 (hex chars — a number cannot hold 64 bits)
print(CRC32("abc"), CRC32C("abc"));         // 891568578 910901175

// extendable output
print(BLAKE3Hex("abc").length);             // 64 (32 bytes default)
print(BLAKE2bHex("abc", 16).length);        // 32 (16-byte output)
print(Murmur3_128Hex("abc", 7).length);     // 32 (seeded 128-bit)

// streaming: absorb in pieces, digest a COPY, reset and reuse
const s = new Hasher("sha256");
s.update("a").update("bc");
print(s.digestHex() === SHA256Hex("abc"));  // true
print(s.digest().length);                   // 32 — the stream stays usable
s.reset().update("abc");
print(s.digestHex() === SHA256Hex("abc"));  // true — reset reuses the scratch
print(s.algorithm, s.digestSize);           // sha256 32

// zero-copy finalize into a caller-owned buffer
const out = new Uint8Array(s.digestSize);
s.reset().update("abc");
s.digestInto(out);                          // 32 bytes written
print(out[0] === 0xba);                     // true

// the whole digest table streams now: SHA-3, Keccak, SHAKE, BLAKE2b/2s, BLAKE3
const h3 = new Hasher("sha3_256");
h3.update("a").update("bc");
print(h3.digestHex() === SHA3_256Hex("abc")); // true — streaming == one-shot
const shake = new Hasher("shake128", { length: 40 });
shake.update("abc");
print(shake.digestSize, shake.digestHex().length); // 40 80 — caller-chosen output
const b3 = new Hasher("blake3");
b3.update("a").update("bc");
print(b3.digestHex() === BLAKE3Hex("abc"));   // true
```

# dyna:crypto

Secret-dependent cryptography: AEAD, public-key signatures and key agreement, keyed hashes, key-derivation, password hashing, OTP, JWT and X.509. Everything here is constant-time where a secret is compared; the linked OpenSSL backend provides the AEAD and the PEM-key algorithms.

`import { Bcrypt, Argon2id, RSA, ECDSA, ECDH, X509, AESGCM, ChaCha20Poly1305, Ed25519Generate, Ed25519Sign, Ed25519Verify, Ed25519PemToRaw, Ed25519PemFromRaw, X25519Generate, X25519Derive, X25519PemToRaw, X25519PemFromRaw, HMAC, HMACHex, Hmac, HKDF, PBKDF2, Scrypt, RandomBytes, TimingSafeEqual, HOTPGenerate, TOTPGenerate, HOTPVerify, TOTPVerify, JWTSign, JWTVerify, MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex, SHA256, SHA256Hex, SHA384, SHA384Hex, SHA512, SHA512Hex, CRC32, CRC32C, XXHash32, XXHash64, XXH3_64, SHA3_224, SHA3_224Hex, SHA3_256, SHA3_256Hex, SHA3_384, SHA3_384Hex, SHA3_512, SHA3_512Hex, Keccak256, Keccak256Hex, SHAKE128, SHAKE128Hex, SHAKE256, SHAKE256Hex, BLAKE3, BLAKE3Hex, BLAKE2b, BLAKE2bHex, BLAKE2s, BLAKE2sHex, Murmur3_128, Murmur3_128Hex } from "dyna:crypto";`

The one-shot digests from `MD5` through `Murmur3_128Hex` are the [dyna:hash](#dynahash) entry points re-exported under dyna:crypto (aliases over the same implementations), so S256-style PKCE challenges and fingerprint pins need no second import.

### Bcrypt

**`Bcrypt.hash(password, rounds = 10) -> string`**

- `password` *(string)* — the password to hash. A password over 72 bytes is refused with a RangeError; silently truncating would hash a different password.
- `rounds` *(number, optional, default 10)* — the log2 cost, 4..31.

Returns the OpenBSD `$2b$` bcrypt hash, made with a fresh 16-byte salt.

**`Bcrypt.verify(password, hash) -> boolean`**

- `password` *(string)* — the password to check.
- `hash` *(string)* — the stored hash, treated as untrusted input.

Returns true when the password matches. The hash is recomputed and compared from the `$` onward in constant time. The cost the hash demands is capped at 20 (`$2b$20` and below verify); `$2x$` prefix variants compare equal for byte-clean passwords.

```js
import { Bcrypt } from "dyna:crypto";
const hash = Bcrypt.hash("secret_password", 10);
const ok = Bcrypt.verify("secret_password", hash);
print("bcrypt:", ok, Bcrypt.verify("wrong", hash));
```

### Argon2id

**`Argon2id.hash(password, salt, opts?) -> string`** (default) / **`-> Uint8Array`** with `{ encoded: false }`

**STRICT options:** the bag is `{iterations, memory, parallelism, hashLen, encoded}`; an unknown key throws, on hash() and on the raw verify() form alike.

- `password` *(string)* — the password to hash.
- `salt` *(Uint8Array)* — at least 8 bytes; a shorter salt is refused (RFC 9106 recommends 16).
- `opts` *(object, optional)* — parameters.
  - `iterations` *(number, default 3)* — 1..16.
  - `memory` *(number, default 65536)* — KiB, must be `8*parallelism..2^22`.
  - `parallelism` *(number, default 4)* — 1..16.
  - `hashLen` *(number, default 32)* — 4..2^20.
  - `encoded` *(boolean, default true)* — `true` returns the PHC string `$argon2id$v=19$m=<KiB>,t=<passes>,p=<lanes>$<base64 salt>$<base64 tag>` (unpadded base64, the PHC string format); `false` keeps the raw tag bytes.

By default the salt and every parameter ride IN the returned string, so storing it is the whole contract — no caller-side salt/parameter bookkeeping to get wrong. The hash runs synchronously on the calling thread — at the allowed cap (1 GiB of memory, bounded iterations) that is seconds of blocking — so call it from a worker when hashing while requests are being served.

**`Argon2id.verify(hashString, password) -> boolean`** (PHC form)

- `hashString` *(string)* — a stored `$argon2id$...` string.
- `password` *(string)* — the password to check.

Every parameter comes from the string (version must be 19): a tampered parameter either refuses with a `TypeError` naming the problem (malformed shape, corrupt base64, out-of-range lengths) or simply fails verification. Compared in constant time.

**`Argon2id.verify(password, salt, expectedHash, opts?) -> boolean`** (raw form, unchanged)

- `password` *(string)* — the password to check.
- `salt` *(Uint8Array)* — the salt used at hashing time.
- `expectedHash` *(Uint8Array)* — the stored raw tag.
- `opts` *(object, optional)* — the same parameters as `hash` (they still override the derivation here).

Returns true when the hash recomputed with the same parameters matches, compared in constant time.

```js
import { Argon2id } from "dyna:crypto";
const salt = new TextEncoder().encode("0123456789abcdef");
const phc = Argon2id.hash("hunter2", salt);   // $argon2id$v=19$m=65536,t=3,p=4$...
print("argon2id:", phc.slice(0, 26), Argon2id.verify(phc, "hunter2"));
print(Argon2id.verify(phc, "hunter3"));       // false
const raw = Argon2id.hash("hunter2", salt, { encoded: false });
print(raw.length, Argon2id.verify("hunter2", salt, raw)); // 32 true — the raw form stays
```

**`Argon2id.hashAsync(password, salt, opts?) -> Promise<string | Uint8Array>`** / **`Argon2id.verifyAsync(hashString, password) -> Promise<boolean>`** / **`Argon2id.verifyAsync(password, salt, expectedHash, opts?) -> Promise<boolean>`**

The identical core (`hash`/`verify`) on the shared offload pool, so the calling
thread keeps serving while a large profile runs. Same parameters, same refusals,
same result forms as the sync pair: `hashAsync` settles the PHC string by
default (`{ encoded: false }` the raw bytes); `verifyAsync` takes either the
PHC pair or the raw triple. Needs the dyna:net reactor: without dyna:net these
names are absent from the module (not exported-and-blocking). Calls below the
offload gate (`memory x iterations` block-fills, see `asyncStats().offloadMin`)
settle inline and still return a promise.

**`Argon2id.asyncStats() -> { inline, offloaded, offloadMin }`**

Counters for which arm the async pair took, plus the offload gate in block
fills — tests assert against this rather than trusting a green resolve.

### RSA

**`RSA.generate(bits = 2048) -> { privateKey, publicKey }`**

- `bits` *(number, optional, default 2048)* — the key size: 2048, 3072 or 4096; anything else is refused.

Returns a fresh RSA key pair as PEM strings.

**`RSA.sign(md, privateKey, msg) -> Uint8Array`**

- `md` *(string)* — the digest: `"sha1"`, `"sha256"`, `"sha384"` or `"sha512"` (case-insensitive, `"sha-256"` accepted).
- `privateKey` *(string)* — the PEM private key.
- `msg` *(string)* — the message to sign.

Returns a PKCS#1 v1.5 signature with the given digest.

**`RSA.verify(md, publicKey, msg, sig) -> boolean`**

- `md` *(string)* — the digest, as in `sign`.
- `publicKey` *(string)* — the PEM public key.
- `msg` *(string)* — the signed message.
- `sig` *(Uint8Array)* — the signature.

Returns true when the signature checks against the public key.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { RSA } from "dyna:crypto";
const key = RSA.generate(2048);
const sig = RSA.sign("sha256", key.privateKey, "payload");
print("rsa:", sig.length, RSA.verify("sha256", key.publicKey, "payload", sig));
```

**`RSA.signPSS(md, privateKey, msg, opts?) -> Uint8Array`** / **`RSA.verifyPSS(md, publicKey, msg, sig, opts?) -> boolean`**

RSASSA-PSS (RFC 8017): the same digest table and the same 2048-bit key floor as `sign`/`verify`, with the padding that carries a randomized salt.

- `md` *(string)* — the digest: `"sha1"`, `"sha256"`, `"sha384"` or `"sha512"`.
- `opts` *(object, optional)*:
  - `saltLen` *(number | "max" | "auto", default "auto")* — the salt length in bytes (0 is the deterministic form), `"max"` fills the modulus, and `"auto"` signs with the maximum and VERIFIES by recovering the length from the signature — OpenSSL's own default, so signatures interop byte-for-byte with `openssl dgst -sigopt rsa_padding_mode:pss` (verified both directions).
  - `mgf1Hash` *(string, default = md)* — the MGF1 digest.

`verifyPSS` returns false for a foreign signature, exactly like `verify`.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { RSA } from "dyna:crypto";
const key = RSA.generate(2048);
const sig = RSA.signPSS("sha256", key.privateKey, "payload");
print("pss:", sig.length, RSA.verifyPSS("sha256", key.publicKey, "payload", sig));
const fixed = RSA.signPSS("sha256", key.privateKey, "payload", { saltLen: 32 });
print(RSA.verifyPSS("sha256", key.publicKey, "payload", fixed, { saltLen: 32 }));
```

**`RSA.OAEP.seal(publicKey, plaintext, opts?) -> Uint8Array`** / **`RSA.OAEP.open(privateKey, sealed, opts?) -> Uint8Array`**

RSA-OAEP encryption (RFC 8017): `seal` encrypts TO a public key, `open` decrypts WITH the private one — the AESGCM `seal`/`open` naming on the RSA namespace. The output length is the modulus size; the plaintext must be shorter by `2 + 2*mdLen` bytes.

- `opts` *(object, optional)*:
  - `md` *(string, default "sha256")* — the OAEP digest (pass `"sha1"` for the legacy interop default).
  - `mgf1Hash` *(string, default = md)* — the MGF1 digest.
  - `label` *(BytesInput, optional)* — associated bytes that must round-trip exactly; a mismatch throws.

The same 2048-bit key floor applies (encrypting to a small key is a confidentiality risk, not just a signature one). Interop with `openssl pkeyutl -pkeyopt rsa_padding_mode:oaep` is verified both directions, labels included.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { RSA } from "dyna:crypto";
const key = RSA.generate(2048);
const sealed = RSA.OAEP.seal(key.publicKey, "secret payload");
const back = RSA.OAEP.open(key.privateKey, sealed);
print("oaep:", sealed.length, new TextDecoder().decode(back));
```

### X.509

**`X509.parse(cert) -> object`**

- `cert` *(string | Uint8Array)* — the certificate as a PEM string or DER bytes; malformed input is refused.

Returns `{ subject, issuer, serialNumber, version, notBefore, notAfter, fingerprint, sans }`. `serialNumber` is hex, `version` is 1-based (3 = v3), `fingerprint` is the SHA-256 hex, and `sans` is `{ dns: [], ip: [], email: [] }` from the Subject Alternative Name extension. Parsing is read-only: no chain or path verification is performed — validating a certificate against a trust store is the TLS layer's job.

**`X509.generateSelfSigned({ key, subject, days, sans, extensions }) -> string`**

- `key` *(string)* — the PEM private key (any RSA/EC key `RSA.generate`/`ECDSA.generate` produce).
- `subject` *(string, optional, default "localhost")* — the certificate subject.
- `days` *(number, optional, default 365)* — the validity in days, clamped to at least 1.
- `sans` *(string[], optional)* — Subject Alternative Names, each classified BY SHAPE: contains `@` is an email, an address `inet_pton` parses (v4 or v6) is an IP, anything else is a DNS name — the same classification `X509.parse` reports back. These become the v3 subjectAltName extension, which is what hostname validation reads: a certificate without it fails EVERY verifier, which is exactly what the old no-SAN output did.
- `extensions` *(object[], optional)* — general extensions, `{ name, value, critical? }`, where `value` uses OpenSSL's own config syntax (the same strings `openssl req -addext` takes). The supported names — refused by name otherwise, never silently ignored — are:
  - `subjectAltName` — `"DNS:example.com,IP:10.0.0.1,email:a@b.c"`
  - `keyUsage` — `"digitalSignature,keyEncipherment"` (comma-separated usages)
  - `extendedKeyUsage` — `"serverAuth,clientAuth"` (or a dotted OID)
  - `basicConstraints` — `"CA:TRUE,pathlen:0"` or `"CA:FALSE"`
  - `subjectKeyIdentifier` — `"hash"`
  - `authorityKeyIdentifier` — `"keyid"`

  An entry naming `subjectAltName` while `sans` is also set is refused as ambiguous. Unknown top-level option keys are refused too (this is a strict option bag).

Returns a v3 self-signed certificate signed with SHA-256, as a PEM string. Generated certificates pass `openssl verify -CAfile cert.pem -verify_hostname <name>` for their own SAN entries.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { RSA, X509 } from "dyna:crypto";
const key = RSA.generate(2048);
const certPem = X509.generateSelfSigned({
    key: key.privateKey,
    subject: "localhost",
    days: 30,
    sans: ["localhost", "myapp.test", "127.0.0.1"],
    extensions: [
        { name: "keyUsage", critical: true, value: "digitalSignature,keyEncipherment" },
        { name: "extendedKeyUsage", value: "serverAuth" },
        { name: "basicConstraints", value: "CA:FALSE" },
    ],
});
const info = X509.parse(certPem);
print("x509:", info.subject, info.version, info.fingerprint.slice(0, 8));
print("sans:", info.sans.dns.join(","), info.sans.ip[0]);
```

### ECDSA

**`ECDSA.generate(curve = "P-256") -> { privateKey, publicKey }`**

- `curve` *(string, optional, default "P-256")* — `"P-256"`, `"P-384"` or `"P-521"` (ECDH.generate takes the same list).

Returns an EC key pair as PEM strings.

**`ECDSA.sign(md, privateKey, msg, { format }?) -> Uint8Array`**

- `md` *(string)* — `"sha1"`, `"sha256"`, `"sha384"` or `"sha512"`.
- `privateKey` *(string)* — the PEM private key.
- `msg` *(string)* — the message to sign.
- `format` *(string, optional, default "raw")* — `"der"` for DER `SEQUENCE{INTEGER r, INTEGER s}`.

Returns a raw `R||S` signature (64 bytes for P-256, 96 for P-384, 132 for P-521 — each half padded to the curve's 32/48/66-byte coordinate) by default.

**`ECDSA.verify(md, publicKey, msg, sig, { format }?) -> boolean`**

- `md` *(string)* — the digest, as in `sign`.
- `publicKey` *(string)* — the PEM public key.
- `msg` *(string)* — the signed message.
- `sig` *(Uint8Array)* — the signature.
- `format` *(string, optional)* — must match what `sign` produced.

Returns true for a valid raw or DER signature. A raw signature of the wrong length is a plain false.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { ECDSA } from "dyna:crypto";
const key = ECDSA.generate("P-256");
const sig = ECDSA.sign("sha256", key.privateKey, "msg");
print("ecdsa:", sig.length, ECDSA.verify("sha256", key.publicKey, "msg", sig));
```

### ECDH

**`ECDH.generate(curve = "P-256") -> { privateKey, publicKey }`**

- `curve` *(string, optional, default "P-256")* — the same curve set and PEM shape as `ECDSA.generate`.

Returns an EC key pair as PEM strings.

**`ECDH.derive(privateKey, peerPublicKey) -> Uint8Array`**

- `privateKey` *(string)* — the PEM private key.
- `peerPublicKey` *(string)* — the peer's PEM public key.

Returns the raw X9.63 shared secret. A small-order peer point makes derivation fail and is refused; an all-zero secret is not a secret.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { ECDH } from "dyna:crypto";
const alice = ECDH.generate("P-256"), bob = ECDH.generate("P-256");
const s1 = ECDH.derive(alice.privateKey, bob.publicKey);
const s2 = ECDH.derive(bob.privateKey, alice.publicKey);
print("ecdh:", s1.length, s1.join(",") === s2.join(","));
```

### Ed25519

**`Ed25519Generate() -> { privateKey, publicKey }`**

No parameters. Returns a raw 32-byte private key (RFC 8032 seed) and a raw 32-byte encoded public point.

**`Ed25519Sign(privateKey, message) -> Uint8Array`**

- `privateKey` *(Uint8Array)* — must be exactly 32 bytes; anything else is refused.
- `message` *(string | Uint8Array)* — the message to sign.

Returns a 64-byte signature over the whole message. Ed25519 is one-shot by construction.

**`Ed25519Verify(publicKey, message, signature) -> boolean`**

- `publicKey` *(Uint8Array)* — must be exactly 32 bytes; a wrong-size key throws (a caller error).
- `message` *(string | Uint8Array)* — the signed message.
- `signature` *(Uint8Array)* — the signature to check.

Returns true when the signature is valid. A wrong-size signature returns false, indistinguishable from any other forgery.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { Ed25519Generate, Ed25519Sign, Ed25519Verify } from "dyna:crypto";
const k = Ed25519Generate();
const sig = Ed25519Sign(k.privateKey, "hello");
print("ed25519:", k.publicKey.length, Ed25519Verify(k.publicKey, "hello", sig));
```

**`Ed25519PemToRaw(pem) -> { privateKey?, publicKey }`** / **`Ed25519PemFromRaw({ privateKey?, publicKey }) -> { privateKey?, publicKey }`**

The bridge between the module's 32-byte raw keys and the PEM world (PKCS#8 `-----BEGIN PRIVATE KEY-----`, SPKI `-----BEGIN PUBLIC KEY-----`) — byte-exact against `openssl genpkey -algorithm ed25519` output, verified in both directions. A private PEM yields BOTH raw halves (the public point is derivable); a public PEM yields `publicKey` only. `PemFromRaw` writes exactly the halves it is given, as PEM strings. `X25519PemToRaw`/`X25519PemFromRaw` are the identical pair for X25519.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { Ed25519Generate, Ed25519PemFromRaw, Ed25519PemToRaw } from "dyna:crypto";
const k = Ed25519Generate();
const pemPair = Ed25519PemFromRaw(k);          // store these in a config file
const back = Ed25519PemToRaw(pemPair.privateKey);
print("pem:", pemPair.privateKey.startsWith("-----BEGIN PRIVATE KEY"),
      back.publicKey.length);                  // true 32
```

### X25519

**`X25519Generate() -> { privateKey, publicKey }`**

No parameters. Returns raw 32-byte keys for X25519 (curve25519) key agreement.

**`X25519Derive(privateKey, peerPublicKey) -> Uint8Array`**

- `privateKey` *(Uint8Array)* — exactly 32 bytes.
- `peerPublicKey` *(Uint8Array)* — exactly 32 bytes.

Returns the 32-byte shared secret. A small-order peer point is refused; OpenSSL reports the derivation as a failure rather than handing back an all-zero secret.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { X25519Generate, X25519Derive } from "dyna:crypto";
const a = X25519Generate(), b = X25519Generate();
const d1 = X25519Derive(a.privateKey, b.publicKey);
const d2 = X25519Derive(b.privateKey, a.publicKey);
print("x25519:", d1.length, d1.join(",") === d2.join(","));
```

`X25519PemToRaw`/`X25519PemFromRaw` are the PEM bridge, same shapes as the Ed25519 pair above.

### AESGCM

**`new AESGCM(key)`**

- `key` *(Uint8Array)* — 16, 24 or 32 bytes (AES-128/192/256-GCM); anything else is refused.

Binds the key once. AES-NI / ARMv8 crypto extensions are selected by the backend at runtime.

**`AESGCM.seal(nonce, plaintext, aad?) -> Uint8Array`**

- `nonce` *(Uint8Array)* — exactly 12 bytes; anything else is refused.
- `plaintext` *(string | Uint8Array)* — the data to encrypt.
- `aad` *(string | Uint8Array, optional)* — additional authenticated data.

Returns the ciphertext with the 16-byte tag appended, so the output is `plaintext.length + 16` bytes. The nonce is caller-supplied and never tracked — the class keeps no counter and draws nothing from the entropy source — so uniqueness under one key is entirely the caller's responsibility. Reusing a nonce under one key breaks GCM's authentication outright (auth-key recovery, and the same collapse for ChaCha20-Poly1305), so use a fresh 12-byte value per message: 12 random bytes stay safe to about 2^32 messages per key by the birthday bound, and a counter-derived nonce is the standard pattern.

**`AESGCM.sealRandom(plaintext, aad?) -> { nonce, sealed }`**

- `plaintext` *(string | Uint8Array)* — the data to encrypt.
- `aad` *(string | Uint8Array, optional)* — additional authenticated data.

Draws a fresh 12-byte nonce from the entropy source, seals with it, and returns `{ nonce: Uint8Array(12), sealed: Uint8Array }` — the pair a caller must store together is produced together, so nonce reuse (which breaks GCM's authentication outright) becomes the thing you have to go out of your way to do. `open(nonce, sealed, aad)` reads the result back. Random 96-bit nonces stay safe to about 2^32 messages per key by the birthday bound; rotate keys at that scale. An entropy-source failure throws rather than sealing under a repeated or predictable nonce.

**`AESGCM.open(nonce, sealed, aad?) -> Uint8Array`**

- `nonce` *(Uint8Array)* — exactly 12 bytes.
- `sealed` *(Uint8Array)* — the ciphertext with its tag.
- `aad` *(string | Uint8Array, optional)* — the additional authenticated data used at seal time.

Returns the plaintext. A forged tag (or a wrong nonce/aad) throws `authentication failed` rather than returning a boolean, so a caller cannot consume the plaintext of a forged message. A sealed message shorter than its tag is refused.

**`AESGCM.sealInto(out, nonce, plaintext, aad?, opts?) -> number`** / **`AESGCM.openInto(out, nonce, sealed, aad?, opts?) -> number`**

The `Into` forms: the CALLER owns `out` (a Uint8Array; a subarray writes its own slice), the callee never allocates, and the return is the byte count. A `RangeError` is thrown before any byte is written when `out.byteLength` is short of the required size.

- `opts.tagOut` *(Uint8Array, sealInto)* — detached-tag layout: the 16-byte tag lands there and `out` holds pure ciphertext. `openInto`'s counterpart is `opts.tag`, reading the tag from that buffer (`sealed` is pure ciphertext).
- `opts.prefixNonce` *(boolean)* — the one-blob wire layout: `out = nonce||ciphertext||tag` (and `openInto` reads the leading nonce from the blob; pass `undefined` as the nonce argument).

`openInto` throws `authentication failed` exactly like `open`, and the plaintext already written into `out` is zeroed first. An options object at the aad position (a shifted-argument bug) is refused, not stringified into a silent AAD change.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { AESGCM } from "dyna:crypto";
const key = new Uint8Array(32).fill(7);
const nonce = new Uint8Array(12).fill(1);
const aes = new AESGCM(key);
try {
    const sealed = aes.seal(nonce, "top secret", "aad");
    const plain = aes.open(nonce, sealed, "aad");
    print("aesgcm:", new TextDecoder().decode(plain), sealed.length);
    const tampered = sealed.slice();
    tampered[tampered.length - 1] ^= 1;
    try { aes.open(nonce, tampered, "aad"); print("unexpected"); }
    catch (e) { print("forged tag refused:", e.message); }

    // the Into forms: caller-owned output, detached tag, one-blob layout
    const out = new Uint8Array(64);
    const n = aes.sealInto(out, nonce, "top secret", "aad"); // n = bytes written
    const back = new Uint8Array(16);
    const m = aes.openInto(back, nonce, out.subarray(0, n), "aad");
    print("into:", n, new TextDecoder().decode(back.subarray(0, m)));
    const ct = new Uint8Array(64), tag = new Uint8Array(16);
    const d = aes.sealInto(ct, nonce, "detached", undefined, { tagOut: tag });
    print(d, tag.length);                                    // 8 16
} finally { aes.close(); }
```

### ChaCha20Poly1305

**`new ChaCha20Poly1305(key)`**

- `key` *(Uint8Array)* — exactly 32 bytes (ChaCha20's key size); anything else is refused.

`seal`, `open`, `sealInto`, `openInto`, `close`, `dispose` and `closed` behave exactly as `AESGCM`'s, with the same 12-byte nonce, 16-byte tag, and throw-on-forgery rule. `sealRandom(plaintext, aad?)` is also inherited in shape: a fresh CSPRNG 12-byte nonce per message, returned as `{ nonce, sealed }`, safe to about 2^32 messages per key.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { ChaCha20Poly1305 } from "dyna:crypto";
const chacha = new ChaCha20Poly1305(new Uint8Array(32).fill(9));
const nonce = new Uint8Array(12).fill(2);
try {
    const sealed = chacha.seal(nonce, "data");
    print("chacha20poly1305:", chacha.open(nonce, sealed).length);
} finally { chacha.close(); }
```

### Hmac

**`new Hmac(algorithm, key)`**

- `algorithm` *(string)* — `"md5"`, `"sha1"`, `"sha224"`, `"sha256"`, `"sha384"` or `"sha512"`; unknown names are refused.
- `key` *(string | Uint8Array)* — the MAC key.

Derives the block-sized key schedule once; every later `sign`/`verify` reuses it.

**`Hmac.sign(msg) -> Uint8Array`** / **`Hmac.signHex(msg) -> string`**

- `msg` *(string | Uint8Array)* — the message.

Returns a complete MAC; the object is ready for the next message (finalise resets).

**`Hmac.update(msg) -> this`**

- `msg` *(string | Uint8Array)* — data to absorb.

Streaming absorb for a message not held in memory. Returns `this`.

**`Hmac.digest() -> Uint8Array`** / **`Hmac.digestHex() -> string`**

No parameters. Finish the accumulated stream.

**`Hmac.reset() -> Hmac`**

Returns the HMAC to its initial state, discarding any `update()` prefix. The key schedule needs no re-derivation — init compressed it once into two chaining states, and reset re-primes from those. `digest()` and `sign()` already reset on their own; `reset()` is for abandoning a half-absorbed stream. (Streaming *verification* is deliberately not offered: `digest()` + `TimingSafeEqual` is the constant-time pattern.)

**`Hmac.verify(msg, tag) -> boolean`**

- `msg` *(string | Uint8Array)* — the message.
- `tag` *(string | Uint8Array)* — raw bytes or a hex string of twice the digest length.

Returns true when the MAC matches, compared in constant time. Use this instead of `signHex(m) === tag`, which compares with an early exit.

**Properties**

`algorithm` — the bound hash. `digestSize` — its digest size.

**`Hmac.close()` / `Hmac.dispose()`**

No parameters. Zero the key schedule at a moment you choose; `closed` is true afterwards.

```js
import { Hmac } from "dyna:crypto";
const h = new Hmac("sha256", "key");
h.update("d").update("ata");
const tag = h.digestHex();
print("hmac:", tag, h.verify("data", tag), h.algorithm, h.digestSize);
h.close();
```

### HMAC & HMACHex

**`HMAC(algorithm, key, data) -> Uint8Array`** / **`HMACHex(algorithm, key, data) -> string`**

- `algorithm` *(string)* — the same names and refusal rules as `Hmac`.
- `key` *(string | Uint8Array)* — the MAC key.
- `data` *(string | Uint8Array)* — the message.

Returns the one-shot HMAC; `HMACHex` returns the same digest as lowercase hex.

```js
import { HMAC, HMACHex } from "dyna:crypto";
const mac = HMAC("sha256", "key", "data");
print("hmac-one-shot:", mac.length, HMACHex("sha256", "key", "data").length);
```

### HKDF

**STRICT options:** the bag is `{key, hash, salt, info, length}`; an unknown key throws (before, a typo'd key silently ran at that parameter's default).

**`HKDF({ hash, key, salt, info, length }) -> Uint8Array`**

- `hash` *(string, optional, default "sha256")* — accepts the same names as `Hmac`.
- `key` *(string | Uint8Array)* — the input keying material.
- `salt` *(string | Uint8Array, optional, default "")* — the salt.
- `info` *(string | Uint8Array, optional, default "")* — context information.
- `length` *(number, optional, default 32)* — the output length, 1..1048576.

Returns the RFC 5869 extract-and-expand key. Options are named so a key and a salt cannot be silently swapped.

```js
import { HKDF } from "dyna:crypto";
const dk = HKDF({ key: "input key material", salt: "salt", info: "context", length: 32 });
print("hkdf:", dk.length);
```

### PBKDF2

**STRICT options:** the bag is `{password, hash, salt, iterations, length}`; an unknown key throws.

**`PBKDF2({ hash, password, salt, iterations, length }) -> Uint8Array`**

- `hash` *(string, optional, default "sha256")* — the PRF hash algorithm.
- `password` *(string | Uint8Array)* — the password.
- `salt` *(string | Uint8Array, optional, default "")* — the salt.
- `iterations` *(number, optional, default 100000)* — 1..67108864.
- `length` *(number, optional, default 32)* — the output length in bytes.

Returns the RFC 8018 derived key. The product `iterations * output-blocks` is capped at 2^24 so a runaway call cannot be demanded for free. The OWASP-2026 recommendation (600k iterations for HMAC-SHA256) sits well inside the cap.

```js
import { PBKDF2 } from "dyna:crypto";
const key = PBKDF2({ hash: "sha256", password: "pw", salt: "salt", iterations: 10000, length: 32 });
print("pbkdf2:", key.length);
```

### Scrypt

**STRICT options:** the bag is `{N, r, p, keyLen}`; an unknown key throws (a typo'd `Nn` used to silently run at the default 16384).

**`Scrypt(password, salt, { N, r, p, keyLen }?) -> Uint8Array`**

- `password` *(string | Uint8Array)* — the password.
- `salt` *(string | Uint8Array)* — the salt.
- `N` *(number, optional, default 16384)* — a power of 2 in 2..2^20.
- `r` *(number, optional, default 8)* — 1..64.
- `p` *(number, optional, default 1)* — 1..32.
- `keyLen` *(number, optional, default 32)* — 1..2^20.

Returns the RFC 7914 derived key. The memory product `128*N*r` is capped at 1 GiB.

<!-- check:skip: needs CONFIG_TLS=y -->
```js
import { Scrypt } from "dyna:crypto";
const dk = Scrypt("password", "salt");
print("scrypt:", dk.length);
```

### RandomBytes

**`RandomBytes(count = 32) -> Uint8Array`**

- `count` *(number, optional, default 32)* — bytes of OS entropy, 0..16777216.

Returns fresh OS entropy. This is the CSPRNG path, not the seeded PRNG of `dyna:random`; mixing the two up is a real failure mode, since the PRNG is reproducible by design.

```js
import { RandomBytes } from "dyna:crypto";
const nonce = RandomBytes(12);
print("randomBytes:", nonce.length);
```

### TimingSafeEqual

**`TimingSafeEqual(a, b) -> boolean`**

- `a` *(Uint8Array)* — the first value.
- `b` *(Uint8Array)* — the second value.

Returns true when the values are equal. The comparison is constant-time: the accumulator runs over the whole input, so the position of the first differing byte does not leak. Different lengths return false (a MAC's length is public).

```js
import { RandomBytes, TimingSafeEqual } from "dyna:crypto";
const nonce = RandomBytes(12);
print("timingSafeEqual:", TimingSafeEqual(nonce, nonce));
```

### HOTPGenerate & TOTPGenerate

**`HOTPGenerate(secret, counter, { digits, algo }?) -> string`**

- `secret` *(string | Uint8Array)* — the shared secret.
- `counter` *(number | bigint)* — the moving counter.
- `digits` *(number, optional, default 6)* — 6..8.
- `algo` *(string, optional, default "sha1")* — accepts the `Hmac` names.

Returns the RFC 4226 HMAC-based one-time password with dynamic truncation.

**`TOTPGenerate(secret, { atSec, period, digits, algo }?) -> string`**

- `secret` *(string | Uint8Array)* — the shared secret.
- `atSec` *(number)* — the time in seconds; explicit rather than read from the clock so the result is testable against RFC vectors. `atSec >= 0` is enforced.
- `period` *(number, optional, default 30)* — the time step; `period > 0` is enforced.
- `digits` *(number, optional, default 6)* — 6..8.
- `algo` *(string, optional, default "sha1")* — accepts the `Hmac` names.

Returns the RFC 6238 one-time password; the counter is `atSec / period`.

```js
import { HOTPGenerate, TOTPGenerate, HOTPVerify, TOTPVerify } from "dyna:crypto";
print("hotp:", HOTPGenerate("secret", 0));
print("totp:", TOTPGenerate("secret", { atSec: 59, period: 30 }));
```

### HOTPVerify & TOTPVerify

**`HOTPVerify(secret, counter, code, { digits, algo }?) -> boolean`**

- `secret` *(string | Uint8Array)* — the shared secret.
- `counter` *(number | bigint)* — the counter the code should be for; negative counters are refused.
- `code` *(string)* — the code under test.
- Same `digits`/`algo` options as `HOTPGenerate`.

Recomputes the expected code and compares constant-time. A malformed code — wrong length, non-digits — is `false`, the same answer a wrong-but-well-formed guess gets, because codes arrive from users and networks and a thrown error there would turn attacker input into a caller-facing exception.

**`TOTPVerify(secret, code, { atSec, period, digits, algo, window }?) -> boolean`**

- `atSec` *(number, optional, default 0)* — explicit, as in `TOTPGenerate`.
- `window` *(number, optional, default 0)* — clock-skew tolerance in periods: every counter `atSec/period - window .. + window` is computed and compared (each comparison constant-time; the window is public configuration, so recomputing it is not a timing leak). `window >= 0`, capped at 1000000 periods.

```js
import { HOTPGenerate, HOTPVerify, TOTPGenerate, TOTPVerify } from "dyna:crypto";
const code = HOTPGenerate("secret", 7);
print("hotp verify:", HOTPVerify("secret", 7, code), HOTPVerify("secret", 8, code)); // true false
const tcode = TOTPGenerate("secret", { atSec: 1000 });
print("totp skew:", TOTPVerify("secret", tcode, { atSec: 1030 }),       // false — beyond one period
      TOTPVerify("secret", tcode, { atSec: 1030, window: 1 }));         // true — window accepts it
print("garbage:", TOTPVerify("secret", "12ab", { atSec: 1000 }));       // false, not a throw
```

### JWTSign & JWTVerify

**`JWTSign(payload, key, { alg }?) -> string`**

- `payload` *(object)* — the claims to sign.
- `key` *(string)* — the secret or PEM key. `HS256`/`HS384`/`HS512` take any secret; `RS256`/`RS384`/`RS512` take a PEM private key, and `ES256`/`ES384`/`ES512` take a PEM EC private key.
- `alg` *(string, optional, default "HS256")* — the signing algorithm.

Returns a signed JWT. ES signatures are emitted as raw `R||S` (JWS form), never DER.

**`JWTVerify(token, key, { algorithms }) -> payload`**

- `token` *(string)* — the JWT to verify.
- `key` *(string)* — the secret or PEM key.
- `algorithms` *(string[])* — a required allowlist of accepted algorithms.

Returns the decoded payload object. The header's `alg` is only read to check it against the list, and the key is never chosen by the token. `alg:none`, an unlisted alg, a malformed token, or a bad signature each throw a TypeError naming the check.

```js
import { JWTSign, JWTVerify } from "dyna:crypto";
const token = JWTSign({ sub: "123" }, "shared secret");
print("jwt:", JWTVerify(token, "shared secret", { algorithms: ["HS256"] }).sub);
```

---

# dyna:serialize

Binary wire formats over plain JS values: a protobuf wire codec with dynamic schemas, a canonical ASN.1 DER codec, MessagePack and RFC 8949 CBOR sharing one graph walker, plus a value hash and a cycle-aware deep clone.

`import { Proto, ASN1, MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode, CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";`

### Proto

**`Proto.encode(value, schema) -> Uint8Array`**

- `value` *(object)* — the value to encode.
- `schema` *(object)* — `{ fields: [{ name, number, type, ... }] }`, parsed per call. Field types are `int32 int64 uint32 uint64 sint32 sint64 fixed32 sfixed32 fixed64 sfixed64 float double bool string bytes enum message`, with `repeated` (packable types pack by default), `packed`, `map` (`keyType`/`valueType`), and `message` (nested schema).

Returns the protobuf wire encoding. `undefined`/`null` values are skipped; empty collections encode to nothing. Encoding is strict: wrong JS types and out-of-range numbers are refused, and 64-bit values must be exactly representable as JS doubles.

**`Proto.decode(bytes, schema) -> object`**

- `bytes` *(Uint8Array)* — the wire bytes.
- `schema` *(object)* — the same descriptor shape as `encode`.

Returns the decoded object. The decoder is the untrusted surface: every declared length is validated against the remaining input before any allocation, nesting is capped at depth 64, and field numbers are 1..2^29-1. Absent singular fields decode to `undefined`, `repeated` to `[]`, maps to `{}`. A known field arriving with the wrong wire type is treated as an unknown field (preserved, never misread), and unknown records are kept in a hidden `__protoUnknown` array and re-emitted by the next encode.

```js
import { Proto } from "dyna:serialize";
const schema = {
    fields: [
        { name: "id", number: 1, type: "int32" },
        { name: "name", number: 2, type: "string" }
    ]
};
const encoded = Proto.encode({ id: 42, name: "test" }, schema);
const decoded = Proto.decode(encoded, schema);
print("proto:", encoded.length, JSON.stringify(decoded));
```

### ASN1

A canonical DER codec. Builders return node objects `{ cls, tag, constructed, value }`; `encode` emits canonical DER (shortest length form, minimal INTEGER, BOOLEAN as 0x00/0xFF), so re-encoding a canonical decode is byte-identical. `decode` is the untrusted surface: a declared length is checked against the enclosing content before anything is allocated, BER indefinite lengths and every non-minimal encoding are refused with a named reason, and an INTEGER wider than 8 bytes is refused.

**`ASN1.encode(node) -> Uint8Array`**

- `node` *(object)* — a node object `{ cls, tag, constructed, value }`.

Returns canonical DER, so re-encoding a canonical decode is byte-identical.

**`ASN1.decode(bytes) -> node`**

- `bytes` *(Uint8Array | ArrayBuffer)* — the DER bytes.

Returns the parsed node. A declared length is checked against the enclosing content before anything is allocated. BER indefinite lengths and every non-minimal encoding are refused with a named reason, and an INTEGER wider than 8 bytes is refused.

**`ASN1.seq(children) -> node`** / **`ASN1.set(children) -> node`**

- `children` *(node[])* — an array of child nodes.

Returns a SEQUENCE or SET node over the children.

**`ASN1.int(value) -> node`**

- `value` *(number | bigint)* — the integer value.

Returns an INTEGER node whose content is the minimal two's complement (X.690 8.3.2/10.4). A Number must be an integer within the int64 range; a BigInt of any magnitude up to the shared 64-content-byte cap is exact — removed the old int64 wall, so `2**64n`, RSA-sized values and their negatives encode and decode without wrapping (a decode answers a Number up to 2^53 and an exact BigInt beyond; a value needing more than 64 content octets refuses by name, both directions).

```js
import { ASN1 } from "dyna:serialize";
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
print(hex(ASN1.encode(ASN1.int(128))));            // 02020080
print(hex(ASN1.encode(ASN1.int(2n ** 64n))));      // 0209010000000000000000
print(hex(ASN1.encode(ASN1.int(-(2n ** 64n)))));   // 0209ff0000000000000000
print(ASN1.decode(ASN1.encode(ASN1.int(2n ** 64n))).value === 2n ** 64n); // true
```

**`ASN1.bool(value) -> node`**

- `value` *(boolean)* — the boolean value.

Returns a BOOLEAN node.

**`ASN1.null() -> node`**

No parameters. Returns a NULL node.

**`ASN1.octets(bytes) -> node`**

- `bytes` *(Uint8Array)* — the byte content.

Returns an OCTET STRING node.

**`ASN1.bitString(bytes, unused) -> node`**

- `bytes` *(Uint8Array)* — the bit content.
- `unused` *(number)* — the count of unused bits, 0..7.

Returns a BIT STRING node. DER requires those trailing bits to be zero.

**`ASN1.oid(str) -> node`**

- `str` *(string)* — an object identifier as a dotted string.

Returns an OBJECT IDENTIFIER node.

**`ASN1.utf8(str) -> node`** / **`ASN1.printable(str) -> node`** / **`ASN1.ia5String(str) -> node`** / **`ASN1.bmpString(str) -> node`** / **`ASN1.universalString(str) -> node`**

- `str` *(string)* — the string value.

Returns a UTF8String, PrintableString, IA5String (tag 22), BMPString (tag 30) or UniversalString (tag 28) node. The constructors are shallow — the string rules are enforced at ENCODE, the single choke point: IA5String is ASCII-only; BMPString encodes UCS-2 big-endian and refuses code points above U+FFFF; UniversalString encodes UCS-4 big-endian; and all of them refuse a lone surrogate by name (the engine's string→UTF-8 boundary would otherwise emit WTF-8, which is invalid DER — UTF8String now refuses it too, matching what its decoder has always done). The DECODERS interpret tags 22/28/30, so a decoded tree re-encodes byte-identically.

```js
import { ASN1 } from "dyna:serialize";
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
print(hex(ASN1.encode(ASN1.ia5String("hello"))));        // 160568656c6c6f
print(hex(ASN1.encode(ASN1.bmpString("héllo"))));       // 1e0a006800e9006c006c006f
print(hex(ASN1.encode(ASN1.universalString("Aé😀"))));  // 1c0c00000041000000e90001f600
print(ASN1.decode(ASN1.encode(ASN1.bmpString("héllo"))).value); // héllo
```

**`ASN1.utcTime(str) -> node`** / **`ASN1.generalizedTime(str) -> node`**

- `str` *(string)* — the time string.

Returns a UTCTime or GeneralizedTime node (the strings are emitted verbatim).

**`ASN1.context(tag, content) -> node`**

- `tag` *(number)* — the context-specific tag.
- `content` *(Uint8Array)* — the byte content.

Returns a context-specific primitive (`[0]`-style) node.

**`ASN1.contextC(tag, children) -> node`**

- `tag` *(number)* — the context-specific tag.
- `children` *(node[])* — the child nodes.

Returns a context-specific constructed node over the children.

```js
import { ASN1 } from "dyna:serialize";
const der = ASN1.encode(ASN1.seq([ ASN1.int(12345), ASN1.utf8("hi") ]));
const parsed = ASN1.decode(der);
print("asn1:", parsed.value.length, parsed.value[0].value, parsed.value[1].value);
```

### MessagePack & CBOR

**`MsgPackEncode(value) -> Uint8Array`**

- `value` *(any)* — the value to encode.

Returns the MessagePack encoding. Encoding refuses symbols and functions.

**`MsgPackDecode(bytes) -> value`**

- `bytes` *(Uint8Array)* — the MessagePack bytes.

Returns the decoded value. Decoding bounds every declared length against the remaining input before allocating, rejects trailing bytes, and caps nesting at depth 256.

**`CBOREncode(value) -> Uint8Array`**

- `value` *(any)* — the value to encode.

Returns the RFC 8949 CBOR encoding, with the same walker, bounds and refusals as MessagePack.

**`CBORDecode(bytes) -> value`**

- `bytes` *(Uint8Array)* — the CBOR bytes.

Returns the decoded value, with the same bounds and refusals as `MsgPackDecode`.

**`CBORCanonical(value) -> Uint8Array`**

- `value` *(any)* — the value to encode.

Returns CBOR with map keys sorted byte-wise, a deterministic form usable for signing or hashing.

```js
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode, CBORCanonical } from "dyna:serialize";
const packed = MsgPackEncode({ a: 1, b: "x", c: [1, 2, 3] });
print("msgpack:", MsgPackDecode(packed).c.length);
const cbor = CBOREncode({ a: 1, b: "x" });
print("cbor:", CBORDecode(cbor).a, CBORCanonical({ b: 2, a: 1 }).length);
```

### ValueHash

**`ValueHash(value) -> string`**

- `value` *(any)* — the value to hash.

Returns a canonical CBOR encoding of the value run through XXH64, as 16 lowercase hex chars. Map-key order never matters: `ValueHash({ a: 1, b: 2 })` equals `ValueHash({ b: 2, a: 1 })`.

```js
import { ValueHash } from "dyna:serialize";
print("valueHash:", ValueHash({ a: 1, b: 2 }) === ValueHash({ b: 2, a: 1 }));
```

### structuredClone

**`structuredClone(value) -> value`**

- `value` *(any)* — the value to clone.

Returns a deep clone of plain JS data: objects, arrays, typed arrays and ArrayBuffers (copied, never aliased), and primitives including BigInt. Cycles and shared references survive (the clone memo preserves identity), so `cl.self === cl` holds. `Date`, `RegExp`, `Map` and `Set` instances are preserved as their own kinds (verified: `structuredClone({d: new Date(0)}).d instanceof Date === true`); functions and accessor properties are refused.

Edge semantics for byte-backed values (pinned by `tests/test_clone_views.js`):

- **Kind identity.** A typed array clones as its intrinsic kind (width AND signedness) and a DataView as a DataView. Kind identification and reconstruction are engine-internal, resolved once per process from the engine's own constructors: an instance `constructor` or prototype spoof, a swapped global intrinsic (`globalThis.Uint8Array = Uint16Array`) and an outright `delete globalThis.DataView` cannot re-route a clone's kind (the clone's `constructor` stays the real intrinsic even with the global gone).
- **Proxy views.** A `Proxy` around a typed array or DataView is not byte-backed to the engine; it clones as a **plain object** over the proxy's own enumerable properties (WHATWG `structuredClone` would carry the underlying buffer).
- **Detached buffers.** A detached `ArrayBuffer` has no bytes to copy and clones to `{}` (a documented cut; WHATWG `structuredClone` throws `DataCloneError` here).
- **Per-byte-window copies.** Several views over ONE buffer clone into **independent** windows: the clones do not share a buffer (WHATWG `structuredClone` shares one cloned buffer across the views). Each window's bytes are copied exactly; only the buffer-sharing relationship is not preserved.

```js
import { structuredClone } from "dyna:serialize";
const doc = { x: 1 };
doc.self = doc;
const cl = structuredClone(doc);
print("clone:", cl.x, cl.self === cl);
```

---

# dyna:compress

Codecs and archive containers: zstd, brotli, snappy, LZ4 (raw block and frame), gzip, and ustar tar / zip packing and extraction. Every function takes a string, TypedArray or ArrayBuffer and returns a fresh Uint8Array (or a string with `{ asString: true }`); decompression output is capped to reject bombs, and invalid input is refused with a named error.

Every options bag in this module is strict: an unknown option key throws a `TypeError` naming the key and the valid set (`unknown option "levle" (valid: level)`), instead of being silently ignored.

`import { zstd, unzstd, brotli, unbrotli, snappy, unsnappy, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, gzip, gunzip, compressBound, TarPack, TarList, TarExtract, ZipPack, ZipList, ZipRead, ZipReadAt, ZipExtractAll, deflate, inflate, Compressor, Dictionary } from "dyna:compress";`

### compressBound

**`compressBound(n, { algo }) -> number`**

- `n` *(number)* — the input size in bytes; negative throws `RangeError`.
- `algo` *(string)* — `"gzip" | "lz4" | "lz4frame" | "zstd" | "brotli" | "snappy"`.

The worst-case compressed size of `n` input bytes under one codec, so a caller-owned output buffer can be allocated once instead of growing. Each formula is the bound the codec's own encoder reserves, not a generic guess: gzip is the RFC 1952 framing around its stored-block fallback, lz4 the raw-block bound (`n + n/255 + 16`), lz4frame adds the frame headers and per-4 MiB-block size fields (including the optional content checksum), zstd/snappy/brotli report the linked codec's own bound (`ZSTD_compressBound`, the vendored snappy cap, `BrotliEncoderMaxCompressedSize`). The bound always fits the codec's output on any input — but for `gzip`/`lz4`/`lz4frame` the encoder ALSO never expands, so typical output is far smaller.

```js
import { compressBound, zstd } from "dyna:compress";
const buf = new Uint8Array(compressBound(1 << 20, { algo: "zstd" }));  // room for 1 MiB of worst case
```

### zstd & unzstd

**`zstd(data, { level }?) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input.
- `level` *(number, optional, default 3)* — the compression level, 1..22.

Returns Zstandard output via the linked libzstd.

**`unzstd(data, { asString }?) -> Uint8Array | string`**

- `data` *(string | TypedArray | ArrayBuffer)* — the compressed input.
- `asString` *(boolean, optional)* — return the decoded output as a string.

Returns the decompressed data. Malformed or oversized input is refused.

```js
import { zstd, unzstd } from "dyna:compress";
const input = new Uint8Array([1, 2, 3, 4, 5, 1, 2, 3, 4, 5]);
const compressed = zstd(input);
const decompressed = unzstd(compressed);
print("zstd:", compressed.length, decompressed.join(",") === input.join(","));
```

### brotli & unbrotli

**`brotli(data, { level }?) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input.
- `level` *(number, optional, default 5)* — the compression level, 0..11.

Returns Brotli output (libcompression on macOS).

**`unbrotli(data, { asString }?) -> Uint8Array | string`**

- `data` *(string | TypedArray | ArrayBuffer)* — the compressed input.
- `asString` *(boolean, optional)* — return the decoded output as a string.

Returns the decompressed data. Malformed or oversized input is refused.

```js
import { brotli, unbrotli } from "dyna:compress";
const b = brotli("hello hello", { level: 9 });
print("brotli:", unbrotli(b, { asString: true }));
```

### snappy & unsnappy

**`snappy(data) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input.

Returns the Snappy block format (vendored, no level).

**`unsnappy(data, { asString }?) -> Uint8Array | string`**

- `data` *(string | TypedArray | ArrayBuffer)* — the compressed input.
- `asString` *(boolean, optional)* — return the decoded output as a string.

Returns the decompressed data. A length prefix beyond the input is refused.

```js
import { snappy, unsnappy } from "dyna:compress";
const input = new Uint8Array([1, 2, 3, 4, 5, 1, 2, 3, 4, 5]);
print("snappy:", unsnappy(snappy(input)).length);
```

### lz4Compress & lz4Decompress

**`lz4Compress(data, { level, dict }?) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input.
- `level` *(number, optional, default 1)* — the compression level, 1..12.
- `dict` *(Uint8Array)* — seeds the match window.

Returns a raw LZ4 block: no header, length or checksum; the caller owns the framing, which is what a message bus wants.

**`lz4Decompress(data, { dict }?) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the raw block.
- `dict` *(Uint8Array, optional)* — the same dictionary used at compress time.

Returns the decompressed data. Invalid blocks are refused.

```js
import { lz4Compress, lz4Decompress } from "dyna:compress";
const input = new Uint8Array([1, 2, 3, 4, 5, 1, 2, 3, 4, 5]);
const block = lz4Compress(input);
print("lz4:", lz4Decompress(block).length);
```

### lz4Frame & lz4Unframe

**`lz4Frame(data, { level, checksum }?) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input.
- `level` *(number, optional, default 1)* — the compression level.
- `checksum` *(boolean, optional, default true)* — include the content checksum.

Returns the LZ4 frame format (magic, descriptor, block sizes, optional content checksum), which the `lz4` command line reads and writes.

**`lz4Unframe(data, { asString }?) -> Uint8Array | string`**

- `data` *(string | TypedArray | ArrayBuffer)* — the frame.
- `asString` *(boolean, optional)* — return the decoded output as a string.

Returns the decompressed data. A frame with a bad checksum or structure is refused.

```js
import { lz4Frame, lz4Unframe } from "dyna:compress";
const input = new Uint8Array([1, 2, 3, 4, 5, 1, 2, 3, 4, 5]);
const frame = lz4Frame(input);
print("lz4frame:", lz4Unframe(frame).length);
```

### gzip & gunzip

**`gzip(data, level?) -> Uint8Array`** / **`gzip(data, { level }?) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input.
- `level` *(number, optional)* — 1..9; below 6 emits a fixed-Huffman stream, 6 and above builds dynamic-Huffman blocks from the input's own statistics (typically 5-15% smaller on text). Both arities are accepted — a bare number or an options object (`{ level: 9 }`); a non-number level throws `TypeError` either way.

Returns RFC 1952 framing (magic `1f 8b`, mtime 0) around a real DEFLATE stream — fixed or dynamic Huffman by level, whichever is smaller at that level — with a stored-block fallback so output never expands, plus the CRC-32/ISIZE trailer.

**`gunzip(data, { asString }?) -> Uint8Array | string`**

- `data` *(string | TypedArray | ArrayBuffer)* — the gzip input.
- `asString` *(boolean, optional)* — return the decoded output as a string.

Returns the decompressed data. Runs a full RFC 1951 inflate (stored/fixed/dynamic blocks), validates the trailer, and refuses invalid gzip input.

```js
import { gzip, gunzip } from "dyna:compress";
const gz = gzip("hello hello hello");
print("gzip:", gunzip(gz, { asString: true }));
```

### TarPack & TarList & TarExtract

**`TarPack(entries) -> Uint8Array`**

- `entries` *(object[])* — archive entries, each `{ name, data?, mode?, mtime? }`; an entry without `data` becomes a directory.

Returns a ustar archive. Names must be safe (no `..` segments, no leading `/`, no drive letters; refused otherwise) and fit ustar's 100+155 byte split; sizes and times must not be negative.

**`TarList(bytes, { allowUnsafeNames }?) -> object[]`**

- `bytes` *(string | TypedArray | ArrayBuffer)* — the archive.
- `allowUnsafeNames` *(boolean, optional)* — lifts the safe-name check on read.

Returns the archive metadata as `{ name, size, mtime, mode, type, linkname? }`; `type` is `"file"`, `"directory"`, `"symlink"`, `"link"`, `"device"` or `"fifo"`.

**`TarExtract(bytes, { allowUnsafeNames }?) -> object[]`**

- `bytes` *(string | TypedArray | ArrayBuffer)* — the archive.
- `allowUnsafeNames` *(boolean, optional)* — lifts the safe-name check on read.

Returns the same list as `TarList` with a `data` (Uint8Array) field added to every non-directory entry. A malformed archive is refused.

```js
import { TarPack, TarExtract } from "dyna:compress";
const tar = TarPack([
    { name: "a.txt", data: new TextEncoder().encode("hello tar") },
    { name: "dir/", type: "directory" }
]);
const entries = TarExtract(tar);
print("tar:", entries.length, new TextDecoder().decode(entries[0].data));
```

### ZipPack & ZipList & ZipRead

**`ZipPack(entries, { method }?) -> Uint8Array`**

- `entries` *(object[])* — archive entries, each `{ name, data }` with a safe-name check.
- `method` *(string, optional, default "deflate")* — `"deflate"` or `"store"`; advisory for `"deflate"`: a member that deflating would not shrink is silently stored instead.

Returns a zip archive. The CRC of every member is written into the directory.

**`ZipList(bytes, { allowUnsafeNames }?) -> object[]`**

Archives using ZIP64 extensions (required past 65535 entries or 4 GiB) are refused with a named
error — the reader does not parse the ZIP64 end-of-central-directory locator.

- `bytes` *(string | TypedArray | ArrayBuffer)* — the archive.
- `allowUnsafeNames` *(boolean, optional)* — lifts the safe-name check.

Returns a central-directory listing with `{ name, size, mtime, type, compressedSize, crc32, method }`.

**`ZipRead(bytes, name, { allowUnsafeNames }?) -> Uint8Array`**

- `bytes` *(string | TypedArray | ArrayBuffer)* — the archive.
- `name` *(string)* — the exact member name to extract.
- `allowUnsafeNames` *(boolean, optional)* — lifts the safe-name check.

Returns the member's contents. The reader handles store and deflate; a member that fails its CRC, does not inflate, or decompresses to a different size than the directory declares is refused, and an unknown member name throws.

```js
import { ZipPack, ZipList, ZipRead } from "dyna:compress";
const zip = ZipPack([{ name: "f.txt", data: new TextEncoder().encode("zip content") }]);
print("zip:", ZipList(zip)[0].name, new TextDecoder().decode(ZipRead(zip, "f.txt")));
```

**`ZipPack(entries, { method }?) -> Uint8Array` (per-entry fields)**

Each entry is `{ name, data, method?, mtime?, mode?, comment? }`: `method` (`"deflate"`/`"store"`) overrides the pack-level default per entry and is ADVISORY for `"deflate"` — when deflating would not shrink the member it is silently stored instead (the record's `method` field reports what was actually written), `mtime` is unix seconds (`0` = none, stored as DOS datetime), `mode` is a unix file mode (default `0644`, `0755` for directory names, stored in external attributes), `comment` is a central-directory comment (truncated at 1024 bytes). Unknown per-entry keys are refused.

**`ZipReadAt(bytes, index, { allowUnsafeNames }?) -> object`**

Entry by-index in central-directory order: `{ name, size, mtime, mode, comment, type, compressedSize, crc32, method, data }`. Out of range is a `RangeError`. The options bag is strict (an unknown key throws a `TypeError` naming the key and the valid set). Iteration is an array loop over `ZipList`/`ZipExtractAll`.

**`ZipExtractAll(bytes, dirOrOpts?, opts?) -> object[]`**

Every member with its bytes (same record shape as `ZipReadAt`). The destination is the second argument (a string or `Path`) or `dir` in the strict options bag `{ dir, allowUnsafeNames }`; a plain object is always the bag and anything ambiguous is refused rather than coerced. With a destination each file is also written under it (mkdir -p, subdirs created) and the same records are returned. Exactly what the write path enforces:

- unsafe member names (absolute, `..`, backslash, drive letter) are refused unless `allowUnsafeNames: true` is given EXPLICITLY — and with a destination that flag is an opt-in escape: such names write OUTSIDE `dir` (a `../` name lands in `dir`'s parent) and the caller owns the consequences. The relaxation is never implied.
- under the destination no symlink is ever traversed or written through: a symlink planted at any component (a directory link, or a final-name link to a file outside) is refused instead of followed, so nothing outside the destination is created or modified.
- extracted files are created with the process default mode (`0666` & ~umask); the member's stored `mode` is reported in its record, never applied to the file. An existing regular file is replaced by a fresh one.

```js
import { ZipPack, ZipExtractAll, ZipReadAt } from "dyna:compress";
const z2 = ZipPack([{ name: "a.txt", data: new TextEncoder().encode("hi"), mtime: 1700000000, mode: 0o644, comment: "greeting" }]);
print(ZipReadAt(z2, 0).comment);                        // greeting
print(ZipExtractAll(z2)[0].name);                       // a.txt
print(ZipExtractAll(z2, { allowUnsafeNames: false }).length); // 1: the bag form
```

### deflate & inflate (raw)

**`deflate(data, { level }?) -> Uint8Array`** / **`inflate(data, { asString }?) -> Uint8Array | string`**

Raw RFC 1951 DEFLATE without gzip framing (`level` 1..12, default 1; same encoder core as zip members, so `deflate(data)` equals the member bytes of a DEFLATED `ZipPack` entry — for incompressible data `ZipPack` falls back to STORED and the member bytes are then the raw input). `inflate` refuses trailing bytes and caps output against bombs.

```js
import { deflate, inflate } from "dyna:compress";
const raw = deflate("hello hello hello");
print("raw:", inflate(raw, { asString: true }));
```

### Compressor

**`new Compressor({ algo, level, checksum, dict })`**

- `algo` *(string)* — `"gzip"`, `"lz4"`, `"lz4frame"`, `"zstd"`, `"brotli"` or `"snappy"`.
- `level` *(number, optional)* — per-codec bounds (zstd 1..22, brotli 0..11, lz4 1..12), defaults matching the one-shot functions.
- `checksum` *(boolean, optional)* — matters only for `"lz4frame"`.
- `dict` *(Uint8Array)* — applies to `"lz4"` only; refused elsewhere.

A compiled capability: the configuration and the codec scratch are owned by the instance and reused across calls, which is the whole point of the class.

**`Compressor.compress(data) -> Uint8Array`** / **`Compressor.decompress(data, { asString }?) -> Uint8Array | string`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input or the compressed record.
- `asString` *(boolean, optional, decompress only)* — return the decoded output as a string.

Returns the compressed or decompressed data. With a dictionary, compress prefixes the record with the dictionary's CRC-32C; `decompress` throws `dictionary mismatch` when the record was written with a different dictionary, so a mismatch is never silent corruption.

**Properties**

`algo` — the configured codec name. `dictId` — the CRC-32C of the dictionary, or `null` when none is set.

**`Compressor.close()` / `Compressor.dispose()`**

No parameters. Release the scratch; `closed` is true afterwards.

```js
import { Compressor } from "dyna:compress";
const input = new Uint8Array([1, 2, 3, 4, 5, 1, 2, 3, 4, 5]);
const c = new Compressor({ algo: "zstd", level: 9 });
try {
    const packed = c.compress(input);
    print("compressor:", c.algo, c.dictId, c.decompress(packed).length);
} finally { c.close(); }
```

### Dictionary

**`new Dictionary(phrases)`**

- `phrases` *(string[])* — an array of non-empty strings.

Builds an Aho-Corasick automaton over the phrase list, compiled in the constructor. Replaces known phrases with codes, which wins when the payload is built from a fixed vocabulary.

**`Dictionary.compress(data) -> Uint8Array`** / **`Dictionary.decompress(data) -> Uint8Array`**

- `data` *(string | TypedArray | ArrayBuffer)* — the input or the compressed record.

Returns the compressed or decompressed data. The record carries no phrase list, so `decompress` throws `not a record from this dictionary` when a different dictionary is used.

**Properties**

`id` — a stable hash of the phrase list. `size` — the number of phrases.

**`Dictionary.close()` / `Dictionary.dispose()`**

No parameters. Release the automaton; `closed` is true afterwards.

```js
import { Dictionary } from "dyna:compress";
const dict = new Dictionary(["hello", "world"]);
try {
    const packed = dict.compress("hello world hello");
    print("dictionary:", dict.size, new TextDecoder().decode(dict.decompress(packed)));
} finally { dict.close(); }
```

---

# dyna:random

A seedable PRNG: xoshiro256** with a 256-bit state, seeded through splitmix64. A given seed is deterministic and reproducible; an omitted seed draws from OS entropy.

Which random when: `crypto.RandomBytes` for anything security-adjacent (the CSPRNG path); this module for reproducible seeded or fast non-security randomness; the `Array.prototype.shuffle`/`sample` extensions for a throwaway shuffle (they draw from a global RNG, not from any `Random` instance).

`import { Random } from "dyna:random";`

### Random

**`new Random(seed?)`**

- `seed` *(number | bigint, optional)* — a Number or BigInt (`42` and `42n` map to the same stream); the unseeded constructor reads OS entropy.

Constructs the generator. The object is a plain GC object (nothing scarce to release), so there is no `close()`.

**`Random.nextU64() -> BigInt`**

No parameters. Returns a full 64-bit draw, always as BigInt so no bits are lost.

**`Random.nextU53() -> Number`**

No parameters. Returns the top 53 bits as an exact Number in `[0, 2^53)`.

**`Random.nextFloat() -> Number`**

No parameters. Returns a double in `[0, 1)`.

**`Random.nextBounded(bound) -> Number | BigInt`**

- `bound` *(number | bigint)* — the exclusive upper bound. A Number bound must be an integer in `[1, 2^53]`, a BigInt bound positive; otherwise a RangeError is thrown.

Returns a value uniform in `[0, bound)` by unbiased rejection sampling. The result type mirrors the argument (`nextBounded(6n)` returns a BigInt). A Number bound must be an integer — a non-integral value like `6.5` throws; a BigInt bound may span the full u64 range (`2n**64n - 1n` works), not just the signed reach of `int64`.

**`Random.fill(typedArray, offset?, length?) -> this`**

- `typedArray` *(TypedArray)* — any byte-width typed array.
- `offset` *(number, optional, default 0)* — first ELEMENT to write (TypedArray convention, not bytes).
- `length` *(number, optional, default rest of the array)* — element count to write.

Fills the window with fresh random bytes. Both bounds are integers with `0 <= offset` and `offset + length <= elementCount`; anything else is a RangeError. Returns `this`. A refused call consumes no draws, and the window form exists so one seeded stream can populate designated regions of a large buffer reproducibly.

**`Random.bytes(n) -> Uint8Array`**

- `n` *(number)* — how many bytes; an integer in `[0, 2^30]` (1 GiB), else a RangeError. The cap is checked before any allocation, so an oversized request is refused immediately instead of allocating first and failing after.

Returns a fresh `Uint8Array` of `n` bytes from the stream. `bytes(0)` is an empty array and consumes no draws.

**`Random.normal(mu = 0, sigma = 1) -> Number`**

- `mu` *(number, optional)* — mean; any finite number.
- `sigma` *(number, optional)* — standard deviation; finite and `>= 0`, else a RangeError. `sigma = 0` is the degenerate point mass at `mu` (it returns `mu` without consuming draws).

Returns a draw from `N(mu, sigma)` by Marsaglia's polar method: rejection on the unit disk, no trigonometry in the hot path, and deliberately NO cached second normal — each call consumes pairs of uniforms until one pair is accepted, so the 32-byte `getState` format stays exact and any sequence of calls replays losslessly from a seed or a checkpoint.

**`Random.exponential(lambda = 1) -> Number`**

- `lambda` *(number, optional)* — rate; a finite number `> 0`, else a RangeError.

Returns a draw from the exponential distribution by inverse CDF: `-log(1-u)/lambda`, always `>= 0`. One draw per call.

**`Random.poisson(lambda) -> Number`**

- `lambda` *(number)* — mean; required, finite, in `[0, 2^31-1]`, else a RangeError.

Returns an exact non-negative integer from the Poisson distribution. For `lambda < 30` it is Knuth's product method; for `lambda >= 30` an exact centered inverse-CDF walk: one uniform draw, the pmf anchored at the mode through libm `lgamma` (a platform primitive — no hand-copied coefficient tables), then an `O(sqrt(lambda))` walk outward. Tails below `1e-20` absolute mass fold to the walk boundary — under the `2^-53` granularity of a uniform, so no reachable draw can change outcome.

**`Random.shuffle(arr) -> this`**

- `arr` *(Array | TypedArray)* — the collection shuffled in place.

Fisher–Yates over the INSTANCE stream: the seeded, reproducible shuffle that the global-RNG `Array.prototype.shuffle` cannot give. Exactly `len-1` draws for `len >= 2`, independent of the contents. Array holes move as holes (a shuffle never densifies); frozen or sealed collections are refused with a TypeError before any draw.

**`Random.sample(arr, n) -> Array | TypedArray`**

- `arr` *(Array | TypedArray)* — the population; never mutated.
- `n` *(number)* — how many, an integer in `[0, len]`; out of range is a RangeError.

Returns `n` elements drawn WITHOUT replacement, by partial Fisher–Yates over an index table (exactly `n` draws), so the result is a uniformly random subset in a uniformly random order. A typed array yields the same species; array holes read as `undefined`.

**`Random.choice(arr) -> value`**

- `arr` *(Array | TypedArray)* — non-empty; emptiness is a RangeError.

Returns one element at a rejection-sampled index (one draw). Holes read as `undefined`.

**`Random.jump() -> this`** / **`Random.longJump() -> this`**

No parameters. Advance the state by `2^128` (`longJump`: `2^192`) draws without generating them — the canonical xoshiro256** jump constants, verified in-tree by rebuilding the generator's `GF(2)` transition matrix and checking the tables reproduce `T^(2^128)`/`T^(2^192)` exactly (see `scratch/probe_jump.c`). Two generators seeded alike and separated by a jump emit disjoint subsequences: independent parallel streams for workers or Monte Carlo lanes, at 256 transition steps per jump. Both mutate in place and return `this`.

**`Random.getState() -> Uint8Array`**

No parameters. Returns a 32-byte snapshot of the generator state (four little-endian u64 words) — enough to resume the stream exactly where it stands. The state is opaque: write only through `setState`.

**`Random.setState(state)`**

- `state` *(Uint8Array)* — a 32-byte state captured by `getState()`; an offset subarray view is accepted. An all-zero state is the xoshiro fixed point and is refused with a RangeError.

Restores the generator to the captured point: the subsequent draws replay the recorded sequence. Useful for deterministic replays from a mid-stream checkpoint, not just from the seed.

```js
import { Random } from "dyna:random";
const r = new Random(42);
print("random:", r.nextU64(), r.nextU53(), r.nextBounded(6));
print("dists:", r.normal(10, 2).toFixed(2), r.exponential(4).toFixed(3), r.poisson(30));
const deck = [1, 2, 3, 4, 5];
r.shuffle(deck);
print("shuffled deterministically:", deck.join(","), r.sample(deck, 2).join(","), r.choice(deck));
const win = new Uint8Array(8);
print("windows:", (r.fill(win, 2, 4) === r), win.length, r.bytes(4).length);
const lane = new Random(7);
const peer = new Random(7);
peer.jump();
const seen = new Set();
for (let k = 0; k < 1000; k++) seen.add(String(lane.nextU64()));
let disjoint = true;
for (let k = 0; k < 1000; k++) if (seen.has(String(peer.nextU64()))) disjoint = false;
print("jumped lanes are disjoint:", disjoint);
const checkpoint = r.getState();
const a = r.nextU64(), b = r.nextU64();
r.setState(checkpoint);
print("replay:", r.nextU64() === a && r.nextU64() === b);
```

---

# dyna:uuid

UUID generation, parsing and inspection. Random versions draw from the OS CSPRNG, never `Math.random`v7 embeds a millisecond timestamp and a same-millisecond counter (RFC 9562 ), so ids generated later in the process sort after earlier ones; v3/v5 are deterministic hashes of `(namespace, name)`. NanoID and ULID live here too — different specs, same entropy source.

`import { v3, v4, v5, v7, v8, compare, parse, validate, version, variant, bytes, fromBytes, NanoID, NanoIDAlphabet, ULID, ULIDTime, NIL, MAX, NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500 } from "dyna:uuid";`

### v4 & v7

**`v4(opts?) -> string | Uint8Array`**

- `opts` *(object, optional)* — `{ as: "string" | "bytes" }`. Unknown keys are a TypeError and `as` values other than the two are a TypeError.

Returns a random version-4 UUID (122 random bits, version/variant bits set). With `{as: "bytes"}` it returns the 16 bytes that string would encode — the DB-key path that avoids formatting and re-parsing.

**`v7(opts?) -> string | Uint8Array`**

- `opts` *(object, optional)* — same `{as}` contract as v4.

Returns a time-ordered version-7 UUID: 48-bit millisecond timestamp, a 12-bit counter seeded randomly per millisecond and incremented on same-millisecond calls (RFC 9562 ), and 62 random bits. The timestamp is clamped to a monotonic floor, so a backwards clock step (NTP, VM restore) stalls it instead of reversing it; ids generated later in the process sort strictly after earlier ones. `{as: "bytes"}` runs the identical counter logic and returns the 16-byte form.

```js
import { v4, v7, version, variant, fromBytes, bytes } from "dyna:uuid";
const u4 = v4(), u7 = v7();
print("uuid:", u4, version(u4), variant(u4), v7() >= u7);
const b7 = v7({ as: "bytes" });
print("bytes:", v4({ as: "bytes" }).length, bytes(fromBytes(b7)).join(",") === b7.join(","));
```

### v8 & compare

**`v8(opts) -> string`**

- `opts` *(object)* — `{ fill: Uint8Array }`; the bag is strict (unknown keys TypeError), `fill` is required and must be a 16-byte view (wrong length RangeError, non-byte-view TypeError).

The RFC 9562 custom form: the caller supplies ALL 16 bytes (from a seeded PRNG, a counter, a hardware source — the entropy policy is yours), and this function overwrites EXACTLY twelve bits: the byte-6 high nibble becomes `1000` (version 8) and the byte-8 top two bits become `10` (RFC 4122 variant). The caller's remaining bits — including byte 6's low nibble and byte 8's low six bits (the RFC's csleep fields) — survive untouched, and the fill view is copied, never mutated. Version 6 stays rejected on purpose: it is a field reorder of v1 and this module has no v1.

**`compare(a, b) -> -1 | 0 | 1`**

- `a`, `b` *(string)* — UUIDs in any accepted form; a non-string or malformed string is a TypeError.

Compares the canonical 16-byte forms — exactly `memcmp` over `bytes(a)` vs `bytes(b)` — which is the order v7 ids already sort in, so a DB can key on `v7()` and index on `compare`. All four accepted string forms compare equal when they denote the same UUID.

```js
import { v8, compare, version, variant, NIL } from "dyna:uuid";
const fill = new Uint8Array(16).fill(0xab);
const u8 = v8({ fill });
print("uuid-v8:", version(u8), variant(u8), fill[6] === 0xab);
print("uuid-compare:", compare(NIL, NIL), compare(NIL, u8) < 0);
```

### v3 & v5

**`v3(namespace, name) -> string`** / **`v5(namespace, name) -> string`**

- `namespace` *(string | Uint8Array)* — a UUID string in any accepted form or a 16-byte view; anything else is refused.
- `name` *(string | Uint8Array)* — the name to hash.

Returns an MD5-based name UUID (RFC 4122) from `v3`; `v5` uses SHA-1. The same `(namespace, name)` pair always yields the same UUID.

```js
import { v3, NAMESPACE_DNS } from "dyna:uuid";
const u3 = v3(NAMESPACE_DNS, "example.com");
print("uuid-v3:", v3(NAMESPACE_DNS, "example.com") === u3, u3);
```

### parse & validate

**`parse(uuid) -> string`**

- `uuid` *(string)* — a UUID in any accepted form: canonical `8-4-4-4-12`, `urn:uuid:...`, `{...}`, or 32 raw hex (case-insensitive).

Returns the canonical lowercase string. Malformed input throws.

**`validate(value) -> boolean`**

- `value` *(any)* — the value to check.

Returns true iff the argument is a string in an accepted form. A non-string is simply false, never an error.

```js
import { v4, parse, validate } from "dyna:uuid";
const u4 = v4();
const canonical = parse("urn:uuid:" + u4);
print("uuid-parse:", canonical === u4, parse(u4.toUpperCase()) === u4);
print("uuid-validate:", validate(u4), validate("not-a-uuid"));
```

### version & variant

**`version(uuid) -> number`**

- `uuid` *(string)* — a UUID string.

Returns the version nibble (3, 4, 5 or 7 for what this module generates). Throws on a malformed string.

**`variant(uuid) -> string`**

- `uuid` *(string)* — a UUID string.

Returns the variant name: `"NCS"`, `"RFC4122"`, `"Microsoft"` or `"Future"`. Throws on a malformed string.

```js
import { v4, v7, version, variant } from "dyna:uuid";
print("uuid-version:", version(v4()), variant(v7()));
```

### bytes & fromBytes

**`bytes(uuid) -> Uint8Array`**

- `uuid` *(string)* — a UUID string.

Returns the 16 raw bytes of the parsed UUID, as a fresh copy.

**`fromBytes(bytes) -> string`**

- `bytes` *(Uint8Array)* — exactly 16 bytes; any other length is refused.

Returns the canonical UUID string.

```js
import { v4, bytes, fromBytes } from "dyna:uuid";
const u4 = v4();
print("uuid-bytes:", bytes(u4).length, fromBytes(bytes(u4)) === u4);
```

### NanoID & NanoIDAlphabet

**`NanoID(size = 21) -> string`**

- `size` *(number, optional, default 21)* — the output length, 1..4096.

Returns a URL-safe ID over the default 64-symbol alphabet (21 chars carry 126 bits), drawn by rejection sampling so no symbol is biased.

**`NanoIDAlphabet(alphabet, size = 21) -> string`**

- `alphabet` *(string)* — 2..256 ASCII symbols; multi-byte symbols are refused, since output is indexed by byte.
- `size` *(number, optional, default 21)* — the output length.

Returns the same generator over the caller-supplied alphabet.

```js
import { NanoID, NanoIDAlphabet } from "dyna:uuid";
print("nanoid:", NanoID(12), NanoIDAlphabet("AB", 8));
```

### ULID & ULIDTime

**`ULID(atMillis?, opts?) -> string`**

- `atMillis` *(number, optional)* — an explicit timestamp that must fit 48 bits. Omitted, the clock is read.
- `opts` *(object, optional)* — `{ monotonic: boolean }`; the bag is strict (unknown keys a TypeError).

Returns a 26-character Crockford base32 ULID: 48-bit millisecond timestamp then 80 bits of randomness, so ULIDs sort by time. The DEFAULT is fresh OS entropy every call, which is NOT monotonic within a millisecond. With `{monotonic: true}` the 80 bits become a counter: same-millisecond calls +1 the previous id (strictly ascending under rapid calls), each new millisecond re-seeds from entropy, and a backwards clock step or an `atMillis` below the counter's floor HOLDS the previous millisecond — the same clamp v7 uses. The monotonic state is guarded by a process-wide mutex because workers share the module's statics (one context is single-threaded, so the lock is uncontended in the common case).

**`ULIDTime(ulid) -> number`**

- `ulid` *(string)* — the ULID string.

Returns the millisecond timestamp encoded in the first 10 characters. All 26 characters must be valid Crockford base32 symbols (`0-9 A-Z` minus `I L O U`); anything else is refused.

```js
import { ULID, ULIDTime } from "dyna:uuid";
const ul = ULID();
print("ulid:", ULIDTime(ul) > 0, ULIDTime(ULID(1700000000000)) === 1700000000000);
const T = 4000000000000;
let prev = null, ascending = true;
for (let i = 0; i < 100; i++) {
    const u = ULID(T, { monotonic: true });
    if (prev !== null && !(u > prev)) ascending = false;
    prev = u;
}
print("ulid-monotonic:", ascending, ULIDTime(prev) === T);
```

### Constants

**`NIL`**

The all-zero UUID `00000000-0000-0000-0000-000000000000`.

**`MAX`**

The all-ones UUID `ffffffff-ffff-ffff-ffff-ffffffffffff`.

**`NAMESPACE_DNS`, `NAMESPACE_URL`, `NAMESPACE_OID`, `NAMESPACE_X500`**

The predefined RFC 4122 / RFC 9562 name namespaces.

```js
import { NIL, MAX, NAMESPACE_DNS } from "dyna:uuid";
print("uuid-consts:", NIL, MAX, NAMESPACE_DNS);
```

# dyna:config

Configuration text parsers: TOML 1.0, INI, `.env`, and front matter. Each grammar is a namespace object with static methods.

`import { TOML, INI, Env, FrontMatter } from "dyna:config";`

### TOML 1.0

**`TOML.parse(text) -> object`**

- `text` *(string)* — the TOML document.

Returns the parsed object. Parses a full TOML 1.0 document (plan 3.13): tables, arrays of tables, dotted keys, all date-time forms, floats with `inf`/`nan`, and string escapes. Leading zeros in integers are refused, and a key collision throws. Section nesting via dots is capped at depth 16.

Date-time values parse as their **raw strings** — `1979-05-27T07:32:00Z` stays the string `"1979-05-27T07:32:00Z"`, and local dates/times likewise. There is deliberately no `{dates: "iso"|"unix"}` option: local date-times carry no UTC offset, so a unix-seconds conversion would silently pick a timezone. Convert explicitly with `dyna:time` (`parseDate`) when the offset is known.

**`TOML.stringify(value) -> string`**

- `value` *(object)* — a plain object root.

Returns the serialized TOML text. Keys needing quoting are quoted, nested tables emit inline `{k = v}`, NaN/Infinity render as `nan`/`inf`/`-inf`, and `null`/`undefined` render as the literal `null` (TOML has none). A non-object root throws; unsupported values throw.

```js
import { TOML } from "dyna:config";

const doc = TOML.parse('title = "DynaJS"\n[server]\nport = 8080');
const text = TOML.stringify(doc);
const data = TOML.parse('[[items]]\nname = "a"\n[[items]]\nname = "b"');
```

### INI

**`INI.parse(text) -> object`**

- `text` *(string)* — the INI document.

Returns the parsed object. Reads the classic INI shape: `[section]` headers, `key = value` pairs, `key[] = v` append lists, `;`/`#` comments, and a bare key set to `true`. Sections nest via dots (`[a.b]`), capped at depth 16. Keys are defined with define semantics, so a file containing `__proto__ = x` produces an own property.

```js
import { INI } from "dyna:config";

const conf = INI.parse('[server]\nhost = localhost\nports[] = 80\nports[] = 443\n\nflag');
const host = conf.server.host;
```

**`INI.stringify(record) -> string`**

- `record` *(object)* — top-level scalars, plus one object value per `[section]`.

Returns INI text: scalars first, then one `[section]` block per object value. Strings go bare when they can (`;`, `#`, `=`, quotes and edge whitespace force double quotes, whose escapes `INI.parse` expands); numbers and booleans emit bare and parse back as strings — INI has no numeric or boolean type. Deeper nesting, arrays and `null` are refused.

### .env

**`Env.parse(text) -> object`**

- `text` *(string)* — the dotenv document.

Returns the parsed object. dotenv's grammar: `KEY=value` records, an optional `export ` prefix, `#` comments, and single or double quotes. Escapes (`\n`, `\t`, `\r`) expand only inside double quotes; single-quoted and bare values are literal. A line without `=` is skipped, not an error.

```js
import { Env } from "dyna:config";

const env = Env.parse('export DB_URL="postgres://localhost:5432"\nPORT=8080');
```

**`Env.load(path[, options]) -> object`**

- `path` *(string)* — the .env file to read.
- `options.assign` *(boolean, default `true`)* — inject each key into the process environment via `setenv`.
- `options.override` *(boolean, default `false`)* — replace entries that already exist in the environment; by default the environment wins.
- `options.expand` *(boolean, default `false`)* — expand `$NAME` and `${NAME}` in values against the environment as each key is assigned, so earlier keys of the same file are visible; `\$` is a literal dollar in quoted and unquoted values alike (a quoted `"\$X"` never becomes a live reference). Unset names expand to the empty string.

dotenv's actual job: parse the file AND assign it. Returns the record the file parses to — with the assigned (and expanded) values, so what the function returns is exactly what landed in the environment. The option bag is strict: a non-boolean value is a `TypeError`, and an unknown key is too — `unknown option "overide" (valid: assign, override, expand)` — checked before the file is opened. A missing file throws.

**`Env.stringify(record) -> string`**

- `record` *(object)* — keys and string values.

Returns `.env` text. Every value must be a string (dotenv values are strings; a silent coercion would round-trip wrongly, so numbers throw). Values that cannot sit bare — empty, whitespace, `#`, quotes — are double-quoted with the escapes `Env.parse` expands, so `Env.parse(Env.stringify(r))` is value-identical.

<!-- check:skip -- needs a .env file on disk -->
```js
import { Env } from "dyna:config";

const rec = Env.load("/srv/app/.env", { expand: true });
print(rec.PORT, Env.stringify(rec).indexOf("PORT=") >= 0);
```

### Front Matter

**`FrontMatter.split(text) -> { data, body, lang }`**

- `text` *(string)* — the input text.

Returns `{ data, body, lang }`. Splits text at a fence that must be the first line and close on its own line: `---` marks YAML, `+++` TOML, `;;;` JSON. `data` stays TEXT — splitting does not parse. When the text has no front matter, `data` and `lang` are `null` and `body` is the whole input; an unclosed fence is not front matter.

```js
import { FrontMatter } from "dyna:config";

const fm = FrontMatter.split('---\ntitle: DynaJS\n---\n\nbody text');
const noFm = FrontMatter.split('plain text');   // data: null
```

---

# dyna:csv

RFC 4180 reader/writer plus a file-backed table class whose every method load-modify-stores the bound file. In-memory `parse`/`stringify` handle CSV text with no file at all.

Which tabular surface when: `CSVFile` (here) is the FILE-BACKED load-modify-store table; `DataFrame` (dyna:dataframe) is the in-memory columnar analytics engine; `structures.Table` is a sparse string-keyed row/column map for lookups, not analytics.

Every options bag in this module is strict: an unknown option key throws a `TypeError` naming the key and the valid set (`unknown option "delimeter" (valid: delimiter, quote, hasHeader, strict)`), instead of being silently ignored.

`import { CSVFile, parse, stringify } from "dyna:csv";`

### parse

**`parse(text, options) -> { headers, rows, totalRows }`**

- `text` *(string)* — the CSV text.
- `delimiter` *(string)* — field separator; exactly one ASCII character, not `"`, CR, or LF, and different from `quote`.
- `quote` *(string)* — quote character; exactly one ASCII character, not `,`, CR, or LF, and different from `delimiter`.
- `hasHeader` *(boolean, default `true`)* — with `false`, the text has no header row: `headers` is `[]` and every row is a data row.
- `strict` *(boolean, default `false`)* — throw a `SyntaxError` naming the row on malformed input (garbage after a closing quote, an unterminated quote at end of text). The default tolerant mode keeps the readable prefix.

Returns `{ headers, rows, totalRows }` — the same shape as `CSVFile.read()`. A leading UTF-8 BOM is stripped. Rows are shaped to the first parsed row's width: short rows pad with `""`, long rows truncate. Unlike `read()` on an empty file, `parse("")` is the empty table (`headers: []`, `rows: []`, `totalRows: 0`). Row numbers in syntax errors are 1-based over the text.

```js
import { parse } from "dyna:csv";

const t = parse("name,score\nalice,95\nbob,82\n");
print(t.headers.join("|"));     // name|score
print(t.rows.join(";"));        // alice,95;bob,82
print(t.totalRows);             // 2

const tsv = parse("id\tname\n7\tann\n", { delimiter: "\t" });
print(tsv.rows[0].join("/"));   // 7/ann

const bare = parse("1,2,3\n4,5,6\n", { hasHeader: false });
print(bare.headers.length);     // 0
print(bare.totalRows);          // 2
```

### stringify

**`stringify(rows, options) -> string`**

- `rows` *(array)* — either arrays of cells (`string[][]`) or objects keyed by header (`Record<string, unknown>[]`); mixing the two forms in one call throws.
- `delimiter` *(string)* — field separator; same rules as `parse`.
- `quote` *(string)* — quote character; same rules as `parse`.
- `hasHeader` *(boolean, default `true`)* — for object rows, controls whether the derived header line is written. Array rows are written verbatim (there is no header to derive; pass one as your first row).

Returns CSV text; every line ends with `\n`. Array rows are written as-is, so `stringify([t.headers, ...t.rows])` round-trips `parse`. Object rows derive the column order from the **first row's own keys in insertion order** (JS ordering: integer-like keys come first); later rows map by those names — a missing key writes an empty cell, an extra key is ignored. Values are coerced to strings.

```js
import { stringify, parse } from "dyna:csv";

print(stringify([["a", "b"], ["1", "x,y"]]));
// a,b
// 1,"x,y"

const text = stringify([
    { name: "alice", score: 95 },
    { name: "bob", score: 82 },
]);
print(text);
// name,score
// alice,95
// bob,82

print(stringify(parse(text).rows, { hasHeader: false }));
// alice,95
// bob,82
```

### CSVFile

**`new CSVFile(path) -> CSVFile`**

- `path` *(Path)* — a `Path` from `dyna:file`.

Returns a new `CSVFile`. Binds the path; touches no disk until a method runs. The path is immutable after construction.

**`CSVFile.create(options) -> { path, rows }`**

- `headers` *(array)* — the column headers; at least one entry is required.
- `rows` *(array)* — optional data rows, each with exactly the header count.
- `overwrite` *(boolean, default `false`)* — allows replacing an existing file.

Returns `{ path, rows }`. Creates the file from `headers` and optional `rows`. Refuses an existing file unless `overwrite: true`.

**`CSVFile.read(options) -> { headers, rows, totalRows }`**

- `offset` *(number)* — first data row to return.
- `limit` *(number, default `-1`)* — `-1` = all.
- `columns` *(array)* — names; an unknown name throws.
- `strict` *(boolean, default `false`)* — throw a `SyntaxError` naming the row on malformed input (garbage after a closing quote, an unterminated quote at end of file). The default tolerant mode keeps the readable prefix.

Returns `{ headers, rows, totalRows }`. Loads the file and returns data rows as arrays of strings. `totalRows` counts data rows, excluding the header. A leading UTF-8 BOM is stripped so the first header name is clean.

**`CSVFile.addRow(options) -> { added, totalRows }`**

- `rows` *(array)* — each either a positional array (short rows pad with empty cells) or an object keyed by header name.

Returns `{ added, totalRows }`. Appends `rows`. Values are coerced to strings.

**`CSVFile.updateCell(options) -> { row, column, value }`**

- `row` *(number)* — the data row.
- `column` *(string)* — column name; out-of-range values are refused.
- `columnIndex` *(number)* — column position; out-of-range values are refused.
- `value` *(string)* — required; an empty string writes an empty cell.

Returns `{ row, column, value }`. Sets one cell.

**`CSVFile.removeRow(options) -> { removed, totalRows }`**

- `row` *(number)* — the data row to remove.

Returns `{ removed, totalRows }`. Removes the data row at `row`, renumbering the rest.

**`CSVFile.addColumn(options) -> { column, totalColumns }`**

- `column` *(string)* — the new column name; a duplicate name throws.
- `defaultValue` *(string, default `""`)* — fills every row.

Returns `{ column, totalColumns }`. Appends a column named `column`, filling every row with `defaultValue`.

**`CSVFile.removeColumn(options) -> { removedIndex, totalColumns }`**

- `column` *(string)* — the column name to drop.
- `columnIndex` *(number)* — the column position to drop.

Returns `{ removedIndex, totalColumns }`. Drops the column from every row.

**`CSVFile.renameColumn(options) -> { oldName, newName }`**

- `oldName` *(string)* — the current name; an unknown old name throws.
- `newName` *(string)* — the new name; a taken new name throws.

Returns `{ oldName, newName }`. Renames `oldName` to `newName`. A rename to the same name is a no-op.

**`CSVFile.readColumnValuesRange(options) -> array`**

- `column` *(string)* — the column to read.
- `start` *(number)* — first data row of the window.
- `end` *(number)* — exclusive bound; when omitted, runs to the last row.

Returns one column's values over a `[start, end)` window of data rows. An explicit window larger than 1000 rows is refused.

**`CSVFile.readRowRange(options) -> { headers, rows }`**

- `start` *(number, default `0`)* — first data row.
- `end` *(number)* — exclusive bound.

Returns `{ headers, rows }` for rows `[start, end)` as arrays. The default is a single row (`readRowRange()` = row 0); a window over 100 rows is refused. Positional arguments are refused — options must be an object.

**`CSVFile.selectColumnRange(options) -> { columns, rows }`**

- `columns` *(array)* — the columns to project; must be non-empty.
- `start` *(number)* — first data row.
- `end` *(number)* — exclusive bound.

Returns `{ columns, rows }`. Projects `columns` over a `[start, end)` window, capped at 100 rows.

**`CSVFile.close()`**

No parameters.

Releases the resource.

**`CSVFile.dispose()`**

No parameters.

Releases the resource.

**`CSVFile.closed -> boolean`**

Returns `true` once the resource is released; any method after that throws.

```js
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";

const f = new CSVFile(new Path("/tmp/teams.csv"));
f.create({ headers: ["name", "score"], rows: [["a", "1"], ["b", "2"]], overwrite: true });
f.addRow({ rows: [{ name: "c", score: "3" }] });
f.updateCell({ row: 0, column: "score", value: "10" });
const all = f.read({});
const top = f.readRowRange({ start: 0, end: 2 });
const names = f.readColumnValuesRange({ column: "name" });
f.close();
```

Showcase — full file-backed table lifecycle (column schema evolution + paginated reads):

```js
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";

const p = new Path("/tmp/showcase_csv.csv");
const t = new CSVFile(p);
t.create({ headers: ["id", "name", "score"], rows: [[1,"alice",95],[2,"bob",82],[3,"carol",88]], overwrite: true });

// schema evolution — every call load-modify-stores the file
t.addColumn({ column: "grade", defaultValue: "B" });
t.renameColumn({ oldName: "score", newName: "points" });
print(t.read({}).headers.join(",")); // id,name,points,grade
print(t.readColumnValuesRange({ column: "grade" }).join(",")); // B,B,B

// paginated / projected reads (capped windows — 100/1000 row guards)
print(JSON.stringify(t.readRowRange({ start: 0, end: 2 }).rows)); // [["1","alice","95","B"],...]
print(JSON.stringify(t.selectColumnRange({ columns: ["name","points"], start: 1, end: 3 }).rows));

// row mutation
t.removeColumn({ column: "grade" });
t.removeRow({ row: 1 }); // removes bob
print(t.read({}).totalRows); // 2
t.updateCell({ row: 0, columnIndex: 1, value: "ALICE" });
print(t.read({}).rows[0][1]); // ALICE
t.close();
```

Showcase — object-row ingestion and column-index updates:

```js
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";

const f2 = new CSVFile(new Path("/tmp/showcase_csv2.csv"));
f2.create({ headers: ["host", "ip", "up"], overwrite: true });
f2.addRow({ rows: [{ host: "a.test", ip: "10.0.0.1", up: "1" }, { host: "b.test", ip: "10.0.0.2", up: "0" }] });
f2.addRow({ rows: [["c.test", "10.0.0.3", "1"]] }); // positional also accepted
print(f2.read({ columns: ["host","up"] }).rows.length); // 3
print(f2.readColumnValuesRange({ column: "ip", start: 0, end: 2 }).join("|")); // 10.0.0.1|10.0.0.2
f2.close();
```

### CSVFile.rows (streaming)

**`CSVFile.rows({ batch, delimiter, quote, strict }?) -> AsyncIterable`**

- `batch` *(number, default `1000`, 1..100000)* — rows per batch.
- `delimiter`/`quote`/`strict` — same dialect rules as `parse`/`read`.

Each `for await` step resolves `{ headers, rows, offset, totalRows }` with at most `batch` rows; only one batch is materialised at a time. Breaking the loop closes the iterator.

The memory half is the point and the time half is the price: each `next()` re-reads and re-parses the file to slice its batch (snapshot-per-batch — a concurrent mutation lands on the next batch boundary), so peak memory stays at one batch while total parse work grows with `batches x file`. Prefer the largest `batch` the consumer can hold (batch=10000 measured ~9x faster end-to-end than batch=1000 on a 200k-row file, at somewhat higher peak memory), or `read()` when the table fits in memory.

```js
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";

const f3 = new CSVFile(new Path("/tmp/showcase_csv3.csv"));
f3.create({ headers: ["a", "b"], rows: [["1", "2"], ["3", "4"]], overwrite: true });
for await (const batch of f3.rows({ batch: 1 })) {
  print(batch.rows.length); // 1
  break;
}
f3.close();
```

---

# dyna:json

RFC 6901 JSON Pointer and RFC 6902 JSON Patch with copy-on-write semantics.

Neighbours on purpose: canonical/stable JSON stringify is `StableStringify` (RFC 8785) in `dyna:encoding`, and so is the JSON5 family (`JSON5Parse`/`JSON5Stringify`) — this module will gain `JSONC.parse` (comments + trailing commas) in phase 1. JSONPath pattern queries (RFC 9535) are `encoding.JSONPath`; Pointer is for exact addressing and patch paths.

`import { Pointer, Patch } from "dyna:json";`

### JSON Pointer

**`Pointer.get(doc, pointer) -> value`**

- `doc` *(object)* — the document to walk.
- `pointer` *(string)* — `""` is the whole document; `~0`/`~1` unescape `~`/`/`; `-` is refused where a real index is required.

Returns the value at `pointer`. A pointer longer than 65536 bytes or deeper than 128 levels throws. Missing members, array indices out of range, and non-numeric tokens against arrays throw.

**`Pointer.has(doc, pointer) -> boolean`**

- `doc` *(object)* — the document to walk.
- `pointer` *(string)* — the pointer to test.

Returns whether the target exists. Same walk as `get`, but a missing target returns `false` instead of throwing; syntax errors still throw.

**`Pointer.set(doc, pointer, value) -> doc`**

- `doc` *(object)* — the caller's document.
- `pointer` *(string)* — the pointer to write.
- `value` *(any)* — the value to insert.

Returns `doc`. MUTATES the caller's document in place (the reference-implementation convention). Inserts with RFC 6902 "add" semantics — `-` appends, a missing parent throws. Values are cloned on insert so the result never aliases the caller's objects.

**`Pointer.remove(doc, pointer) -> doc`**

- `doc` *(object)* — the caller's document.
- `pointer` *(string)* — the pointer to remove.

Returns `doc`. MUTATES the caller's document in place (the reference-implementation convention). Refuses the root and a missing target.

**`Pointer.escape(token) -> string`**

- `token` *(string)* — a pointer token.

Returns the escaped token. Turns `~` into `~0` and `/` into `~1`.

**`Pointer.unescape(token) -> string`**

- `token` *(string)* — an escaped pointer token.

Returns the unescaped token. Reverses `escape`; throws on a `~` not followed by `0` or `1`.

```js
import { Pointer } from "dyna:json";

const doc = { a: { b: ["first", "second"] } };
const val = Pointer.get(doc, "/a/b/1");
const has = Pointer.has(doc, "/a/b/2");
const key = Pointer.escape("a/b~c");
const result = Pointer.set(doc, "/a/b/-", "third");
```

### JSON Patch

**`Patch.apply(doc, ops) -> doc`**

- `doc` *(object)* — the input document; never written.
- `ops` *(array)* — RFC 6902 operations.

Returns a new document. Runs the six RFC 6902 ops (`add`, `remove`, `replace`, `move`, `copy`, `test`) on a PRIVATE deep copy: a failing op frees the copy and throws, leaving the input intact. `test` compares with deep equality (primitives via `===`); `move` refuses a `from` that is a proper prefix of `path`. Non-plain values (Date, Map, typed arrays, functions) pass by reference.

```js
import { Patch } from "dyna:json";

const result = Patch.apply({ a: { b: [1, 2] } }, [
    { op: "add", path: "/a/b/-", value: 3 },
    { op: "test", path: "/a/b/0", value: 1 },
    { op: "remove", path: "/a/b/1" }
]);
```

### NDJSON

Newline-delimited JSON, buffer-at-once: one JSON value per line, the whole text processed before anything is returned. The line rules match `stream.ndjson(src)` in dyna:stream — the single streaming form, which this module deliberately does not duplicate (plan cut `Ndjson.reader` as a second streaming surface): lines split on `\n`, one trailing `\r` is stripped (CRLF input), and a line that is empty or only whitespace is skipped. Canonical/stable JSON lives in `dyna:encoding` (`StableStringify`, RFC 8785), not here.

**`Ndjson.parse(text) -> value[]`**

- `text` *(string)* — the NDJSON text.

Returns one array entry per non-blank line, in order. A malformed line throws a `SyntaxError` naming its **1-based file line number** — blank lines count toward the number, because it is a position in the text, not an index into the result (`Ndjson.parse: line 2: ...`).

**`Ndjson.stringify(items) -> string`**

- `items` *(array)* — the values to write.

Returns one compact JSON document per line with the trailing newline included, so `parse(stringify(items))` is the identity for JSON-safe values. An item with no JSON form (`undefined`, a function, a symbol) throws a `TypeError` naming its 1-based index — emitting an empty line would corrupt the file for every reader after it; a `BigInt` propagates `JSON.stringify`'s own error.

```js
import { Ndjson } from "dyna:json";

const lines = Ndjson.stringify([{ i: 1 }, { i: 2 }, { i: 3 }]);
const items = Ndjson.parse(lines);
const one = Ndjson.parse('{"a":1}\n\n{"b":2}\n');
```

---

# dyna:log

Leveled structured logging. The default emits one JSON object per line to stderr; a Logger can also write to a file with size- or time-based rollover, retention, and a symlink to the active file, and can render a human-readable text format instead of JSON.

`import { Logger, Debug } from "dyna:log";`

### Logger

**`new Logger(options) -> Logger`**

Options bags are strict: an unknown key — in `options`, in `options.rollover`, or in `child`'s second argument — throws a `TypeError` naming the key and the valid set (`unknown option "host" (valid: level, name, timestamp, format, pid, hostname, base, mkdir, buffer, sample, rollover, dest)`). `null`/`undefined`/primitive options still mean "no options". The `fields` argument of `child` is not an options bag and stays open.

- `level` *(string, default `"info"`)* — `"trace"`..`"fatal"` or `"silent"`; an unknown level throws.
- `name` *(string)* — optional logger name.
- `timestamp` *(string | false, default `"epoch"`)* — `"epoch"` | `"iso"` | `false`; any other string throws (a wrong string silently meaning epoch is exactly the mistake this refuses).
- `base` *(object)* — fields pre-serialized into a prefix every line carries.
- `dest` *(string | function)* — write to this path (append mode) instead of stderr. Two Loggers naming the same path share one open file and one rollover state. A **function** `dest` is a custom sink: it receives the exact formatted line — trailing newline included — the instant it is built, in both `json` and `text` formats. Nothing is batched for a function dest, so `flush()` is a no-op for it and exit-time flushing has nothing to do; and because a sink that raises is a caller bug (unlike a failing fd, which the sink machinery tracks), a throwing `dest` propagates the throw out of the emit. `rollover` requires a path and refuses a function.
- `mkdir` *(boolean)* — create `dest`'s parent directories.
- `buffer` *(boolean | number)* — batch file output: `true` is 64 KiB, a number is a byte size (1..1048576). Buffered lines land on `flush()`, on a full buffer, on rotation, and when the last Logger closes. stderr is never buffered (one `write(2)` per line is its interleave contract). **This is the performance lever**: an emit is syscall-bound, and measured on the same workload, a buffered file `dest` costs 95 ns/op against 630 ns/op to stderr — 6.6×. See `tests/bench_log_ab.js`.
- `pid` *(boolean)* — add `"pid"` to every line.
- `hostname` *(boolean)* — add `"hostname"` to every line.
- `format` *(string, default `"json"`)* — `"json"` or `"text"`.
- `sample` *(number, default `1`)* — emit sampling for hot paths: every Nth attempt that passes the level gate is written. Deterministic — a per-logger counter modulo `N` — so a sampled logger's output is reproducible run to run given the same call sequence. The counter counts level-passing attempts (a call below the level is never counted), it is per Logger, and a `child()` restarts its parent's divisor at count zero. `0`, negatives, non-integers and non-numbers throw.
- `rollover` *(object)* — file rotation; requires `dest`. Checked lazily before each emit, so a line never straddles two files:
    - `size` *(number | string)* — rotate at this size; a number is bytes, a string carries a 1024-based unit (`"500k"`, `"10m"`, `"2g"`).
    - `frequency` *(string | number)* — `"daily"` or `"hourly"` (the period is named in the file, UTC), or a number of milliseconds.
    - `count` *(number)* — rotated files to keep besides the active one; oldest deleted first. Unbounded when omitted.
    - `symlink` *(boolean)* — keep `dest` itself as a symlink to the active file, so `tail -f dest` follows rotations.

Rotated names carry the extension last — `app.2026-08-28.1.log` — and sort chronologically by name alone. Numbering continues from the highest existing number, so a pruned file's number is never reused; a time-only rollover (no `size`) reuses the current period's file across restarts. Day and hour boundaries are UTC.

Returns a new `Logger`.

**`Logger.trace(msg)`**, **`Logger.debug(msg)`**, **`Logger.info(msg)`**, **`Logger.warn(msg)`**, **`Logger.error(msg)`**, **`Logger.fatal(msg)`**

- `msg` *(any)* — the message, or Pino's shapes: `(msg)`, `(fields, msg)`, `(err, msg)`, `(err, fields, msg)`.

Each emits one line and one `write(2)` below the configured level; the level check is the gate before any work. An Error serializes as `{type, message, stack}` plus its extra enumerable props (`code`, `errno`, ...). A caller key that collides with a key the frame writes — `time`, `level`, `name`, `pid`, `hostname`, `msg`, `err` — is dropped, so a field cannot forge or reroute a line. A line is truncated at 64 KiB rather than allowed to grow, and a broken destination never kills the caller. The cut is marked `...` in-band and the line is still closed and parseable, so an oversized payload cannot corrupt the framing of the lines around it.

**`Logger.child(fields, { level }?) -> Logger`**

- `fields` *(object)* — fields appended to the base prefix.
- `options.level` *(string, optional)* — a level threshold for the child that overrides the parent's (the child may be noisier or quieter than its parent). An unknown level throws.

Returns a new `Logger` with `fields` appended to the base prefix. The prefix serializes once, not per line, and the child shares the parent's destination.

**`Logger.enabled(level) -> boolean`**

- `level` *(string)* — a log level.

Returns whether `level` passes the current threshold. An unknown level throws.

**`Logger.flush()`**

Lands any buffered file output now. A no-op on stderr, and a no-op for function dests (a function sink is written the instant each line is built — there is nothing to buffer). Exit-time behavior is the same contract: buffered file output lands when the last Logger closes or at process exit; stderr and function dests need nothing.

**`Logger.level -> string`**

Gets or sets the level. The setter throws on an unknown level.

```js
import { Logger } from "dyna:log";

const log = new Logger({ level: "warn", name: "app", base: { env: "dev" }, timestamp: "iso" });
log.warn({ req: 42 }, "slow request");
log.error(new Error("boom"));
const child = log.child({ route: "/api" });
```

<!-- check:skip -->
```js
// A file destination with rollover: rotate at 10 MiB or daily, keep 7,
// and let app.log always point at the active file.
import { Logger } from "dyna:log";

const log = new Logger({
    dest: "logs/app.log", mkdir: true,
    rollover: { size: "10m", frequency: "daily", count: 7, symlink: true },
    pid: true,
});
log.info({ requestId: "r1" }, "started");
log.flush();
```

The text format is for humans (a terminal, a dev run): one line, level aligned to five columns, then the name, the message, and the fields as `k=v` — composites as compact JSON, an Error as `Type: message` (its stack stays in the JSON format).

```text
2026-08-28T09:15:02.123Z WARN  api: slow request env=dev req=42
```

### Debug

**`Debug(namespace) -> function`**

- `namespace` *(string)* — the debug namespace.

Returns a function that prints `namespace message` to stderr only when `DEBUG` matches — comma-separated patterns where the LAST match wins, a leading `-` negates, and one `*` may sit anywhere (`DEBUG=*,-app:secret`). The match is computed once at construction, so an unmatched Debug costs nothing per call. Multiple arguments join with a space.

```js
import { Debug } from "dyna:log";

const dbg = Debug("svc:parse");
dbg("tokens:", 42);
```

### Custom sinks & sampling

```js
import { Logger } from "dyna:log";

// LG-1: dest accepts a function -- the exact formatted line, newline
// included, is delivered the moment it is built (unbuffered; flush() is a
// no-op for it, a throwing sink propagates).
const lines = [];
const sink = new Logger({ dest: (line) => lines.push(line) });
sink.info("to a test sink");
sink.warn({ req: 7 }, "custom sink with fields");

// LG-2: sample -- every Nth level-passing attempt, deterministically.
const sampled = new Logger({ dest: () => {}, sample: 100 });
for (let i = 0; i < 300; i++) {
    sampled.info("hot path");          // 3 of these reach the sink
}
print(lines.length === 2, lines[0].endsWith("\n"));
```

---

# dyna:schema

JSON Schema Draft 2020-12 validator with memoized compilation: patterns pre-compile with unicode semantics and `$ref` resolves at compile time, so validation is pure dispatch.

`import { Schema } from "dyna:schema";`

### Validation

**`Schema.compile(schema) -> CompiledSchema`**

- `schema` *(object)* — the schema object to compile.

Returns a compiled schema. Compiles the schema object once into a native node tree. Compile bounds: schema nesting capped at 256, list keywords at 1<<20 elements, and an invalid `pattern` throws with the regex error. A compiled schema is reusable from any thread.

**`CompiledSchema.validate(instance) -> { valid, errors }`**

- `instance` *(any)* — the value to validate.

Returns `{ valid, errors }`; each error is `{ path, message, keyword }`. Errors are bounded at 256, instance recursion at 512, the `$ref` chain at 64, and arrays longer than 1<<22 elements pass unvalidated (a documented cap, not a failure).

**`Schema.validate(schema, instance) -> { valid, errors }`**

- `schema` *(object)* — the schema to compile; may already be compiled.
- `instance` *(any)* — the value to validate.

Returns `{ valid, errors }`. Convenience form: compiles `schema` and caches the compiled form on the schema object itself, so repeated calls compile once. Accepts an already-compiled schema as the first argument.

```js
import { Schema } from "dyna:schema";

const schema = {
    type: "object",
    properties: {
        id: { type: "integer", minimum: 1 },
        email: { type: "string" }
    },
    required: ["id", "email"]
};

const validator = Schema.compile(schema);
const ok = validator.validate({ id: 1, email: "user@example.com" });
const oneShot = Schema.validate(schema, { id: 1, email: "user@example.com" });
const bad = validator.validate({ id: 0 });
```

`unevaluatedProperties` / `unevaluatedItems` (2020-12 ) are enforced with per-position evaluation tracking: annotations flow through `allOf`, `dependentSchemas`, `if`/`then`/`else`, `$ref` targets and passing `anyOf`/`oneOf` branches, never across sibling branches ("cousins") or child instance positions, and a failed subschema annotates nothing. Over 4096 tracked evaluated properties or items at one position the validation fails closed with a `RangeError` (a documented cap, never a guessed outcome).

Draft support: the compiled tree implements Draft 2020-12 core keywords only. There is no draft-dispatch code inside the compiler, so there is deliberately no `{draft}` option — draft-07 documents need upgrading by hand (`exclusiveMinimum` as a number, `id`, `$ref` in place).

### Custom formats

**`Schema.registerFormat(name, fn)`**

- `name` *(string)* — the format name; non-empty, NUL-free, at most 256 bytes.
- `fn` *(function)* — validator `(value: string) => truthy | falsy`.

Wires a custom `"format"` validator into the `"format"` keyword (which is otherwise annotation-only per Draft 2020-12). The registry is **process-wide and live**: every compiled schema asserts against the same registry at `validate()` time, including schemas compiled before the registration. Semantics:

- `fn` is called with the instance STRING; its return value is coerced to boolean. A throw aborts `validate` with that exception.
- The assertion applies only to string instances; numbers, objects and arrays pass unchecked. Unregistered format names stay annotation-only (no assertion, no error).
- Duplicate names are refused with a `TypeError` (the registry is immutable once set); so are non-functions and bad names.

```js
import { Schema } from "dyna:schema";

Schema.registerFormat("evenLength", (s) => s.length % 2 === 0);
const fmt = Schema.compile({ type: "string", format: "evenLength" });
print(fmt.validate("abcd").valid, !fmt.validate("abc").valid);
const nested = Schema.compile({
    type: "object",
    properties: { code: { type: "string", format: "evenLength" } }
});
print(nested.validate({ code: "ab" }).valid, !nested.validate({ code: "a" }).valid);
```

---

# dyna:semver

semver 2.0.0 parsing, comparison, coercion, and range matching. Every numeric field or identifier above `Number.MAX_SAFE_INTEGER` fails parsing.

`import { parse, isValid, clean, coerce, compare, eq, gt, gte, lt, lte, neq, sort, major, minor, patch, prerelease, inc, diff, compareBuild, intersects, satisfies, maxSatisfying, minSatisfying, Range } from "dyna:semver";`

### Parsing

**`parse(version) -> object`**

- `version` *(string)* — the version string; a leading `v` is allowed.

Returns `{ major, minor, patch, prerelease, build, version }`; `prerelease`/`build` are arrays of identifiers. Parses `major.minor.patch[-pre][+build]`. Leading zeros, over-long fields, and values above MAX_SAFE throw.

**`isValid(version) -> boolean`**

- `version` *(string)* — the version string.

Returns whether the string parses at all.

**`clean(version) -> string`**

- `version` *(string)* — the version string.

Returns the normalized version, or `null` when unparsable. Trims, strips a leading `=`, and returns the normalized version.

**`coerce(version) -> string | null`**

- `version` *(string)* — the input string.

Returns the normalized `X.Y.Z` (missing minor/patch become `0`), or `null` when no usable run exists. Finds the first run of digits in the input. Runs longer than 16 digits are skipped.

```js
import { parse, isValid, clean, coerce } from "dyna:semver";

const v = parse("1.2.3-beta.1+build.5");
const ok = isValid("1.2.3");
const norm = clean("  =v1.2.3  ");
const loose = coerce("release-2.3.4");
```

### Comparison

**`compare(a, b) -> -1 | 0 | 1`**

- `a` *(string)* — a version.
- `b` *(string)* — a version.

Returns full semver precedence. Prerelease identifiers are compared per the spec's rules (numeric identifiers compare numerically, a version without prerelease beats one with).

**`eq(a, b) -> boolean`**

- `a` *(string)* — a version.
- `b` *(string)* — a version.

Returns whether the versions are equal.

**`neq(a, b) -> boolean`**

- `a` *(string)* — a version.
- `b` *(string)* — a version.

Returns whether the versions differ.

**`gt(a, b) -> boolean`**

- `a` *(string)* — a version.
- `b` *(string)* — a version.

Returns whether `a` is greater than `b`.

**`gte(a, b) -> boolean`**

- `a` *(string)* — a version.
- `b` *(string)* — a version.

Returns whether `a` is greater than or equal to `b`.

**`lt(a, b) -> boolean`**

- `a` *(string)* — a version.
- `b` *(string)* — a version.

Returns whether `a` is less than `b`.

**`lte(a, b) -> boolean`**

- `a` *(string)* — a version.
- `b` *(string)* — a version.

Returns whether `a` is less than or equal to `b`.

**`sort(versions) -> array`**

- `versions` *(array)* — version strings.

Returns the same array, sorted ascending in place by precedence.

**`major(v) -> number`**

- `v` *(string)* — a version.

Returns the major field.

**`minor(v) -> number`**

- `v` *(string)* — a version.

Returns the minor field.

**`patch(v) -> number`**

- `v` *(string)* — a version.

Returns the patch field.

**`prerelease(v) -> array | null`**

- `v` *(string)* — a version.

Returns the prerelease identifiers, or `null` when none.

**`inc(version, release[, identifier]) -> string`**

- `version` *(string)* — the version to bump.
- `release` *(string)* — `major`, `minor`, `patch`, `premajor`, `preminor`, `prepatch`, `prerelease`, or `release`.
- `identifier` *(string)* — applies to the `pre*` steps (default base `0`, as in `1.2.3-0`).

Returns the bumped version. An unknown release type throws. `release: "release"` drops the prerelease and returns `X.Y.Z` (build metadata never survives `inc`); when the input has no prerelease it returns `null` (node-semver pins `inc('1.0.0','release') === null`).

**`diff(a, b) -> string | null`**

- `a`, `b` *(string)* — versions.

Returns the highest-precedence component that differs — `"major"`, `"minor"`, `"patch"`, `"prerelease"`, or `"build"` — or `null` when the two versions are identical. This is node-semver's documented contract, with two pinned deviations from its modern implementation: the plain component name is always returned (node 7.8.5 adds a `pre` prefix when the higher version carries a prerelease), and a build-only difference is `"build"` (node's `eq` cannot see build metadata, so it answers `null`).

**`compareBuild(a, b) -> -1 | 0 | 1`**

- `a`, `b` *(string)* — versions.

The same as `compare`, but when the two versions are precedence-equal the build metadata decides: a version without build metadata sorts below one with it, and identifiers compare numerically when both are numeric, numeric < alphanumeric otherwise, else lexically.

```js
import { compare, eq, gt, gte, lt, lte, neq, sort, major, minor, patch, prerelease, inc, diff, compareBuild } from "dyna:semver";

const cmp = compare("1.0.0", "2.0.0");
const bigger = gt("2.0.0", "1.0.0");
const next = inc("1.2.3", "minor");
const pre = inc("1.2.3", "prerelease", "beta");
const field = major("1.2.3");
const order = sort(["1.10.0", "1.2.0"]);
print(diff("1.2.3", "1.3.0") === "minor", diff("1.2.3", "1.2.3+b2") === "build");
print(compareBuild("1.0.0+a", "1.0.0+b") === -1, compareBuild("1.0.0", "1.0.0+rc1") === -1);
```

### Ranges

**`new Range(rangeString) -> Range`**

- `rangeString` *(string)* — a range expression.

Returns a compiled `Range`. Comparator syntax: `>=1.2.3 <2`, `^1.2.3`, `~1.2`, hyphen ranges `1.2.3 - 2.0.0`, `||`-separated sets, and `*`/partials. An invalid range throws. Compile caps: 24 identifiers per version, 16 comparator sets, 24 comparators per set.

**`Range.test(version) -> boolean`**

- `version` *(string)* — a version.

Returns whether the version matches the compiled range.

**`Range.filter(versions) -> array`**

- `versions` *(array)* — version strings.

Returns the versions that match, preserving input order.

**`Range.maxSatisfying(versions) -> string`**

- `versions` *(array)* — version strings.

Returns the highest matching version, or `null`.

**`Range.minSatisfying(versions) -> string`**

- `versions` *(array)* — version strings.

Returns the lowest matching version, or `null`.

**`Range.source -> string`**

Returns the original range text.

**`Range.setCount -> number`**

Returns the number of `||`-separated sets (the observable measure of compilation).

**`Range.intersects(other) -> boolean`**

- `other` *(string | Range)* — a range expression or a compiled `Range`.

Whether the two ranges intersect: node-semver's comparator-pairwise rule, including its `">=0.0.0"`-as-ANY normalization (so `intersects(">=0.0.0", "<0.0.0")` is `true`, as in node).

```js
import { Range } from "dyna:semver";

const r = new Range(">=1.2.3 <2 || ^3.0.0");
const hit = r.test("1.5.0");
const picks = r.filter(["1.0.0", "1.5.0", "3.1.0"]);
const best = r.maxSatisfying(["1.5.0", "1.6.0", "3.1.0"]);
print(r.intersects("^1.5.0"), !r.intersects("2.0.0"));
```

**`satisfies(version, range) -> boolean`**

- `version` *(string)* — a version.
- `range` *(string)* — a range expression.

Returns whether the version matches. One-shot match; a range string is parsed per call.

**`maxSatisfying(versions, range) -> string`**

- `versions` *(array)* — version strings.
- `range` *(string)* — a range expression.

Returns the highest matching version, or `null`. Free-function form of the Range method; coerce every element, and a non-string array element throws.

**`minSatisfying(versions, range) -> string`**

- `versions` *(array)* — version strings.
- `range` *(string)* — a range expression.

Returns the lowest matching version, or `null`. Free-function form of the Range method; coerce every element, and a non-string array element throws.

**`intersects(rangeA, rangeB) -> boolean`**

- `rangeA`, `rangeB` *(string | Range)* — range expressions or compiled `Range`s.

Whether the two ranges intersect: node-semver's comparator-pairwise rule (a pair of `||`-sets intersects when their comparators pairwise intersect), including the `">=0.0.0"`-as-ANY normalization. Range-method form: `new Range(a).intersects(b)`.

```js
import { satisfies, maxSatisfying, minSatisfying, intersects } from "dyna:semver";

const inRange = satisfies("1.4.0", ">=1.2.3 <2.0.0");
const newest = maxSatisfying(["1.2.0", "1.5.0", "2.0.0"], ">=1.0.0 <2.0.0");
const oldest = minSatisfying(["1.2.0", "1.5.0", "0.9.0"], ">=1.0.0 <2.0.0");
print(intersects("^1.2.3", "~1.5.0") === true, intersects("1.2.3", "1.3.0") === false);
```

---

# dyna:url

WHATWG-style URL parsing, IDNA 2008 (UTS #46) over Unicode 16.0.0 tables, Punycode, and `application/x-www-form-urlencoded` coding.

`import { URL, domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode, formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";`

### URL

**`new URL(input[, base]) -> URL`**

- `input` *(string)* — the URL to parse.
- `base` *(string)* — optional; the base URL.

Returns a new `URL`. Parses `input` against an optional `base` URL; a bad `base` or unparsable `input` throws, and input longer than 65536 bytes throws a RangeError.

**`URL.parse(input[, base]) -> URL | null`**

- `input` *(string)* — the URL to parse.
- `base` *(string)* — optional; the base URL.

The constructor's parse without throwing: returns `null` for EVERY failure the constructor would throw for (unparsable input, bad base, over-long input). A non-string argument is still the constructor's `TypeError`. The result is always the plain `URL` class.

**`URL.canParse(input[, base]) -> boolean`**

- `input` *(string)* — the URL to test.
- `base` *(string)* — optional; the base URL.

`URL.parse` as a predicate.

**`URL.join(base, rel) -> string`**

- `base` *(string)* — the base URL.
- `rel` *(string)* — the reference to resolve against it.

Returns the resolved href — `URL.join("https://a.com/x/y?q=1", "../z")` is `"https://a.com/z"`, and an absolute `rel` wins outright. Throws `TypeError` on an unparseable base or reference: in a join that is a caller bug, not a probe.

```js
import { URL } from "dyna:url";

const u = new URL("https://user:pass@example.com:8080/p/a?q=1#frag");
const joined = new URL("/p", "https://base.example:99/x");
const host = u.hostname;
u.searchParams.set("q", "2");
const maybe = URL.parse("::::");          // null, never throws
const ok = URL.canParse("https://x.test/p", "https://base.test/");  // true
const rel = URL.join("https://a.com/x/y", "../z");                  // "https://a.com/z"
```

**`URL.href`, `URL.protocol`, `URL.username`, `URL.password`, `URL.origin -> string`** (read-only)

Returns the corresponding component, re-rendered from the parsed parts. `origin` is `scheme://host[:port]`.

**`URL.host`, `URL.hostname`, `URL.port`, `URL.pathname`, `URL.search`, `URL.hash`, `URL.searchParams`** (settable)

The mutable components take assignments with the WHATWG setter semantics. Each assignment reparses a substituted candidate through this module's own parser, so percent-encoding, IDNA, lowercasing, default-port elision and dot-segment resolution are the parser's, not a second implementation's.

```js
const u = new URL("https://a.test:8443/x?old#f");
u.pathname = "/p";            // https://a.test:8443/p?old#f
u.search = "a=1";             // https://a.test:8443/p?a=1#f
u.host = "b.test:99";         // https://b.test:99/p?a=1#f
u.hash = "";                  // https://b.test:99/p?a=1
u.searchParams = { q: "v" };  // https://b.test:99/p?q=v
```

- `pathname` is parsed in path state, so a `?` inside the value starts the query and a `#` starts the fragment (the state override the specification defines); a relative value is rooted at `/`; `.`/`..` resolve; a space becomes `%20`.
- `search` takes one leading `?` as a delimiter; `#` inside the value starts the fragment; `""` clears the query.
- `hash` takes one leading `#` as a delimiter; a later `#` is fragment content; `""` clears it.
- `host` may carry `:port` (the parser splits it, and a default port is elided); `hostname` leaves the port alone; `port` takes digits only, and `""` clears it. A value containing `/`, `?` or `#`, or an empty one, is a NO-OP -- as is any assignment on a URL with no host (`mailto:`).
- `searchParams = value` replaces the whole query with `new URLSearchParams(value)`'s serialization (a string, a pair array or a record), and `""` clears it. A previously-read `url.searchParams` object stays live and revalidates against the new query.
- Setting an unrepresentable value is a no-op rather than an error, which is the specification's own rule for these setters.

**`URL.toJSON() -> string`**

Returns the full href.

**`URL.toString() -> string`**

Returns the full href.

**`URL.searchParams -> URLSearchParams`**

Returns a fresh URLSearchParams BOUND to this URL: mutating it changes `url.search` and `url.href`.

```js
import { URL } from "dyna:url";

const u = new URL("https://user:pass@example.com:8080/p/a?q=1#frag");
const joined = new URL("/p", "https://base.example:99/x");
const host = u.hostname;
u.searchParams.set("q", "2");
```

### URLSearchParams

The WHATWG query list. A BOUND instance (from `url.searchParams`) writes through to the URL's query slot, so a mutation changes `url.search` and `url.href`; a STANDALONE instance owns its query. A searchParams created from another URLSearchParams copies its list.

**Iteration (changed from arrays to iterators, WHATWG parity):** `entries()`/`keys()`/`values()` return `IterableIterator` objects over a live index into the pair list — `it.next()`, `for..of`, spread, `Array.from` and `new Map(sp)` all work, on the iterators and (via the instance's `[Symbol.iterator]`, which IS the entries iterator) on the object directly. Mutation between steps is observed; once an iterator is exhausted it stays done forever, even if pairs are appended afterward. Code that indexed or measured the old arrays must switch to the `entriesArray()`/`keysArray()`/`valuesArray()` twins, which return exactly what `entries()`/`keys()`/`values()` returned before.

**`new URLSearchParams([init]) -> URLSearchParams`**

- `init` *(string | URLSearchParams | Array<[string, string]> | Record<string, string>)* — optional; the query list. A leading `?` on a string is not part of the list.

**`URLSearchParams.size -> number`**

The number of name/value pairs.

**`URLSearchParams.append(name, value)`**

Appends a pair; a name that already exists keeps its other values.

**`URLSearchParams.delete(name)`**

Removes every pair with that name.

**`URLSearchParams.get(name) -> string | null`**

The first value for the name, or `null` when absent.

**`URLSearchParams.getAll(name) -> string[]`**

Every value for the name.

**`URLSearchParams.has(name) -> boolean`**

Whether the name has at least one value.

**`URLSearchParams.set(name, value)`**

Replaces every pair with that name with a single pair.

**`URLSearchParams.sort()`**

Orders pairs by the DECODED key's byte order, in place.

**`URLSearchParams.toString() -> string`**

The `name=value&...` form, re-encoded.

**`URLSearchParams.forEach(callback)`**

- `callback` *(function)* — called as `(value, key, params)` per pair, in order.

**`URLSearchParams[Symbol.iterator]() -> IterableIterator<[string, string]>`**

The entries iterator, so the instance itself is iterable.

**`URLSearchParams.keys() -> IterableIterator<string>`**

The names in order.

**`URLSearchParams.values() -> IterableIterator<string>`**

The values in order.

**`URLSearchParams.entries() -> IterableIterator<[string, string]>`**

The pairs in order.

**`URLSearchParams.keysArray() -> string[]`**

The names in order, as an array (the pre-iteration `keys()` form).

**`URLSearchParams.valuesArray() -> string[]`**

The values in order, as an array (the pre-iteration `values()` form).

**`URLSearchParams.entriesArray() -> Array<[string, string]>`**

The pairs in order, as an array (the pre-iteration `entries()` form).

```js
import { URLSearchParams } from "dyna:url";

const p = new URLSearchParams("a=1&b=x%20y&a=2");
p.size;              // 3
p.getAll("a");       // ["1", "2"]
p.set("b", "z");
p.toString();        // "a=1&a=2&b=z"
for (const [k, v] of p) console.log(k, v);   // a 1, a 2, then b z
console.log([...p.keys()]);                  // ["a", "a", "b"]
console.log(JSON.stringify(p.entriesArray()));// [["a","1"],["a","2"],["b","z"]]
```

### IDNA & Punycode

**`domainToASCII(domain[, options]) -> string`**

- `domain` *(string)* — a Unicode domain.
- `options.transitional` *(boolean, default `false`)* — selects transitional processing. **STRICT:** the bag is `{transitional}` only; an unknown key throws — the two modes disagree on exactly the names where the option matters, so a typo must not silently pick one.

Returns the ASCII form, lowercasing on the ASCII fast path. A domain that fails IDNA validation throws naming the stage.

**`domainToUnicode(domain[, options]) -> string`**

- `domain` *(string)* — an ASCII or Punycode domain.

Returns the reverse mapping, decoding Punycode labels back to Unicode.

**`punycodeEncode(text) -> string`**

- `text` *(string)* — the text to encode.

Returns the RFC 3492 encoding. Input over 1024 code points is refused (a DNS label is at most 63 octets).

**`punycodeDecode(text) -> string`**

- `text` *(string)* — the Punycode text to decode.

Returns the decoded text. Input over 1024 octets or malformed Punycode throws.

```js
import { domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode } from "dyna:url";

const ascii = domainToASCII("münchen.de");
const unicode = domainToUnicode("xn--mnchen-3ya.de");
const enc = punycodeEncode("bücher");
const dec = punycodeDecode("bcher-kva");
```

### Form Encoding

**`formEncode(obj) -> string`**

- `obj` *(object)* — the object to encode.

Returns the percent-encoded query string (`a=1&b=2`). Encodes the object's own enumerable string keys; undefined values are skipped.

**`formDecode(text) -> object`**

- `text` *(string)* — a form-encoded string.

Returns the decoded object. Decodes `+` as space, keeps the LAST value per key, and skips a leading `?`. Keys are defined, so `__proto__=x` cannot retarget the prototype.

**`encodeURIComponentStrict(text) -> string`**

- `text` *(string)* — the text to encode.

Returns the encoded string. Like `encodeURIComponent` but also escapes `!'()~`, which several servers treat as delimiters.

```js
import { formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";

const qs = formEncode({ a: 1, b: "x y" });
const parsed = formDecode("a=1&b=x+y");
const strict = encodeURIComponentStrict("a b!'()~");
```

---

# dyna:oauth2

OAuth 2.0 helpers (RFC 6749 + 6750 + 7636): PKCE S256, state CSRF, bearer auth, `WWW-Authenticate`, scope and `redirect_uri` checks, and the authorization-code flow's URL builders and parsers. Pure utilities — no IO. JWT verification (`verifyJWT`) is the one function that reaches into `dyna:crypto`; it needs `CONFIG_TLS=y` and otherwise throws `not compiled in`.

`import { generateCodeVerifier, isValidCodeVerifier, generateCodeChallenge, verifyCodeChallenge, generateState, secureCompare, buildClientAuthHeader, verifyJWT, parseScope, formatScope, buildBearerHeader, parseBearerHeader, parseBearerFromRequest, isValidBearerToken, buildWWWAuthenticate, isValidRedirectUri, buildAuthorizationUrl, parseAuthorizationResponse, buildTokenRequestBody, parseTokenResponse } from "dyna:oauth2";`

### PKCE (RFC 7636)

**`generateCodeVerifier() -> string`**

Returns 43 unreserved characters from 32 CSPRNG octets.

**`isValidCodeVerifier(s) -> boolean`**

- `s` *(string)* — the verifier to check.

True for 43..128 unreserved characters.

**`generateCodeChallenge(verifier, method = "S256") -> string`**

- `verifier` *(string)* — the code verifier.
- `method` *(string, default `"S256"`)* — `"S256"` or `"plain"`.

`S256` = base64url of the SHA-256 digest; `plain` echoes the verifier.

**`verifyCodeChallenge(verifier, challenge, method) -> boolean`**

- `verifier` *(string)* — the code verifier.
- `challenge` *(string)* — the challenge to compare.
- `method` *(string)* — the challenge method.

Recomputes and compares constant-time.

### State CSRF and comparisons

**`generateState(bytes = 32) -> string`**

- `bytes` *(number, default 32)* — the CSPRNG byte count.

Returns a base64url state token.

**`secureCompare(a, b) -> boolean`**

- `a`, `b` — strings or byte views.

Constant-time comparison over UTF-8 (or raw bytes for a `ByteView`).

**`buildClientAuthHeader(clientId, clientSecret?) -> string`**

- `clientId` *(string)* — the client identifier.
- `clientSecret` *(string, optional)* — the client secret.

`Basic` header per RFC 6749: form-encodes `id:secret`, then base64.

### Bearer tokens (RFC 6750)

**`buildBearerHeader(token) -> string`**

- `token` *(string)* — the token.

Returns `Bearer <token>`.

**`parseBearerHeader(header) -> string|null`**

- `header` *(string)* — the `Authorization` header value.

Returns the token, or `null` when the header is not `Bearer <token>`.

**`parseBearerFromRequest(req) -> string|null`**

- `req` *(object)* — `{ headers?, query?, body?, allowQuery?, allowBody? }`.

Reads the bearer token from `headers.authorization`, or (opt-in) `query`/`body`; returns `null` when absent.

**`isValidBearerToken(token) -> boolean`**

- `token` *(string)* — the token.

Validates the base64url `b64token` grammar.

**`buildWWWAuthenticate({ realm?, error?, errorDescription?, error_description?, errorUri?, error_uri?, scope? }) -> string`**

- All options optional; both the camel and snake spellings of `error_description`/`error_uri` are accepted.

Returns a `WWW-Authenticate: Bearer ...` challenge value.

### Scope (RFC 6749 A.4)

**`parseScope(scope) -> string[]`**

- `scope` *(string)* — a space-delimited scope string.

**`formatScope(scopes) -> string`**

- `scopes` *(string[])* — the scopes.

Returns the NQCHAR-token space-delimited form.

### Redirect URIs (RFC 8252 )

**`isValidRedirectUri(candidate, registered) -> boolean`**

- `candidate` *(string)* — the URI to check.
- `registered` *(string[])* — the registered URIs.

Verbatim exact match, except a loopback candidate may differ from its registered form in PORT (the OAuth-for-native-apps port-ignore rule).

### The authorization-code flow

**`buildAuthorizationUrl({ authorizationEndpoint, clientId, redirectUri, responseType?, scope?, state?, codeChallenge?, codeChallengeMethod?, allowPlain?, allowInsecure?, extraParams? }) -> { url, state? }`**

- All fields except the first three are optional; `responseType` defaults to `"code"`.
- `allowPlain` permits a `plain` code challenge (default false); `allowInsecure` permits an `http://` authorization endpoint.

Builds the authorize URL, auto-generating `state` when absent (returned in the result).

**`parseAuthorizationResponse(url) -> { code?, state?, error?, errorDescription?, errorUri? }`**

- `url` *(string)* — the redirect URL.

Parses the `code`/`state`/`error` query (the code flow puts them in the query, not the fragment).

**`buildTokenRequestBody(params) -> string`**

- `params` *(object)* — the token-request parameters.

Returns an `application/x-www-form-urlencoded` body.

**`parseTokenResponse(body) -> object`**

- `body` *(string)* — the token endpoint's JSON body.

Parses it, refusing a `token_type` other than `Bearer`.

**`verifyJWT(token, key, { algorithms, aud?, iss?, requiredScope?, clockSkewSec? }) -> payload`**

- `token` *(string)* — a JWS compact token.
- `key` — the verification key.
- `algorithms` *(string[])* — the required allowlist.
- `aud`, `iss`, `requiredScope` — claims to check; `clockSkewSec` (default 60) bounds the `exp`/`nbf` comparison.

Verifies the signature via `dyna:crypto` and the `exp`/`nbf`/`aud`/`iss`/`scope` claims. Needs `CONFIG_TLS=y` (otherwise throws `not compiled in`).

```js
import { generateCodeVerifier, isValidCodeVerifier, generateCodeChallenge, verifyCodeChallenge, secureCompare, parseScope, formatScope, buildBearerHeader, parseBearerHeader, isValidBearerToken, buildWWWAuthenticate, isValidRedirectUri, buildAuthorizationUrl, parseAuthorizationResponse, buildTokenRequestBody, parseTokenResponse, generateState } from "dyna:oauth2";

// PKCE — the RFC 7636 Appendix B vector pins the S256 transform
const verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
print(generateCodeChallenge(verifier, "S256") === "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"); // true
print(isValidCodeVerifier(verifier), isValidCodeVerifier("short"));   // true false
const mine = generateCodeVerifier();
print(verifyCodeChallenge(mine, generateCodeChallenge(mine, "S256"), "S256")); // true

// state + constant-time compare
print(generateState().length >= 32, secureCompare("abc", "abc"), secureCompare("abc", "abd")); // true true false

// bearer plumbing
print(buildBearerHeader("mF_9.B5f-4.1JqM"));                          // Bearer mF_9.B5f-4.1JqM
print(parseBearerHeader("Bearer mF_9.B5f-4.1JqM"));                   // mF_9.B5f-4.1JqM
print(isValidBearerToken("mF_9.B5f-4.1JqM"), isValidBearerToken("bad token")); // true false
print(buildWWWAuthenticate({ realm: "api", error: "invalid_token" })); // Bearer realm="api", error="invalid_token"

// scope and redirects
print(parseScope("read write").join(","), formatScope(["read", "write"])); // read,write read write
print(isValidRedirectUri("https://client.example.com/cb", ["https://client.example.com/cb"])); // true
print(isValidRedirectUri("http://127.0.0.1:54321/cb", ["http://127.0.0.1:8080/cb"])); // true (loopback port ignored)

// the authorize round trip
const { url, state } = buildAuthorizationUrl({
    authorizationEndpoint: "https://auth.example.com/authorize",
    clientId: "s6BhdRkqt3",
    redirectUri: "https://client.example.com/cb",
    scope: "read write",
    codeChallenge: generateCodeChallenge(verifier, "S256")
});
print(url.includes("response_type=code"), url.includes("state="), typeof state === "string"); // true true true
const back = parseAuthorizationResponse("https://client.example.com/cb?code=abc&state=" + state);
print(back.code, back.state === state);                              // abc true
print(buildTokenRequestBody({ grant_type: "authorization_code", code: "abc" }));
// grant_type=authorization_code&code=abc
print(JSON.stringify(parseTokenResponse(JSON.stringify({ access_token: "at", token_type: "Bearer" }))));
// {"access_token":"at","token_type":"Bearer"}
```

---

# dyna:validate

String and data validation predicates. Every function takes one string and returns a boolean; a non-string argument throws.

`import { IsAlpha, IsAlphanumeric, IsAscii, IsBase64, IsBase64Url, IsCreditCard, IsDateString, IsDomain, IsE164, IsEmail, IsHex, IsHexColor, IsIBAN, IsIP, IsIPv4, IsIPv6, IsJSON, IsJWT, IsMimeType, IsNumeric, IsPort, IsRFC3339, IsSemver, IsSlug, IsStrongPassword, IsURL, IsUUID } from "dyna:validate";`

### Char Classes

**`IsAlpha(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is ASCII letters only. An empty string satisfies no class.

**`IsAlphanumeric(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is ASCII letters and digits. An empty string satisfies no class.

**`IsAscii(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether every byte is below 0x80. An empty string satisfies no class.

```js
import { IsAlpha, IsAlphanumeric, IsAscii } from "dyna:validate";

const a = IsAlpha("abc");
const an = IsAlphanumeric("abc123");
const ascii = IsAscii("plain");
```

### E-mail

**`IsEmail(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input matches the practical grammar, not full RFC 5322: one unquoted atext local part (at most 64 chars, no leading/trailing dot, no `..`), one dotted domain with a letters-only TLD of at least two chars, total length 3..254. RFC 5322's quoted strings and comments are refused because real mail systems do not round-trip them.

```js
import { IsEmail } from "dyna:validate";

const ok = IsEmail("user@example.com");
```

### Cards & IBAN

**`IsCreditCard(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the Luhn check digit passes over 12..19 digits. Spaces and hyphens are ignored, and the check proves transcription care, not that the card exists.

**`IsIBAN(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the IBAN validates. Length is per country from the IBAN registry (an IBAN of the wrong length for its country is invalid even when the digits check out), then a mod-97 over the rearranged digits. Spaces are stripped and letters uppercased.

```js
import { IsCreditCard, IsIBAN } from "dyna:validate";

const card = IsCreditCard("4111111111111111");
const iban = IsIBAN("DE89 3704 0044 0532 0130 00");
```

### Domain & URL

**`IsDomain(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input matches RFC 1035 label grammar over the whole input: at least one dot, each label 1..63 chars with no edge hyphens, total 3..253 octets. An IP literal is not a domain.

**`IsURL(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the `dyna:url` constructor accepts it, plus the lenient-corner check that special schemes (`http`, `https`, `ws`, `wss`, `ftp`) carry a non-empty host, so `"https://"` fails.

```js
import { IsDomain, IsURL } from "dyna:validate";

const d = IsDomain("example.com");
const url = IsURL("https://example.com");
```

### Tokens

**`IsSlug(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is lowercase letters, digits, and single hyphens (no leading, trailing, or doubled hyphen), at most 64 chars.

**`IsUUID(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is the RFC 4122 canonical `8-4-4-4-12` form only (36 chars), with the hex and dash positions decided by the `dyna:uuid` parser; braces and URN prefixes are refused.

**`IsJWT(text, { algs }?) -> boolean`**

- `text` *(string)* — the input.
- `algs` *(string[], optional)* — when given, the token must also name one of these algorithms in its header (exact string match, e.g. `["ES256", "RS256"]`); a token with any other `alg` is false.

Returns whether the input is JWS Compact Serialization (RFC 7515): three non-empty, unpadded base64url segments whose header and claims are JSON objects, the header naming an `alg`.

**`IsSemver(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the `dyna:semver` parser accepts it.

**`IsE164(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is ITU-T E.164: an optional `+` and 8 to 15 digits, nothing else.

```js
import { IsSlug, IsUUID, IsJWT, IsSemver, IsE164 } from "dyna:validate";

const slug = IsSlug("hello-world");
const uuid = IsUUID("123e4567-e89b-12d3-a456-426614174000");
const jwt = IsJWT("eyJhbGciOiJub25lIn0.eyJzdWIiOiIxIn0.sig");
const sem = IsSemver("1.2.3");
const phone = IsE164("+14155552671");
```

### Addresses

**`IsIPv4(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is a strict dotted-quad: exactly four decimal octets, each 0..255, with no leading zeros (`"127.000.000.001"` is refused outright -- some resolvers read such octets as octal, so its meaning is ambiguous), no whitespace, no sign.

**`IsIPv6(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input matches the RFC 4291 text grammar: eight 1-4 hex-digit groups, `::` compressing one or more zero groups (at most once), and the embedded IPv4 tail form (`::ffff:1.2.3.4`) only at the end. Group leading zeros are grammar-legal (`"0001"`); zone ids (`"fe80::1%eth0"`) are refused -- a zone is interface metadata, not address text.

**`IsIP(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is an IPv4 or IPv6 address.

**`IsPort(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is a TCP/UDP port: a decimal string of value 0..65535, no sign, no leading zeros. `"0"` is allowed (bind it and the kernel assigns an ephemeral port).

```js
import { IsIPv4, IsIPv6, IsIP, IsPort } from "dyna:validate";

const v4 = IsIPv4("127.0.0.1");
const v6 = IsIPv6("2001:db8::8a2e:370:7334");
const anyIp = IsIP("::ffff:1.2.3.4");
const port = IsPort("443");
```

### Encodings

**`IsBase64(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is RFC 4648 base64 in canonical form: the standard alphabet (`+/`), `=` only as the final one or two characters, and the discarded pad bits zero (`"AR=="` decodes, but what it decodes to depends on the decoder's leniency, so a validator refuses it). Unpadded input is refused -- the unpadded spelling is base64url's. Empty is false.

**`IsBase64Url(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is RFC 4648 sec.5 base64url, unpadded (the RFC 7515 rule JWT segments follow): letters, digits, `-` and `_`, no `=` anywhere -- and canonical, exactly like IsBase64. A final lone character (`"n"`, length = 1 mod 4) or non-zero wasted bits in a 2- or 3-character final quantum (`"QQR"`, `"a-"`) is refused: which byte the tail decodes to depends on the decoder's leniency. Empty is false.

**`IsHex(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is an even-length run of hex digits -- the shape a byte string must have; an odd length is refused because no nibble-oriented consumer can eat it. Empty is false.

**`IsHexColor(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is `#RGB`, `#RRGGBB` or `#RRGGBBAA`. The `#` is required (named colors are not hex), and the 4-digit `#RGBA` short form is not accepted -- spell the alpha out. Case-insensitive, like CSS.

**`IsJSON(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the engine's own JSON parser accepts the input; any JSON value counts (object, array, string, number, bool, null). Inputs over 4096 bytes throw a RangeError rather than parse.

```js
import { IsBase64, IsBase64Url, IsHex, IsHexColor, IsJSON } from "dyna:validate";

const b64 = IsBase64("aGVsbG8=");
const b64u = IsBase64Url("eyJhbGciOiJIUzI1NiJ9");
const hex = IsHex("deadBEEF");
const color = IsHexColor("#ff8800");
const json = IsJSON('{"ok":true}');
```

### Media types & dates

**`IsMimeType(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is an RFC 6838 media type, simplified to the subset real headers use: `type/subtype`, each half 1..127 chars from the RFC 6838 restricted-name charset (ASCII letters, digits, and the specials `!#$&-^_.+`; the remaining RFC 2045 token extras ``%'*`{|}~`` are tolerated too), then optional `; name=value` parameters whose names come from that charset (never repeated, case-insensitively) and whose values are drawn from it or are quoted strings. Linear whitespace is allowed around the `;` separators, nowhere else.

**`IsRFC3339(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is a full RFC 3339 date-time: a real calendar date, a `T` separator (lowercase `t`/`z` are RFC-legal), `HH:MM:SS` with the leap second `:60` allowed, an optional fractional second, and an offset of `Z` or `+/-HH:MM`. A space separator is refused -- that spelling needs agreement the RFC does not give.

**`IsDateString(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is a strict ISO calendar date `YYYY-MM-DD` with the month and day actually checked (leap years included, 400-year rule and all).

**`IsNumeric(text) -> boolean`**

- `text` *(string)* — the input.

Returns whether the input is an optionally signed decimal: digits with at most one decimal point and at least one digit (`.5` and `5.` pass; `-` alone does not). Exponent notation is refused on purpose -- this screens ledger/spreadsheet input, where the literal decimal form is what survives a CSV round-trip.

```js
import { IsMimeType, IsRFC3339, IsDateString, IsNumeric } from "dyna:validate";

const mime = IsMimeType("text/html; charset=utf-8");
const when = IsRFC3339("2023-01-15T10:30:00Z");
const day = IsDateString("2024-02-29");
const num = IsNumeric("-1.5");
```

### Passwords

**`IsStrongPassword(text, opts?) -> boolean`**

- `text` *(string)* — the input.
- `opts` *(object, optional)* — `minLength`, `minLower`, `minUpper`, `minDigits`, `minSymbols`, all optional non-negative integers (default 8/1/1/1/1). A value that is not a number throws a TypeError; a negative, fractional or over-4096 value throws a RangeError.

Returns whether the password meets the class minimums. A "symbol" is a printable non-alphanumeric ASCII character; a space counts toward the length only. The input must be printable ASCII outright -- a non-ASCII byte (a Cyrillic letter, say) refuses the password rather than guess its class -- and an empty string is never a strong password. A second argument that is not an object is ignored.

```js
import { IsStrongPassword } from "dyna:validate";

const ok = IsStrongPassword("Str0ng!pass");
const relaxed = IsStrongPassword("aaaaaaaa", { minLower: 8, minUpper: 0,
                                               minDigits: 0, minSymbols: 0 });
```

---

# dyna:xml

One scanner, three front ends: streaming SAX, a document tree, and a plain-object collapse — plus a streaming writer on the way out. All bounds are on by default: a token over 16 MiB or a document over 256 MiB is refused, and the tree front ends cap nesting at 256. The streaming `XmlWriter` is the documented exception: it never builds a tree, so its nesting cap is 65536 and exists only as a runaway guard.

`import { XMLParse, XMLStringify, XMLToObject, SAXParser, XmlWriter } from "dyna:xml";`

### Document Tree

**`XMLParse(text[, options]) -> element | element[]`**

- `text` *(string)* — the XML document.
- `options.trim` *(boolean, default `true`)* — whether to trim text.
- `options.entities` *(string, default `"strict"`)* — `"strict"` | `"keep"`.
- `options.multiple` *(boolean, default `false`)* — return EVERY root element as an array (parity with HTMLParse's fragment array) instead of demanding exactly one.

Options bags are strict: an unknown key in `XMLParse`'s or `XMLStringify`'s options throws a `TypeError` naming the key and the valid set (`unknown option "multipe" (valid: multiple, trim, entities)`). The `SAXParser` handlers object deliberately keeps accepting extra keys: its single argument is a handler registry that merely reuses the parse options for `entities` — that positional/handlers duality is the module's one documented exception.

Returns `{ name, attrs, children }` — attrs an object (attribute values normalize a literal tab/newline to space), children an array of strings and elements. Strict refuses anything but the five predefined entities and numeric refs; `keep` passes an unknown `&foo;` through. Exactly one root element is enforced unless `multiple: true` collects them all. Document-level strictness is unchanged either way: malformed input still throws, and a document with no element at all still throws in both modes. Attribute names are defined, so `__proto__` in a document stays an own property.

**`XMLStringify(node[, options]) -> string`**

- `node` *(object)* — a `{ name, attrs, children }` node.
- `options.indent` *(number, default `0`)* — 0..16 spaces; default is minified.

Returns the serialized XML. Nesting beyond 256 and names that are not valid element names throw.

**`XMLToObject(node) -> object`**

- `node` *(object)* — an element from `XMLParse`.

Returns a plain object keyed by element name. Collapses an element: attributes become `@attr` keys, a single child element becomes a value, repeated child names become an array. Element names like `__proto__` are read as own properties only.

```js
import { XMLParse, XMLStringify, XMLToObject } from "dyna:xml";

const el = XMLParse('<book id="1"><title>T</title><tag>a</tag><tag>b</tag></book>');
const text = XMLStringify({ name: "a", attrs: { b: "1" }, children: ["text"] });
const obj = XMLToObject(el);
const roots = XMLParse('<item>1</item><item>2</item>', { multiple: true });
```

### XmlWriter

**`new XmlWriter({ indent? })`**

- `indent` *(number, default `0`)* — 0..16 spaces per level; 0 is minified. The type is strict: a non-integer (`"3"`, `3.5`) throws a `TypeError`, an integer outside the range throws `RangeError`. The options bag is strict: an unknown key throws a `TypeError` naming the key and the valid set (`unknown option "indnt" (valid: indent)`).

A streaming document writer for output the tree + `XMLStringify` round-trip cannot produce: no tree is ever built. The writer holds an incremental byte buffer plus a heap stack of open element names, so memory tracks the bytes produced and the depth — a multi-megabyte or 300-deep document costs its output size, not nodes. One writer produces one document; elements close innermost-first.

**`writer.open(name, attrs?)`**

- `name` *(string)* — a valid XML element name (anything else throws `TypeError`/`RangeError`).
- `attrs` *(object, optional)* — attribute values, escaped (quotes included); an invalid attribute name throws.

Writes `<name attrs>` (or `<name attrs>`-with-values) and pushes the element. Nesting beyond 65536 throws `RangeError` — deliberately NOT the serializer's 256, since deep output is what this class exists for. The start tag is atomic: any throw while building it — an invalid attribute name, an illegal character in a value, or an attribute getter that throws — writes NOTHING and pushes nothing, leaving the writer exactly as it was (later writes still produce well-formed output).

Attribute values and text refuse C0 control characters (other than tab/LF/CR): they are not legal XML 1.0 characters and cannot even be escaped, so a value carrying one throws `RangeError` instead of emitting a document no strict consumer accepts.

**`writer.text(s)`**

- `s` *(string)* — text to append.

Appends text with `&`, `<`, `>` escaped. Quotes are legal raw in text and stay as sent.

**`writer.close()`**

Writes `</name>` for the innermost open element. With nothing open it throws `RangeError` — an unmatched close is a document bug, not a no-op.

**`writer.toString() -> string`**

The bytes so far, WITHOUT closing anything — a checkpoint, not a finalize.

**`writer.finish() -> string`**

Closes every remaining open element (innermost-first) and returns the complete document. Idempotent: one writer, one document, the buffer never truncates.

```js
import { XmlWriter, XMLParse } from "dyna:xml";
const w = new XmlWriter({ indent: 2 });
w.open("book", { id: "1" });
w.open("title");
w.text("T <raw>");
w.close();
w.close();
const doc = w.finish();
console.log(doc);                          // <book id="1">\n  <title>T &lt;raw&gt;\n  </title>\n</book>
console.log(XMLParse(doc).attrs.id);       // 1
const deep = new XmlWriter();
for (let i = 0; i < 300; i++) deep.open("d" + (i % 10));
for (let i = 0; i < 300; i++) deep.close();
console.log(deep.finish().length > 1500);  // true -- 300 deep past the builder's 256
```

### Streaming SAX

**`new SAXParser(handlers) -> SAXParser`**

- `handlers.onOpen(name, attrs)` *(function)* — optional.
- `handlers.onClose(name)` *(function)* — optional.
- `handlers.onText(text)` *(function)* — optional.
- `handlers.onCData(text)` *(function)* — optional.
- `handlers.onComment(text)` *(function)* — optional.
- `handlers.onPI(target, data)` *(function)* — optional.

Returns a new `SAXParser`. Each handler must be a function or absent.

**Push and pull** — the form is fixed by the constructor's argument shape. Passing a handlers object (even `{}`) is a **push** parser: `write()`/`end()` scan synchronously, handlers fire during the scan, and malformed input throws from `write()`/`end()` — the historic contract, unchanged. Constructing with NO handlers argument (or `undefined`/`null`) is a **pull** parser: `write()` only buffers, `next()` scans on demand and returns one event object at a time, and `end()` is lazy — it only finalizes, so a pull parser's errors surface from `next()` (sticky: every later `next()` re-throws) and the end-of-document checks run at the last `next()`. A pull parser's options come from the second constructor argument (`new SAXParser(undefined, { entities: "keep" })`). Mixing is safe and defined: a push parser's handlers fire whenever the scanner runs, including from a `next()` call, but a push `write()` never leaves events pending, so the forms never double-deliver.

**`SAXParser.next() -> event | null`**

No parameters.

Returns the next event object, or `null` while the input is drained — feed more with `write()`, or the document is finished. The event is `{ event, name?, attrs?, text?, offset }`: `event` is `"open" | "close" | "text" | "cdata" | "comment" | "pi"`; `name` rides open/close/pi; `attrs` rides open; `text` rides text/cdata/comment and carries the PI's data. `offset` is the event's starting byte offset in the stream AS FED — absolute across chunks, matching a string chunk's UTF-8 bytes or a byte view's indices. Errors throw and are sticky.

**`SAXParser.iterate(source[, options]) -> AsyncGenerator<event>`**

- `source` *(sync or async iterable)* — chunks of the document (strings or byte views).
- `options.entities` *(string)* — as above.

Returns an async-iterable wrapper over its own no-handler (pull) parser: `for await (const ev of SAXParser.iterate(src))` feeds each chunk and surfaces one event object per step. Chunk boundaries may fall anywhere — a token interrupted mid-way resumes. Malformed input rejects the iteration; a missing source is a `TypeError`.

**`SAXParser.write(chunk)`**

- `chunk` *(string | byte view)* — a chunk of the document.

Feeds a string or any byte view; a token interrupted by a chunk boundary resumes where it stopped. `write()` from inside a handler, `write()` after `end()`, and a `write()` with no chunk are all refused.

**`SAXParser.end()`**

No parameters.

Finalizes the stream; trailing content after the last element throws a syntax error.

```js
import { SAXParser } from "dyna:xml";

const seen = [];
const p = new SAXParser({
    onOpen: (name, attrs) => seen.push(["open", name]),
    onText: (t) => seen.push(["text", t]),
    onClose: (name) => seen.push(["close", name])
});
p.write("<a x=\"1\">hi</a>");
p.end();

const pulled = new SAXParser();
pulled.write("<a x=\"1\">hi</a>");
for (let ev; (ev = pulled.next()) !== null;)
    console.log(ev.event, ev.offset, ev.name ?? ev.text ?? "");
pulled.end();

for await (const ev of SAXParser.iterate(["<r>", "<b/>", "</r>"]))
    seen.push(ev.event);
```

---

# dyna:yaml

The YAML 1.2 core schema, block and flow, and nothing else: anchors, aliases, tags, merge keys, and directives are refused by name rather than ignored — a config parser that silently drops an anchor returns a wrong document.

`import { Parse, ParseAll, Stringify } from "dyna:yaml";`

### Parsing

**`Parse(text[, options]) -> value`**

- `text` *(string)* — the YAML document.
- `options.maxDepth` *(number, 1..128)* — lower the nesting cap; the 128 default is the fuzzer-hardening bound and a C-stack guard, so it cannot be raised from JS.
- `options.schema` *(string, default `"core"`)* — `"core"` is the only implemented schema; `"full"` is refused by name with a `RangeError` rather than silently parsed as core.

Returns the parsed value. Parses exactly one document. Multi-document input is refused with a pointer to `ParseAll`; input over 64 MiB and nesting over the depth cap throw. There is deliberately no `maxAliases` option: anchors and aliases are refused outright, which is stricter than any alias cap could be. The bag is fully strict — wrong types are `TypeError`s, and an unknown KEY throws one naming the key and the valid set (`unknown option "dep" (valid: maxDepth, schema)`).

**`ParseAll(text[, options]) -> array`**

- `text` *(string)* — the YAML input.
- `options` — the same bag as `Parse`.

Returns an array. Parses every `---`-separated document into an array.

**`Stringify(value[, options]) -> string`**

- `value` *(any)* — the value to serialize.
- `options.indent` *(number, default `2`)* — 1..10 spaces.
- `options.width` *(number, default `0`)* — 0 means never fold, which is the emitter's historic behavior byte-for-byte, so the default output does not change. Above 0, a STRING value whose single-line form exceeds `width` is emitted as a double-quoted scalar folded across lines with backslash continuations; every break contributes NOTHING to the value, so the re-parse is exact (the reader accepts precisely this continuation form and nothing more). The first line carries up to `width` columns of the scalar's own form (the `key: ` prefix is not counted), continuation lines up to `width - indent`; a budget under 16 clamps to 16 rather than breaking per byte, and a word longer than the whole budget is split mid-word with a continuation (still exact) when no space falls in the window. Columns are bytes of the UTF-8 form, which is conservative for wide characters. Keys are never folded — a key must stay one line — and flow style is never folded. Integer 0..65536; wrong types are `TypeError`s.
- `options.sortKeys` *(boolean, default `false`)* — mapping keys sort by UTF-8 byte order (not locale collation), recursively, in block and flow style.
- `options.flow` *(boolean, default `false`)* — every collection is emitted in flow style (`[a, b]`, `{k: v}`), recursively, `, `-separated; empty collections stay `[]`/`{}`; a plain-safe string stays bare. Flow lines may exceed `width`.

Options bags are strict: an unknown key in `Stringify`'s options throws a `TypeError` naming the key and the valid set (`unknown option "indnt" (valid: indent, width, sortKeys, flow)`). `null`/`undefined`/primitive options still mean "no options".

Returns the serialized YAML document. Nesting over 128 throws.

**`ParseStream(text[, options]) -> Iterator<value>`**

- `text` *(string)* — the multi-document YAML input.
- `options` — the same bag as `Parse`; option errors are labeled `ParseStream`.

Returns a sync iterator yielding the `---`-separated documents one at a time, with `ParseAll`'s strictness — same markers, same refusals, and an empty document between markers yields `null` exactly as `ParseAll` stores one. The input is the whole text (the line table is built up front, as for `Parse`); what the iterator buys is LAZY DOCUMENTS, not incremental feeding: only one parsed document is alive at a time, so a 100-document stream no longer materializes all 100 at once, and malformed document N is reported AS document N. Per-document errors are `SyntaxError`s carrying the 1-based document index and the line number within the whole text (`ParseStream: document 3: ... at line 17`); after one, the iterator is dead — every further `next()` re-throws the same error. `return()` closes early (a `for-of` break); `next()` after done stays done.

```js
import { Parse, ParseAll, ParseStream, Stringify } from "dyna:yaml";

const cfg = Parse("name: DynaJS\nport: 8080\nlist:\n  - a\n  - b");
const docs = ParseAll("---\na: 1\n---\nb: 2\n");
const text = Stringify({ name: "DynaJS", port: 8080 });
const sorted = Stringify({ b: 1, a: 2 }, { sortKeys: true });
const flow = Stringify([1, { k: "v" }], { flow: true });
let n = 0;
for (const doc of ParseStream("---\na: 1\n---\nb: 2\n")) n += 1;
```

# dyna:mathx

Numerical library: C99 `libm` passthroughs, MATLAB-tier special functions, big-integer and number theory, fixed-width bit primitives, statistics over typed arrays and number[] (`stats`), and a compiled arithmetic expression evaluator.

`import { E, Pi, Phi, Ln2, Ln10, Log2E, Log10E, Sqrt2, SqrtE, SqrtPi, MaxInt32, MinInt32, MaxInt64, MaxSafeInteger, realmin, realmax, flintmax, eps, round, roundToEven, fix, sign, signbit, trunc, modf, mod, rem, fmod, remainder, idivide, nthroot, gamma, cbrt, hypot, copysign, nextafter, expm1, log1p, log2, logb, scalbn, ldexp, ilogb, frexp, isInf, isNaN, pow2, deg2rad, rad2deg, nextpow2, erf, erfc, erfinv, erfcinv, erfcx, lgamma, gammaln, beta, betaln, psi, polygamma, gammainc, gammaincinv, betainc, betaincinv, expint, besselj, bessely, besseli, besselk, besseliScaled, besselkScaled, besselh, ellipke, ellipj, legendre, legendreP, airy, isPrime, factor, primes, gcd, lcm, factorial, abs, bitLen, popcount, nchoosek, perms, rat, linspace, logspace, cumsum, cumprod, diff, bits, stats, Expression } from "dyna:mathx";`

All double-valued functions coerce arguments with `ToFloat64`, so a BigInt argument throws `TypeError`; integer/BigInt functions take a Number or a BigInt and return a BigInt. Pure functions: nothing allocates per-call state that needs closing.

Where statistics live: scalar/array helpers here (`cumsum`/`cumprod`/`diff`, `linspace`); the columnar statistics — including the histogram (`DataFrame.HISTOGRAM`/`HISTOGRAM_NORMALIZED`, equal-width bins; dyna:mathx deliberately has no histogram of its own) — live on `dyna:dataframe`; `Array.prototype` carries the `number[]` conveniences (`sum`/`mean`/`min`/`max`/`median`/`product`).

### Constants

**`E -> number`, `Pi -> number`, `Phi -> number`, `Sqrt2 -> number`, `SqrtE -> number`, `SqrtPi -> number`, `Ln2 -> number`, `Ln10 -> number`, `Log2E -> number`, `Log10E -> number`**

Read-only, non-enumerable doubles, each written with enough digits that the compiler performs one correctly-rounded conversion. `Phi` is the golden ratio.

**`MaxInt32 -> number`, `MinInt32 -> number`, `MaxSafeInteger -> number`, `MaxInt64 -> bigint`**

Read-only, non-enumerable. `MaxInt32` is 2147483647, `MinInt32` is -2147483648, `MaxSafeInteger` is 2^53-1, and `MaxInt64` is 2^63-1.

```js
import { E, Pi, Phi, Ln2, Ln10, Log2E, Log10E, Sqrt2, SqrtE, SqrtPi, MaxInt32, MinInt32, MaxInt64, MaxSafeInteger } from "dyna:mathx";

print(E, Pi, Phi);                 // 2.718281828459045 3.141592653589793 1.618033988749895
print(Ln2, Ln10, Log2E, Log10E);
print(Sqrt2, SqrtE, SqrtPi);
print(MaxInt32, MinInt32, MaxInt64, MaxSafeInteger);
```

### Machine limits

**`realmin() -> number`**

Returns the smallest positive normal double (`DBL_MIN`).

**`realmax() -> number`**

Returns the largest finite double (`DBL_MAX`).

**`flintmax() -> number`**

Returns 2^53, the largest integer every double below it represents exactly.

**`eps(x) -> number`**

- `x` *(number)* — optional; defaults to 1, MATLAB's bare `eps` (2^-52).

Returns the gap to the next representable double away from zero — one ulp. It is correct for subnormals because it is `nextafter(|x|, +Inf) - |x|` rather than `2^(ilogb-52)`. NaN and infinite arguments give NaN.

```js
import { realmin, realmax, flintmax, eps } from "dyna:mathx";

print(realmin());                  // 2.2250738585072014e-308
print(realmax());                  // 1.7976931348623157e+308
print(flintmax());                 // 9007199254740992
print(eps());                      // 2.220446049250313e-16
print(eps(1000));                  // 1.1368683772161603e-13
```

### Rounding and signs

**`round(x) -> number`**

- `x` *(number)* — the value to round.

Returns C99 `round` (ties away from zero), deliberately NOT `Math.round` (ties toward +Inf): `round(-2.5)` is -3.

**`roundToEven(x) -> number`**

- `x` *(number)* — the value to round.

Rounds half to even, independent of the process FP rounding mode. It decomposes via `trunc`/`fmod`, exact for the values involved per Sterbenz's lemma.

**`fix(x) -> number`**

- `x` *(number)* — the value to truncate.

Truncates toward zero.

**`sign(x) -> number`**

- `x` *(number)* — the value to test.

Returns 1, -1, or `x` itself, so -0 and NaN pass through: `sign(-0)` is -0, `sign(NaN)` is NaN.

**`signbit(x) -> boolean`**

- `x` *(number)* — the value to test.

Reports the sign bit directly.

**`trunc(x) -> number`**

- `x` *(number)* — the value to truncate.

Returns C `trunc`.

**`modf(x) -> [intPart, fracPart]`**

- `x` *(number)* — the value to split.

Splits into integer and fractional parts. NaN gives `[NaN, NaN]`; `±Inf` gives `[±Inf, NaN]` with the same sign.

```js
import { round, roundToEven, fix, sign, signbit, trunc, modf } from "dyna:mathx";

print(round(-2.5));                // -3
print(roundToEven(2.5));           // 2
print(fix(-3.7));                  // -3
print(sign(-0));                   // -0
print(signbit(-0));                // true
print(modf(3.7));                  // [ 3, 0.7000000000000002 ]
```

### Modular and integer division

**`mod(a, b) -> number`**

- `a` *(number)* — the dividend.
- `b` *(number)* — the divisor.

Returns MATLAB's FLOORED modulo: `mod(-7, 3)` is 2, where C's `fmod(-7, 3)` is -1 — the whole reason both exist, since a signed-input caller reaching for a modulo silently gets the wrong convention. `mod(a, 0)` is `a`.

**`rem(a, b) -> number`**

- `a` *(number)* — the dividend.
- `b` *(number)* — the divisor.

Returns the truncated `fmod`.

**`fmod(a, b) -> number`**

- `a` *(number)* — the dividend.
- `b` *(number)* — the divisor.

Returns the C99 truncated remainder.

**`remainder(a, b) -> number`**

- `a` *(number)* — the dividend.
- `b` *(number)* — the divisor.

Returns the C99 round-to-nearest remainder.

**`idivide(a, b, mode) -> number`**

- `a` *(number)* — the dividend.
- `b` *(number)* — the divisor.
- `mode` *(string)* — one of `"fix"` (toward zero, the default and C's `/`), `"floor"`, `"ceil"`, or `"round"`.

Returns integer division with an explicit rounding mode, because "integer division" names four operations and C's truncation is only one of them. Division by zero follows IEEE (`±Inf`, or NaN for 0/0) rather than throwing.

**`nthroot(x, n) -> number`**

- `x` *(number)* — the radicand.
- `n` *(number)* — the root index.

Returns the real n-th root: defined for negative `x` with an odd integer `n`, which is the case it exists for (`nthroot(-8, 3)` is -2, where `pow(-8, 1/3)` is NaN); otherwise `pow(x, 1/n)`.

```js
import { mod, rem, fmod, remainder, idivide, nthroot } from "dyna:mathx";

print(mod(-7, 3));                 // 2
print(rem(-7, 3));                 // -1
print(fmod(-7, 3));                // -1
print(remainder(5, 2));            // 1
print(idivide(-7, 2));             // -3
print(idivide(-7, 2, "floor"));    // -4
print(idivide(-7, 2, "round"));    // -4
print(nthroot(-8, 3));             // -2
print(nthroot(16, 4));             // 2
```

### Elementary libm passthroughs

**`gamma(x) -> number`**

- `x` *(number)* — the argument.

Returns the gamma function (`tgamma`).

**`cbrt(x)`, `hypot(a, b)`, `copysign(a, b)`, `nextafter(a, b)`, `expm1(x)`, `log1p(x)`, `log2(x)`, `logb(x)`**

Direct libm calls.

**`pow2(x) -> number`**

- `x` *(number)* — the exponent.

Returns `2^x` (`exp2`), the inverse of `log2`.

**`deg2rad(x)`, `rad2deg(x)`**

Convert by the exact constants.

**`nextpow2(x) -> number`**

- `x` *(number)* — the value.

Returns the smallest `p` with `2^p >= |x|`. `nextpow2(0)` is 0; NaN and `±Inf` pass through.

**`scalbn(x, n) -> number`**

- `x` *(number)* — the significand.
- `n` *(number)* — the exponent.

Returns `x * 2**n`.

**`ldexp(frac, exp) -> number`**

- `frac` *(number)* — the significand.
- `exp` *(number)* — the exponent.

Identical to `scalbn`; kept as the MATLAB spelling.

**`frexp(x) -> [frac, exp]`**

- `x` *(number)* — the value.

Splits into `x = frac * 2**exp` with `|frac|` in [0.5, 1). 0, `±Inf` and NaN give `[x, 0]`.

**`ilogb(x) -> number`**

- `x` *(number)* — the value.

Classifies itself: `±Inf` and NaN give `2^31-1`, 0 gives `-(2^31)`, else the unbiased exponent.

**`isInf(x, sign=0) -> boolean`**

- `x` *(number)* — the value to test.
- `sign` *(number)* — optional; restrict to one sign (`sign > 0` positive, `sign < 0` negative).

Tests infinity.

**`isNaN(x) -> boolean`**

- `x` *(number)* — the value to test.

Tests NaN.

```js
import { gamma, hypot, copysign, nextafter, expm1, log1p, pow2, deg2rad, nextpow2, scalbn, ldexp, frexp, ilogb, isInf, isNaN } from "dyna:mathx";

print(gamma(0.5));                 // 1.772453850905516
print(hypot(3, 4));                // 5
print(copysign(3, -1));            // -3
print(nextafter(1, Infinity));     // 1.0000000000000002
print(expm1(1e-10));               // 1.0000000000827404e-10
print(log1p(1e-10));               // 9.9999999995e-11
print(pow2(3));                    // 8
print(deg2rad(180));               // 3.141592653589793
print(nextpow2(17));               // 5
print(scalbn(3, 4));               // 48
print(ldexp(0.5, 3));              // 4
print(frexp(8));                   // [ 0.5, 4 ]
print(ilogb(8));                   // 3
print(isInf(1/0, 1));              // true
print(isNaN(0/0));                 // true
```

### Error functions

**`erf(x)`, `erfc(x)`**

Libm.

**`erfinv(y) -> number`**

- `y` *(number)* — the erf value to invert, in [-1, 1].

Inverts `erf` by Giles' rational approximation refined with two Newton steps against the same libm `erf` the forward direction uses, so `erfinv(erf(x))` round-trips to ~1 ulp under whichever backend is linked. `erfinv(±1)` is `±Inf`; outside [-1, 1] it is NaN.

**`erfcinv(y) -> number`**

- `y` *(number)* — the erfc value to invert, in [0, 2].

Returns `erfinv(1 - y)`.

**`erfcx(x) -> number`**

- `x` *(number)* — the argument.

Returns the scaled `exp(x^2)*erfc(x)`, which stays finite where `erfc` underflows to 0. For `x >= 25` it switches to the asymptotic series so the multiplication never overflows.

```js
import { erf, erfinv, erfcinv, erfcx } from "dyna:mathx";

print(erf(1));                     // 0.8427007929497149
print(erfinv(0.5));                // 0.4769362762044699
print(erfcinv(0.5));               // 0.4769362762044699
print(erfcx(10));                  // 0.056140992743822594
```

### Gamma, beta, digamma and their inverses

**`lgamma(x) -> [value, sign]`**

- `x` *(number)* — the argument.

Returns `log|Gamma(x)|` and the sign of `Gamma(x)`, via the reentrant `lgamma_r` so it is thread-safe.

**`gammaln(x) -> number`**

- `x` *(number)* — the argument.

Returns the scalar `log|Gamma|` without the sign.

**`beta(a, b) -> number`**

- `a` *(number)* — the first argument.
- `b` *(number)* — the second argument.

Computed through lgamma, so it does not overflow for moderate arguments the way `tgamma(a)*tgamma(b)/tgamma(a+b)` does. It is `+Inf` when either argument is a non-positive integer.

**`betaln(a, b) -> number`**

- `a` *(number)* — the first argument.
- `b` *(number)* — the second argument.

Returns `log beta`.

**`psi(x) -> number`**

- `x` *(number)* — the argument.

Returns the digamma function. `psi(0)` is `±Inf` with the sign of `-x`; negative integers are poles and give NaN; `x < 0.5` uses the reflection `psi(1-x) - psi(x) = π cot(πx)`.

**`polygamma(n, x) -> number`**

- `n` *(number)* — the derivative order, in [0, 64].
- `x` *(number)* — the argument.

Returns the n-th derivative. `polygamma(0, x)` is identical to `psi(x)`; negative `x` gives NaN except through the recurrence.

**`gammainc(x, a, tail?) -> number`**

- `x` *(number)* — the integration limit, first in MATLAB's argument order.
- `a` *(number)* — the shape parameter.
- `tail` *(string)* — optional; `"upper"` selects the complement Q = 1 - P.

Returns the regularised incomplete gamma. The `"upper"` tail is computed directly rather than by subtraction, so it keeps its relative accuracy. Measured worst case 5.7e-16 for P and 2.9e-14 for the far upper tail.

**`gammaincinv(p, a) -> number`**

- `p` *(number)* — the target probability.
- `a` *(number)* — the shape parameter.

Inverts `P(a, x) = p` with Halley iterations from an Abramowitz–Stegun start. Measured worst-case round trip 5.8e-14.

**`betainc(x, a, b) -> number`**

- `x` *(number)* — the integration limit, first in MATLAB's argument order.
- `a` *(number)* — the first shape parameter.
- `b` *(number)* — the second shape parameter.

Returns the regularised incomplete beta.

**`betaincinv(p, a, b) -> number`**

- `p` *(number)* — the target probability.
- `a` *(number)* — the first shape parameter.
- `b` *(number)* — the second shape parameter.

Inverts the regularised incomplete beta with Halley iterations from a normal approximation. Measured worst-case round trip 1.2e-14.

**`expint(x) -> number`**

- `x` *(number)* — the argument, defined for `x > 0`.

Returns E1(x), computed by a series below 1 and a Lentz continued fraction above. `expint(0)` is `+Inf`; `x < 0` gives NaN.

```js
import { lgamma, gammaln, beta, betaln, psi, polygamma, gammainc, gammaincinv, betainc, betaincinv, expint } from "dyna:mathx";

print(lgamma(5));                  // [ 3.1780538303479458, 1 ]
print(gammaln(6));                 // 4.787491742782046
print(beta(2, 3));                 // 0.08333333333333333
print(betaln(2, 3));               // -2.4849066497880004
print(psi(1));                     // -0.5772156649015332
print(polygamma(1, 1));            // 1.6449340668482264
print(gammainc(2, 3));             // 0.32332358381693654
print(gammainc(2, 3, "upper"));    // 0.6766764161830634
print(gammaincinv(0.5, 3));        // 2.6740603137235603
print(betainc(0.5, 2, 3));         // 0.6875
print(betaincinv(0.5, 2, 3));      // 0.38572756813238956
print(expint(1));                  // 0.21938393439552029
```

### Bessel functions

**`besselj(n, x) -> number`**

- `n` *(number)* — an INTEGER order (libm `j0/j1/jn`; non-integer order is not offered rather than offered badly).
- `x` *(number)* — the argument.

Negative order folds by `J_-n = (-1)^n J_n`. libm's `jn` runs an O(n) recurrence, so an order above 16777216 throws unless the bound `|J_n(x)| <= (|x|/2)^n / n!` answers the result outright — that bound also short-circuits the recurrence when the answer is provably zero.

**`bessely(n, x) -> number`**

- `n` *(number)* — an INTEGER order (libm `y0/y1/yn`; non-integer order is not offered rather than offered badly).
- `x` *(number)* — the argument.

Returns the Bessel function of the second kind.

**`besseli(nu, x) -> number`**

- `nu` *(number)* — the real order.
- `x` *(number)* — the argument.

Uses an all-positive ascending series below `x = 20` and a Hankel asymptotic expansion above.

**`besselk(nu, x) -> number`**

- `nu` *(number)* — the real order.
- `x` *(number)* — the argument.

For `x >= 0.5` uses a trapezoid rule on `∫ exp(-x(cosh t - 1)) cosh(nu t) dt`, with the step narrowed as `0.25/√x` so the peak at t=0 stays sampled; below it uses series and connection forms with the stable downward-direction recurrence.

**`besseliScaled(nu, x) -> number`**

- `nu` *(number)* — the real order.
- `x` *(number)* — the argument.

Returns `I_nu(x) e^-x`, which stays finite where the plain function overflows.

**`besselkScaled(nu, x) -> number`**

- `nu` *(number)* — the real order.
- `x` *(number)* — the argument.

Returns `K_nu(x) e^x`, which stays finite where the plain function underflows.

**`besselh(n, x, kind) -> [re, im]`**

- `n` *(number)* — an INTEGER order.
- `x` *(number)* — the argument.
- `kind` *(number)* — `1` for the first kind, `2` for the second.

Returns the Hankel function `J_n ± i Y_n` as `[re, im]`.

```js
import { besselj, bessely, besseli, besselk, besseliScaled, besselkScaled, besselh } from "dyna:mathx";

print(besselj(1, 2));              // 0.5767248077568733
print(bessely(0, 1));              // 0.08825696421567697
print(besseli(0, 1));              // 1.2660658777520082
print(besselk(0, 1));              // 0.4210244382407083
print(besseliScaled(0, 10));       // 0.12783333716342862
print(besselkScaled(0, 1));        // 1.1444630798068949
print(besselh(1, 2, 1));           // [ 0.5767248077568733, -0.10703243154093756 ]
```

### Elliptic integrals

**`ellipke(m) -> [K, E]`**

- `m` *(number)* — the elliptic modulus, in [0, 1].

Returns the complete elliptic integrals of the first and second kind together — they share the AGM iteration, so the pair costs one pass (quadratic convergence, at most 30 iterations). `m > 1` gives NaN; `K(1)` is `+Inf` and `E(1)` is 1.

**`ellipj(u, m) -> {sn, cn, dn}`**

- `u` *(number)* — the argument.
- `m` *(number)* — the elliptic modulus, in [0, 1].

Evaluates the Jacobi elliptic functions by descending Landen transformations. The degenerate moduli `m < 1e-14` and `1-m < 1e-14` fall out to the circular and hyperbolic functions; values outside [0, 1] throw a RangeError.

```js
import { ellipke, ellipj } from "dyna:mathx";

print(ellipke(0.5));               // [ 1.8540746773013717, 1.3506438810476753 ]
print(ellipj(1, 0.5));             // { sn: 0.8030018248956439, cn: 0.5959765676721407, dn: 0.8231610016315963 }
```

### Legendre functions

**`legendre(n, x) -> number[]`**

- `n` *(number)* — the degree, an integer in [0, 1024].
- `x` *(number)* — the argument.

Returns the associated Legendre functions `P_n^m(x)` for the whole column `m = 0..n`, which is what a spherical-harmonic caller wants and what the upward recurrence produces on the way to any single value (O(n) with the Condon–Shortley phase). `|x| > 1` or `m > n` gives NaN.

**`legendreP(n, m, x) -> number`**

- `n` *(number)* — the degree, capped at 150 because the recurrence overflows float64 past ~150.
- `m` *(number)* — the order.
- `x` *(number)* — the argument.

Returns the single value for when the whole column is not needed.

```js
import { legendre, legendreP } from "dyna:mathx";

print(legendre(2, 0.5));           // [ -0.125, -1.299038105676658, 2.25 ]
print(legendreP(2, 1, 0.5));       // -1.299038105676658
```

### Airy functions

**`airy(x) -> {ai, aip, bi, bip}`**

- `x` *(number)* — the argument.

Returns all four values from one evaluation. For `x >= 0.1` they are positive combinations of the modified Bessel functions above (no cancellation anywhere on that half of the domain); between -7 and 0.1 the Maclaurin series runs, and below -7 an oscillatory asymptotic expansion takes over. The regime switch points are chosen so both methods agree at the seam.

```js
import { airy } from "dyna:mathx";

print(JSON.stringify(airy(0)));    // {"ai":0.3550280538878172,"aip":-0.2588194037928068,"bi":0.6149266274460007,"bip":0.4482883573538264}
```

### Number theory

**`isPrime(n) -> boolean`**

- `n` *(number)* — the integer to test.

Deterministic Miller–Rabin with the 12-witness set `{2..37}`, proven sufficient for every `n < 3.317e24` — zero probabilistic error over the whole uint64 domain.

**`factor(n) -> number[]`**

- `n` *(number)* — an integer in [1, 2^53].

Returns the ascending prime factors with multiplicity by trial division to `sqrt(n)`, ample for the integers <= 2^53 a double holds exactly. `factor(1)` is `[]`; anything outside [1, 2^53] throws.

**`primes(n) -> number[]`**

- `n` *(number)* — the upper bound.

Returns every prime `<= n` by sieve, up to a hard limit of 5e7 (thrown past it). `n < 2` gives `[]`.

```js
import { isPrime, factor, primes } from "dyna:mathx";

print(isPrime(97));                // true
print(isPrime(91));                // false
print(factor(84));                 // [ 2, 2, 3, 7 ]
print(primes(20));                 // [ 2, 3, 5, 7, 11, 13, 17, 19 ]
```

### Big-integer arithmetic

**`gcd(a, b) -> bigint`**

- `a` *(number | bigint)* — the first integer.
- `b` *(number | bigint)* — the second integer.

Returns the greatest common divisor by Euclid's algorithm.

**`lcm(a, b) -> bigint`**

- `a` *(number | bigint)* — the first integer.
- `b` *(number | bigint)* — the second integer.

Divides first so the intermediate never overflows, and spills to a full-width BigInt past 2^64 — two 64-bit inputs can lcm to 128 bits.

**`factorial(n) -> bigint`**

- `n` *(number)* — a non-negative integer, capped at 10000 as a bound on worst-case work (a RangeError past it, not a precision limit).

Returns `n!` exactly via a base-10^9 limb accumulator past 64 bits.

**`abs(n) -> bigint`**

- `n` *(bigint)* — a BigInt only; a Number throws `TypeError`.

Returns the magnitude as unsigned, so `abs(-(2^63))` is `2^63`.

**`bitLen(n) -> number`**

- `n` *(bigint)* — the value.

Returns the minimum bits needed to represent its magnitude (`bitLen(255n)` is 8, `bitLen(0n)` is 0).

**`popcount(n) -> number`**

- `n` *(bigint)* — the value.

Counts set bits in the magnitude.

```js
import { gcd, lcm, factorial, abs, bitLen, popcount } from "dyna:mathx";

print(gcd(48, 36));                // 12n
print(lcm(6, 8));                  // 24n
print(factorial(20));              // 2432902008176640000n
print(factorial(100));             // 93326215443944152681699238856266700490715968264381621468592963895217599993229915608941463976156518286253697920827223758251185210916864000000000000000000000000n
print(abs(-42n));                  // 42n
print(bitLen(255n));               // 8
print(popcount(0b101101n));        // 4
```

### Combinatorics

**`nchoosek(n, k) -> number`**

- `n` *(number)* — a non-negative integer.
- `k` *(number)* — a non-negative integer.

Returns the binomial coefficient, built multiplicatively so the intermediate never exceeds the result — `n!/(k!(n-k)!)` overflows a double at n=171 while the answer is still exact. `k > n` gives 0.

**`perms(v) -> number[][]`**

- `v` *(array)* — the input, at most 8 elements (9! rows is past useful).

Emits every permutation of the input in reverse lexicographic order to match MATLAB; each row is a fresh array.

**`rat(x, tol=1e-6) -> [num, denom]`**

- `x` *(number)* — the value to approximate.
- `tol` *(number)* — optional relative tolerance, default 1e-6.

Finds a rational approximation by continued fractions within relative tolerance `tol`.

```js
import { nchoosek, perms, rat } from "dyna:mathx";

print(nchoosek(10, 3));            // 120
print(perms([1, 2]));              // [ [ 2, 1 ], [ 1, 2 ] ]
print(rat(0.333));                 // [ 333, 1000 ]
print(rat(Math.PI));               // [ 355, 113 ]
```

### Vector generators and reductions

**`linspace(a, b, n=100) -> number[]`**

- `a` *(number)* — the first point.
- `b` *(number)* — the last point.
- `n` *(number)* — optional point count, default 100.

Returns `n` points inclusive of both ends, with the last point set to `b` exactly.

**`logspace(a, b, n=100) -> number[]`**

- `a` *(number)* — the first exponent.
- `b` *(number)* — the last exponent.
- `n` *(number)* — optional point count, default 100.

Returns `10^t` over the same grid as `linspace`.

**`cumsum(v) -> number[]`**

- `v` *(array)* — the input values.

Returns the running sum.

**`cumprod(v) -> number[]`**

- `v` *(array)* — the input values.

Returns the running product.

**`diff(v) -> number[]`**

- `v` *(array)* — the input values.

Returns adjacent differences, one element shorter.

Point counts are bounded at 1e8.

```js
import { linspace, logspace, cumsum, cumprod, diff } from "dyna:mathx";

print(linspace(0, 1, 5));          // [ 0, 0.25, 0.5, 0.75, 1 ]
print(logspace(1, 3, 3));          // [ 10, 100, 1000 ]
print(cumsum([1, 2, 3, 4]));       // [ 1, 3, 6, 10 ]
print(cumprod([1, 2, 3, 4]));      // [ 1, 2, 6, 24 ]
print(diff([1, 3, 6, 10]));        // [ 2, 3, 4 ]
```

### stats — reductions over typed arrays and number[]

`stats` is a namespace object: `stats.mean(f64a)`. It is the middle layer of the statistics stack — the same reductions DataFrame carries, over a bare `Float64Array` without constructing a frame:

- **Array.prototype** (`sum`/`mean`/`min`/`max`/`median`) — `number[]` convenience, plain accumulators.
- **`mathx.stats`** (this section) — `Float64Array` fast path (read directly, no per-element machinery) plus the missing half: `variance`, `stddev`, `quantile`, `cov`, `corr`. `number[]` and array-likes take a per-element fallback.
- **DataFrame** (`dyna:dataframe`) — columnar + masked, the full set (`VARIANCE`/`STDDEV`/`SKEW`/`KURTOSIS`/`CORR`/`COV`/`QUANTILE(S)`/`DESCRIBE`/`HISTOGRAM`).

Related overlaps, cross-referenced: binning lives on `DataFrame.HISTOGRAM`, not here; `linspace` (n points, inclusive ends) is the closed-grid twin of `Number.range` (exclusive end stepper). Related items on Array.prototype: `uniq`/`unique` and `average`/`mean` duplicates are documented at the prototype extensions.

Contracts that differ from the neighbours, on purpose (none are bugs):

- `sum` is Neumaier-compensated: `stats.sum` of 1e7 copies of 0.1 is exactly 1000000, where a plain accumulator drifts to 999999.9998389754. The same sequential fold runs on both input paths, so a `Float64Array` and a `number[]` holding identical values answer bit-for-bit identically.
- A partial sum that overflows to `Infinity` is reported as `NaN` (the compensation term becomes Inf-Inf), not `Inf`.
- `quantile`/`median` use the exact `DataFrame.QUANTILE`/`MEDIAN` convention (R-7: position `q*(n-1)`, linear between the closest ranks); an EMPTY input throws `RangeError` where DataFrame answers `undefined`, and a NaN element answers `NaN` where DataFrame's columnar mask drops it — `stats.median([1, NaN, 3])` is NaN, `DataFrame.MEDIAN` is 2: a bare array has no mask, so there is nothing to drop the NaN against (order statistics of unordered input are undefined). Interpolated quantiles agree with `DataFrame.QUANTILE` bit-for-bit (both kernels contract the interpolation the same way); against an exact R-7 reference they sit within 1 ULP.
- `corr` throws `RangeError` on zero variance (constant input, or fewer than two rows) and on length mismatch, where `DataFrame.CORR` answers NaN; over a bare array a constant input is usually a feeding bug. NaN inputs answer `NaN`.
- `min`/`max` follow `Math.min`/`Math.max`: NaN poisons, an empty input answers `+Infinity`/`-Infinity`, and of two equal zeros `-0` wins min and `+0` wins max.

**`stats.sum(a) -> number`, `stats.mean(a) -> number`**

- `a` *(Float64Array | number[] | array-like)*

`sum` of the empty array is `+0`; `mean` is `sum / n`, so the empty array is `0/0 = NaN`.

**`stats.variance(a, opts?) -> number`, `stats.stddev(a, opts?) -> number`**

- `a` *(Float64Array | number[] | array-like)*
- `opts` *(object)* — `{ pop: true }` divides by `n` (population); the default is the sample form dividing by `n-1`.

Two-pass (mean first, then squared deviations, compensated). Sample variance of fewer than two observations is NaN; the population variance of a single observation is 0. `stddev` is `sqrt` under the same gates.

**`stats.median(a) -> number`, `stats.quantile(a, q) -> number`**

- `a` *(Float64Array | number[] | array-like)*
- `q` *(number)* — in [0, 1]; outside the range or NaN throws `RangeError`.

`median` is `quantile(a, 0.5)`. Sorting is over a scratch copy; the input is never mutated.

**`stats.min(a) -> number`, `stats.max(a) -> number`**

`Math.min`/`Math.max` semantics: any NaN element gives NaN; the empty array gives `+Infinity` / `-Infinity`.

**`stats.cov(a, b, opts?) -> number`, `stats.corr(a, b) -> number`**

- `a`, `b` *(Float64Array | number[] | array-like)* — equal lengths; a mismatch throws `RangeError` (the inputs may be mixed, one typed array and one `number[]`).
- `opts` *(object)* — `{ pop: true }` for the population covariance, sample (n-1) by default.

`corr` is the Pearson coefficient, computed from compensated moments and clamped into [-1, 1].

```js
import { stats } from "dyna:mathx";

const f = new Float64Array([1, 2, 3, 4, 5]);
print(stats.sum(f));               // 15
print(stats.mean(f));              // 3
print(stats.variance(f));          // 2.5
print(stats.variance(f, { pop: true })); // 2
print(stats.median([3, 1, 2]));    // 2
print(stats.quantile(f, 0.25));    // 2
print(stats.corr([1, 2, 3], [2, 4, 6]) > 0.999999);  // true
print(stats.cov([1, 2, 3], [2, 4, 6]));  // 2
```

### bits — fixed-width bit primitives

**`bits.uintSize`** — 64.

`bits` is a namespace object holding width-parameterised bit manipulation. Every argument is coerced and masked to its width: the 8/16/32-bit operations take a Number and return a Number; the 64-bit operations take a BigInt and return a BigInt (or a pair of them). Zero is special-cased per primitive rather than left to undefined builtins: `leadingZeros(0)` and `trailingZeros(0)` are the width, `len(0)` is 0.

**`bits.leadingZeros8(x)`, `bits.leadingZeros16(x)`, `bits.leadingZeros32(x)`, `bits.leadingZeros64(x)`**

Count leading zeros.

**`bits.trailingZeros8(x)`, `bits.trailingZeros16(x)`, `bits.trailingZeros32(x)`, `bits.trailingZeros64(x)`**

Count trailing zeros.

**`bits.onesCount8(x)`, `bits.onesCount16(x)`, `bits.onesCount32(x)`, `bits.onesCount64(x)`**

Count set bits.

**`bits.len8(x)`, `bits.len16(x)`, `bits.len32(x)`, `bits.len64(x)`**

Minimum bits to represent `x` (`len(0)` = 0).

**`bits.reverse8(x)`, `bits.reverse16(x)`, `bits.reverse32(x)`, `bits.reverse64(x)`**

Reverse bit order; the 64-bit form returns a BigInt.

**`bits.reverseBytes16(x)`, `bits.reverseBytes32(x)`, `bits.reverseBytes64(x)`**

Swap byte order (endianness).

**`bits.rotateLeft8(x, k)`, `bits.rotateLeft16(x, k)`, `bits.rotateLeft32(x, k)`, `bits.rotateLeft64(x, k)`**

Rotate left by `k`, reducing `k` modulo the width so a negative `k` rotates right and any `k` wraps.

**`bits.rotateRight8(x, k)`, `bits.rotateRight16(x, k)`, `bits.rotateRight32(x, k)`, `bits.rotateRight64(x, k)`**

Rotate right by `k` — the named twin of `rotateLeft`, added for readability at call sites that think in rotations. The `k` normalization is mirrored exactly: `k` reduces modulo the width, so a negative `k` rotates LEFT (by `-k`), any `k` wraps, and `bits.rotateRightW(x, k)` is always `bits.rotateLeftW(x, -k)`.

**`bits.add32(a, b, carry) -> [sum, carryOut]`, `bits.add64(a, b, carry)`**

Widening add with carry in and out; the 64-bit form takes BigInts.

**`bits.sub32(a, b, borrow)`, `bits.sub64(a, b, borrow)`**

Subtract with borrow.

**`bits.mul32(a, b) -> [hi, lo]`, `bits.mul64(a, b)`**

Full-width product, high word first.

**`bits.div32(hi, lo, y) -> [quo, rem]`, `bits.div64(hi, lo, y)`**

Divide the double-width value `hi:lo` by `y`, throwing RangeError on `y == 0` or `y <= hi` (quotient overflow).

**`bits.rem32(hi, lo, y) -> rem`, `bits.rem64(hi, lo, y)`**

Return only the remainder; throw only on `y == 0`.

```js
import { bits } from "dyna:mathx";

print(bits.leadingZeros8(0));      // 8
print(bits.trailingZeros8(8));     // 3
print(bits.onesCount8(0b101101));  // 4
print(bits.len16(0xFF));           // 8
print(bits.reverse8(0b10110000));  // 13
print(bits.reverseBytes16(0x1234));// 13330
print(bits.rotateLeft8(0b11000001, 2));  // 7
print(bits.rotateLeft8(8, -1));    // 4
print(bits.rotateRight8(0b11000001, 2)); // 112
print(bits.rotateRight8(8, -1));   // 16
print(bits.add32(0xFFFFFFFF, 1, 0)); // [ 0, 1 ]
print(bits.mul32(0xFFFFFFFF, 0xFFFFFFFF)); // [ 4294967294, 1 ]
print(bits.div32(0, 10, 3));       // [ 3, 1 ]
print(bits.mul64(0xFFFFFFFFFFFFFFFFn, 2n)); // [ 1n, 18446744073709551614n ]
print(bits.leadingZeros64(1n));    // 63
```

### Expression — compiled arithmetic

**`new Expression(text)`**

- `text` *(string)* — the arithmetic expression to compile.

Compiles an arithmetic string to an RPN program over doubles with shunting-yard; there is no eval and no scope — an identifier is a variable or one of 29 named functions (`sin cos tan asin acos atan sinh cosh tanh sqrt cbrt exp log log2 log10 abs floor ceil round trunc sign expm1 log1p atan2 pow hypot min max fmod`). `^` is right-associative and above unary minus, so `-x^2` is `-(x^2)` and `2^3^2` is 512. Throws SyntaxError on a bad expression, RangeError past 4096 source bytes.

**`Expression.prototype.variables() -> string[]`**

Lists the free variables in first-use order.

**`Expression.prototype.eval(vars) -> number`**

- `vars` *(object)* — optional; required when the program has variables.

Evaluates, reading only OWN data properties (a getter throws — the one thing this evaluator exists not to run). A program with variables needs the `vars` object; one without takes no argument.

```js
import { Expression } from "dyna:mathx";

const e = new Expression("a * x^2 + b");
print(e.variables());              // [ "a", "x", "b" ]
print(e.eval({ a: 2, x: 3, b: 1 })); // 19
const p = new Expression("2^3^2");
print(p.eval());                   // 512
const s = new Expression("sin(x) + sqrt(y)");
print(s.eval({ x: 0, y: 16 }));    // 4
```

---

# dyna:matcher

Substring and multi-pattern search: a compiled single-pattern `Matcher`, an Aho-Corasick `MultiMatcher`, edit distance and bigram similarity, and Myers diff. All offsets are code-unit offsets, so a match found at byte offset `k` in Latin-1 or UTF-8 is reported as its `k`-th character position.

`import { Matcher, MultiMatcher, Levenshtein, DiceCoefficient, JaroWinkler, DamerauLevenshtein, DiffChars, DiffWords, DiffLines } from "dyna:matcher";`

### Matcher

**`new Matcher(pattern, { algo? })`**

- `pattern` *(string)* — the pattern to compile.
- `algo` *(string)* — optional; `"kmp"` or `"bmh"`/`"boyer-moore"`, anything else throws.

Copies the pattern once and decides at construction whether every pattern byte is below 0x80. A pure-ASCII pattern searches the subject's narrow bytes directly with no scan, copy or translation (byte offset is code-unit offset, and no false match is possible against Latin-1); a non-ASCII pattern goes through a UTF-8 copy with offset translation. `algo` is parsed, validated and reported back by the getter, but searching always routes through the SIMD `strfind` kernel (measured 6.8–15 GB/s against scalar KMP's 463–493 MB/s), so construction is O(pattern length) and each scan is a linear pass over the text. A 1024-byte pattern constructs in ~119 ns; the crossover where a compiled `Matcher` beats `String.prototype.indexOf` is about N=100 searches.

**`Matcher.prototype.firstIn(text, fromIndex?) -> number`**

- `text` *(string)* — the subject.
- `fromIndex` *(number)* — optional start offset, in code units.

Returns the code-unit offset of the first match at or after `fromIndex`, or -1. The conventions are `String.prototype.indexOf`'s: a negative `fromIndex` is clamped to 0, `fromIndex` past the end answers -1 (an empty pattern answers `min(fromIndex, length)`, like `indexOf("", pos)`), and `NaN` coerces to 0.

**`Matcher.prototype.test(text) -> boolean`**

- `text` *(string)* — the subject.

Answers existence.

**`Matcher.prototype.countIn(text) -> number`**

- `text` *(string)* — the subject.

Counts matches.

**`Matcher.prototype.allIn(text) -> number[]`**

- `text` *(string)* — the subject.

Lists every match offset.

An empty pattern occurs at position 0 — `firstIn` returns 0, `test` returns true, and `countIn`/`allIn` stay empty rather than enumerating every position.

**`Matcher.prototype.replaceAllIn(text, repl) -> string`**

- `text` *(string)* — the subject.
- `repl` *(string)* — the replacement.

Replaces matches non-overlapping, left to right — the only sane rule once bytes are being removed — allocating the output exactly once after a counting pass.

**`Matcher.prototype.length -> number`** and **`Matcher.prototype.algo -> string`**

Read-only getters.

```js
import { Matcher } from "dyna:matcher";

const m = new Matcher("abc");
print(m.firstIn("xxabcyyabc"));          // 2
print(m.test("xxabcyy"));                // true
print(m.countIn("abcabcabc"));           // 3
print(m.allIn("xxabcyyabc"));            // [ 2, 7 ]
print(m.replaceAllIn("xxabcyyabc", "Z"));// xxZyyZ
print(m.length, m.algo);                 // 3 kmp
const bm = new Matcher("abc", { algo: "bmh" });
print(bm.algo);                          // bmh
```

### MultiMatcher

**`new MultiMatcher(patterns[])`**

- `patterns` *(array)* — one or more patterns (max 65536); empty patterns are refused.

Builds a byte-trie Aho-Corasick automaton: each state holds 256 goto slots (1 KiB per state) plus suffix `fail` links and an output chain, so all patterns are found in ONE pass whose cost does not grow with N — the compiled capability `Matcher` only looks like. Below ~36 patterns, N calls to `indexOf` are faster; the automaton wins above. An empty pattern matches everywhere and nowhere, which is why it is refused. Every pattern is materialised to libc memory before the automaton is touched, so a getter in the pattern array cannot observe a half-built machine.

**`MultiMatcher.prototype.firstIn(text) -> {index, at} | null`**

- `text` *(string)* — the subject.

Returns the pattern index and offset of the earliest hit.

**`MultiMatcher.prototype.test(text) -> boolean`**

- `text` *(string)* — the subject.

Tests whether any pattern occurs.

**`MultiMatcher.prototype.countIn(text) -> number`**

- `text` *(string)* — the subject.

Counts every emitted hit; overlapping matches and multiple patterns ending at the same position each count.

**`MultiMatcher.prototype.allIn(text) -> [{index, at}]`**

- `text` *(string)* — the subject.

Returns them all.

**`MultiMatcher.prototype.size -> number`** and **`MultiMatcher.prototype.states -> number`**

Read-only getters: `size` is the pattern count, `states` the number of automaton states.

**`MultiMatcher.prototype.replaceAllIn(text, repl) -> string`**

- `text` *(string)* — the subject.
- `repl` *(string)* — the replacement, or a function `(match, patternIndex) => replacement`.

Parity with `Matcher.prototype.replaceAllIn`'s rule — non-overlapping, left to right — over the whole pattern set in ONE pass. Hits are consumed in `allIn`'s emission order (by end position; longest pattern first at one end), and a hit is replaced only when it starts at or after the previous replacement's end, so overlapping hits keep their first (longest) claim. The callback receives the matched text and the pattern's index — the same two fields `allIn` reports — and its result is `ToString`'d like `String.prototype.replace`. The subject is snapshotted before the scan because the callback runs JS mid-scan.

```js
import { MultiMatcher } from "dyna:matcher";

const mm = new MultiMatcher(["he", "she", "hers"]);
print(mm.size);                          // 3
print(mm.states);                        // 8
print(mm.test("ushers"));                // true
print(JSON.stringify(mm.firstIn("ushers"))); // {"index":1,"at":1}
print(mm.countIn("ushers"));             // 3
print(JSON.stringify(mm.allIn("ushers")));
// [{"index":1,"at":1},{"index":0,"at":2},{"index":2,"at":2}]
```

### Edit distance and similarity

**`Levenshtein(a, b, { max? }) -> number`**

- `a` *(string)* — the first string.
- `b` *(string)* — the second string.
- `max` *(number)* — optional cutoff.

Returns the exact edit distance in code points. The shorter side is the pattern: at most 64 code points it runs the Myers bit-parallel kernel, one machine word of state per text element (O(n)); beyond that a banded two-row DP costs O(band · n). Work is bounded at 4e8 DP cells — a throw rather than a 16-second burn — and each operand is capped at 16 MiB. With `{ max }` the answer is exact while `<= max` and is `max + 1` once it exceeds it, so `d <= max` is always a correct "within max" test; `max` also narrows the DP band but can never bypass the cell budget. Malformed UTF-8 throws.

**`DiceCoefficient(a, b) -> number`**

- `a` *(string)* — the first string.
- `b` *(string)* — the second string.

Scores string similarity in [0, 1] by bigram multiset intersection, O(|a|+|b|) over an open-addressed hash set (each left bigram consumed at most once, so "aa" vs "aaaa" scores below 1). Two contract surprises are intentional: ASCII whitespace is stripped first, and a side under two characters scores 0 unless the sides are equal. Byte-identical input short-circuits to 1 before decoding.

**`JaroWinkler(a, b) -> number`**

- `a` *(string)* — the first string.
- `b` *(string)* — the second string.

Scores [0, 1] by the Jaro similarity with Winkler's common-prefix boost: `l = min(common prefix, 4)`, scale `0.1`, applied when the Jaro score exceeds the 0.7 boost threshold — the standard definition (Winkler 1990), threshold included. Code-point operands, like Levenshtein; the window walk is `O(n · window)` and is bounded by the same 4e8-cell budget.

**`DamerauLevenshtein(a, b, { max? }) -> number`**

- `a` *(string)* — the first string.
- `b` *(string)* — the second string.
- `max` *(number)* — optional cutoff.

The restricted Damerau-Levenshtein — the OPTIMAL STRING ALIGNMENT variant: a transposition counts as one edit, but a transposed pair may not take part in another edit (`ca` vs `abc` is 3, not 2). Three-row banded DP with the same `{ max }` contract and cell budget as Levenshtein.

```js
import { Levenshtein, DiceCoefficient, JaroWinkler, DamerauLevenshtein } from "dyna:matcher";

print(Levenshtein("kitten", "sitting"));         // 3
print(Levenshtein("kitten", "sitting", { max: 2 })); // 3 (max + 1)
print(Levenshtein("café", "cafe"));              // 1
print(DiceCoefficient("night", "nacht"));        // 0.25
print(DiceCoefficient("hello", "hello"));        // 1
print(DiceCoefficient("a", "b"));                // 0
print(JaroWinkler("MARTHA", "MARHTA").toFixed(4));      // 0.9611
print(JaroWinkler("DIXON", "DICKSONX").toFixed(4));     // 0.8133
print(DamerauLevenshtein("abcd", "acbd"));       // 1 (one transposition)
print(DamerauLevenshtein("ca", "abc"));          // 3 (OSA, not 2)
print(DamerauLevenshtein("kitten", "sitting"));  // 3
```

### Line, word and character diffs

**`DiffChars(a, b) -> [{op, text}]`**

- `a` *(string)* — the source text.
- `b` *(string)* — the target text.

Produces a Myers diff (middle-snake, O(ND)) tokenised by character, in source order.

**`DiffWords(a, b) -> [{op, text}]`**

- `a` *(string)* — the source text.
- `b` *(string)* — the target text.

Produces a Myers diff (middle-snake, O(ND)) tokenised by word, in source order.

**`DiffLines(a, b) -> [{op, text}]`**

- `a` *(string)* — the source text.
- `b` *(string)* — the target text.

Produces a Myers diff (middle-snake, O(ND)) tokenised by line, in source order.

Each hunk carries `op`: -1 deleted from `a`, 1 inserted from `b`, 0 common. Concatenating the hunks with `op != 1` rebuilds `a`, and with `op != -1` rebuilds `b` — that property is the test's oracle. Identical input answers on the bytes with a single common hunk, needing no tokenisation, interning or search; more than ~2^16 tokens per side throws.

```js
import { DiffChars, DiffLines, DiffWords } from "dyna:matcher";

print(JSON.stringify(DiffChars("ab", "acb")));  // [{"op":0,"text":"a"},{"op":1,"text":"c"},{"op":0,"text":"b"}]
print(JSON.stringify(DiffLines("a\nb", "a\nc")));
// [{"op":0,"text":"a\n"},{"op":-1,"text":"b"},{"op":1,"text":"c"}]
print(JSON.stringify(DiffWords("quick brown", "quick red")));
// [{"op":0,"text":"quick "},{"op":-1,"text":"brown"},{"op":1,"text":"red"}]
```

# dyna:structures

Native containers and graph algorithms — everything the language lacks a builtin for: bit sets, disjoint sets, deques, lists, ring buffers, heaps, tries, ordered maps and sets, range trees, the Guava/Commons collections, and the probabilistic sketches. Every class is a plain GC-managed object (no `.close()`); a finalizer frees its native arrays when it becomes unreachable. Keyed containers take keys as byte strings: compared byte for byte, never interpreted, so an embedded NUL is an ordinary key byte. Containers that hold JavaScript values mark every one of them on each GC mark cycle, so a container holding millions of values makes each GC pause proportional to its size; prefer plain JS objects for huge static datasets.

`import { Graph, LRU, Heap, SortedSet, SortedMap, Deque, List, RingBuffer, BitSet, UnionFind, Fenwick, SegTree, BloomFilter, Trie, Multiset, Multimap, BiMap, Table, RangeSet, RangeMap, IntervalTree, MinMaxHeap, CountMinSketch, HyperLogLog, BTree } from "dyna:structures";`

### Graph

Adjacency-list graph over integer node ids `0..n-1`. `addNode` returns the next id; `addEdge` auto-grows to include its endpoints. Every result is copied into fresh JS arrays/objects; nothing native escapes.

```js
import { Graph } from "dyna:structures";

const g = new Graph({ directed: true, weighted: true });
const a = g.addNode(), b = g.addNode(), c = g.addNode();
g.addEdge(a, b, 2).addEdge(b, c, 3);
console.log(g.dijkstra(a));       // [0, 2, 5]
console.log(g.aStar(a, c, n => Math.abs(n - c)));   // { dist: 5, path: [0,1,2] }
console.log(g.topologicalSort()); // [0,1,2]
```

**`new Graph(options?)`**

  - `directed` *(boolean, default `false`)* — edges are directed.
  - `weighted` *(boolean, default `false`)* — edges carry weights.

Creates an adjacency-list graph over integer node ids. Options are read once at construction. The bag is strict: an unknown key throws a `TypeError` naming the key and the valid set (`unknown option "weight" (valid: directed, weighted)`) — `{weight: true}` would otherwise silently create an unweighted graph — while an absent, `null` or primitive options argument still means "no options".

**`Graph.addNode() -> number`**

Appends an isolated node and returns its id.

**`Graph.addEdge(u, v, w?) -> this`**

- `u` *(number)* — the source node id.
- `v` *(number)* — the target node id.
- `w` *(number, default `1`)* — the edge weight.

Adds a directed edge (or both directions, if the graph is undirected); nodes grow on demand. A non-finite weight throws `RangeError`; on an unweighted graph `w` is forced to 1.0.

**`Graph.neighbors(u) -> number[]`**

- `u` *(number)* — the node id.

Returns the targets of `u`'s outgoing edges. Throws for a node id out of range.

**`Graph.neighborsInto(u, out) -> number`** —

- `u` *(number)* — the node id.
- `out` *(Int32Array)* — the caller's buffer, written in place.

Writes `u`'s out-neighbors into `out` and returns the count (the degree). This is the "into" contract: never allocates, so a traversal that reads every hop through a reused buffer allocates nothing per step. A buffer shorter than the degree throws `RangeError` *before* anything is written; extra capacity is left untouched. Throws `TypeError` for a non-`Int32Array` buffer and `RangeError` for an out-of-range node.

**`Graph.exportCSR() -> { offsets: Int32Array, edges: Int32Array }`** —

Returns the adjacency in compressed-sparse-rows form, node-ordered: `edges[offsets[u] .. offsets[u+1]]` is exactly `neighbors(u)`, `offsets` has `n + 1` entries. The two arrays are FRESH copies (the export copies once and never aliases the native adjacency), so mutating a result cannot corrupt the graph, and the pair feeds dyna:simd kernels or a hand-rolled walk directly. An empty graph exports `{ offsets: Int32Array([0]), edges: Int32Array([]) }`.

```js
const h = new Graph({ directed: true });
h.addEdge(0, 1).addEdge(0, 2).addEdge(1, 3);
const csr = h.exportCSR();
for (let u = 0; u < csr.offsets.length - 1; u++)
    for (let p = csr.offsets[u]; p < csr.offsets[u + 1]; p++)
        console.log(u, csr.edges[p]);   // every edge, node-ordered
```

**`Graph.hasEdge(u, v) -> boolean`**

- `u` *(number)* — the source node id.
- `v` *(number)* — the target node id.

Returns false if `u` is out of range or no such edge exists.

**`Graph.nodeCount -> number`**

The number of nodes. Getter.

**`Graph.edgeCount -> number`**

The number of `addEdge` calls (an undirected edge is one call). Getter.

**`Graph.bfs(src) -> number[]`**

- `src` *(number)* — the start node id.

Returns the breadth-first visit order. Throws `RangeError` for an out-of-range source.

**`Graph.bfs(src, { visit }) -> number`** —

- `visit` *(function `(node) -> boolean | void`)* — called once per node, in the same order the array form returns.

Visitor form: the walk stops the moment `visit` returns true (a falsy or `undefined` return keeps going) and the number of nodes visited — counting the one that returned true — is returned. Only the traversal working set (queue + seen bits) is allocated, once per call: nothing per node, so a scan of a million-edge graph allocates no JS garbage. A `visit` throw unwinds the traversal cleanly and propagates; the graph is unaffected. A `visit` that MUTATES the graph (`addNode`/`addEdge`) is refused with `TypeError: bfs: the visitor mutated the graph` — the working set is sized once, so the walk aborts rather than read stale buffers; the mutation itself is not rolled back (same semantics as `aStar`'s mutating-heuristic refusal). `bfs(src, undefined)` keeps the array form.

```js
const h = new Graph({ directed: true });
h.addEdge(0, 1).addEdge(1, 2).addEdge(2, 3);
// stop at the first node past 60 (early exit: later nodes are never visited)
const visited = h.bfs(0, { visit: (u) => u > 60 });
console.log(visited, "nodes visited");
```

**`Graph.dfs(src) -> number[]`**

- `src` *(number)* — the start node id.

Returns the depth-first visit order; neighbours are visited so smaller ids come first.

**`Graph.dfs(src, { visit }) -> number`** —

Same contract as the `bfs` visitor overload, walking in dfs order — including the mutating-visitor refusal (`dfs: the visitor mutated the graph`).

**`Graph.topologicalSort({ visit }) -> number`** —

Visitor form of `topologicalSort()`: visits in topological order, early-exits when `visit` returns true, and returns the visited count. A cycle reached before the walk stops still throws `RangeError`; stopping early deliberately skips the check for the part of the order never reached. A `visit` that mutates the graph is refused (`topologicalSort: the visitor mutated the graph`; the mutation persists). Requires a directed graph as before.

**`Graph.dijkstra(src, dst?) -> number[] | number`**

- `src` *(number)* — the source node id.
- `dst` *(number, optional)* — a target node; when given, returns the single distance to it.

Returns the shortest distances from `src` (`Infinity` for unreachable nodes). Any negative edge anywhere throws `RangeError` (checked up front, not per-search). O(E log V).

**`Graph.bellmanFord(src) -> number[]`**

- `src` *(number)* — the source node id.

Returns the shortest distances from `src`, tolerating negative edges. Throws `RangeError` on a negative cycle. O(V·E).

**`Graph.topologicalSort() -> number[]`**

Returns a topological order via Kahn's algorithm. Requires a directed graph (`TypeError` otherwise) and throws `RangeError` on a cycle.

**`Graph.connectedComponents() -> number[]`**

Returns the component id per node (0-based). Deliberately answers *weak* components on a directed graph.

**`Graph.floydWarshall() -> number[][]`**

Returns the all-pairs distance matrix; `Infinity` marks unreachable pairs. Refused for `n > 1024` (`RangeError`, the triple loop is O(n³)); throws on a negative cycle.

**`Graph.mst() -> { weight, edges }`**

- `weight` *(number)* — the total weight of the spanning forest.
- `edges` *(array of `[u, v, w]` triples)* — the edges chosen.

Returns the minimum spanning forest (Kruskal) for an undirected graph; `TypeError` if directed. Equal-weight tie order is unspecified.

**`Graph.aStar(src, dst, heuristic) -> { dist, path }`**

- `src` *(number)* — the start node id.
- `dst` *(number)* — the target node id.
- `heuristic` *(function)* — `heuristic(node)` estimates the distance to `dst`; it must return finite numbers and must not mutate the graph (both enforced).

Runs the heuristic once per node *before* the search (so it cannot re-enter the C loop). Returns `{ dist, path }`; `dist` is `Infinity` and `path` empty when `dst` is unreachable. Negative edges refuse up front.

**`Graph.serialize() -> Uint8Array`**

Returns the graph as a compact type-tagged record with delta-varint encoded adjacency.

**`Graph.deserialize(bytes) -> Graph`**

- `bytes` *(Uint8Array)* — a record produced by `Graph.serialize()`.

Rebuilds a graph from its record. Refuses malformed records: a bad target node, a non-finite weight, or asymmetric undirected adjacency all throw.

### LRU

Capacity-bounded string→value cache with least-recently-used eviction: hash map for O(1) lookup plus a doubly-linked MRU→LRU list. Expiry is measured on the monotonic clock.

```js
import { LRU } from "dyna:structures";

const lru = new LRU(2);
lru.put("a", 1).put("b", 2).put("c", 3);   // put returns this; "a" is evicted
console.log(lru.get("a"), lru.get("b"));   // undefined 2
console.log(lru.stats);                    // { hits, misses, evictions, expired, size, capacity }
```

**`new LRU(capacity, options?)`**

- `capacity` *(number)* — an integer in `1..2^24`.
  - `ttlMs` *(number, optional)* — when `> 0`, gives every entry a default expiry.
  - `onEvict` *(function, optional)* — `onEvict(key, value)` fires per eviction or expiry.

Creates a capacity-bounded cache. `onEvict` runs with the entry already detached; calling `get`/`put`/`purgeExpired` from inside it throws `TypeError`. The options bag is strict: an unknown key throws a `TypeError` naming the key and the valid set (`unknown option "ttl" (valid: ttlMs, onEvict)`) — `{ttlmMs: 30000}` would otherwise build a cache with NO expiry while the caller believed otherwise — while an absent, `null` or primitive options argument still means "no options". This is the module's only options bag; every other constructor takes positional arguments.

**`LRU.get(key) -> value | undefined`**

- `key` *(string)* — the cache key.

Returns the value and moves the entry to MRU. An expired entry is dropped (counted in `expired`) before the miss.

**`LRU.put(key, value) -> this`**

- `key` *(string)* — the cache key.
- `value` *(any)* — the value to store.

Inserts or updates, marking the entry MRU; past capacity the LRU entry is evicted.

**`LRU.set(key, value) -> this`**

- `key` *(string)* — the cache key.
- `value` *(any)* — the value to store.

Alias for `put`.

**`LRU.setWithTTL(key, value, ms) -> this`**

- `key` *(string)* — the cache key.
- `value` *(any)* — the value to store.
- `ms` *(number)* — the per-entry expiry; must be `> 0`.

Inserts the entry with its own expiry.

**`LRU.has(key) -> boolean`**

- `key` *(string)* — the cache key.

Returns presence without touching recency or counting a hit; an expired key reads absent.

**`LRU.delete(key) -> boolean`**

- `key` *(string)* — the cache key.

Removes the entry if present.

**`LRU.purgeExpired() -> number`**

Reclaims every expired entry now (expiry is otherwise lazy; no automatic sweep exists) and returns the count removed.

**`LRU.size -> number`**

The number of entries. Getter.

**`LRU.capacity -> number`**

The capacity bound. Getter.

**`LRU.stats -> object`**

`{ hits, misses, evictions, expired, size, capacity }`. Getter.

**`LRU.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`LRU.deserialize(bytes) -> LRU`**

- `bytes` *(Uint8Array)* — a record produced by `LRU.serialize()`.

Rebuilds a cache from its record.

### Heap

Binary heap ordered by a JS comparator, or by natural number order when none is given.

```js
import { Heap } from "dyna:structures";

const h = new Heap();
h.push(3); h.push(1); h.push(2);
console.log(h.pop(), h.pop(), h.pop());   // 1 2 3
console.log(Heap.deserialize(h.serialize(), (a, b) => a - b).size);
```

**`new Heap(comparator?)`**

- `comparator` *(function, optional)* — `(a, b) -> number`; the root is the minimum per `comparator`. NaN and 0 results mean "equal" (as in `Array.prototype.sort`).

Creates a binary heap. The comparator must not push/pop its own heap (reentrancy throws `TypeError`).

**`Heap.push(v) -> number`**

- `v` *(any)* — the value to insert.

Inserts and sifts, returning the new size. Without a comparator only numbers are accepted (`TypeError` otherwise, checked before storing).

**`Heap.pop() -> v | undefined`**

Removes and returns the root; `undefined` when empty.

**`Heap.peek() -> v | undefined`**

Returns the root without removal; never calls the comparator.

**`Heap.size -> number`**

The element count. Getter; `Heap.length` is the same getter.

**`Heap.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record. A comparator is not data, so the record cannot carry it.

**`Heap.deserialize(bytes, cmp?) -> Heap`**

- `bytes` *(Uint8Array)* — a record produced by `Heap.serialize()`.
- `cmp` *(function, optional)* — the comparator; omit for natural order.

Rebuilds a heap from its record.

### MinMaxHeap

Atkinson/Sack/Santoro/Strothotte min-max heap: both the minimum (root) and the maximum (larger of the root's two children) are readable in O(1), and both pops are O(log n), with one array.

```js
import { MinMaxHeap } from "dyna:structures";

const mh = new MinMaxHeap();
mh.push(5); mh.push(1); mh.push(3);
console.log(mh.peekMin(), mh.peekMax());  // 1 5
console.log(mh.popMin(), mh.popMax());    // 1 5
```

**`new MinMaxHeap()`**

Creates an empty min-max heap.

**`MinMaxHeap.push(priority, value?) -> this`**

- `priority` *(number)* — the sort key.
- `value` *(any, default `priority`)* — the stored value.

Inserts the pair. A NaN priority throws `RangeError`.

**`MinMaxHeap.popMin() -> value | undefined`**

Removes and returns the minimum's stored value.

**`MinMaxHeap.popMax() -> value | undefined`**

Removes and returns the maximum's stored value.

**`MinMaxHeap.peekMin() -> value | undefined`**

Returns the minimum without removal.

**`MinMaxHeap.peekMax() -> value | undefined`**

Returns the maximum without removal.

**`MinMaxHeap.size -> number`**

The element count. Getter.

**`MinMaxHeap.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`MinMaxHeap.deserialize(bytes) -> MinMaxHeap`**

- `bytes` *(Uint8Array)* — a record produced by `MinMaxHeap.serialize()`.

Rebuilds a min-max heap from its record.

### SortedSet

Set of numbers in sorted order (B+tree-backed: the same order-32 dyn-ds core as `BTree`, an audit M30-04 consolidation of the former skiplist). O(log n) for add/delete/floor/ceil.

```js
import { SortedSet } from "dyna:structures";

const s = new SortedSet();
s.add(3); s.add(1); s.add(2);
console.log(s.toArray());               // [1,2,3]
console.log(s.floor(2), s.ceil(3));     // 2 3
console.log(s.rangeQuery(1, 2));        // [1,2]
```

**`new SortedSet()`**

Creates an empty set. NaN keys are rejected on insert (`RangeError`).

**`SortedSet.add(x) -> this`**

- `x` *(number)* — the key.

Inserts the key if absent; duplicates are no-ops.

**`SortedSet.has(x) -> boolean`**

- `x` *(number)* — the key.

Returns whether the key is present.

**`SortedSet.delete(x) -> boolean`**

- `x` *(number)* — the key.

Removes the key, reporting whether it was present.

**`SortedSet.first() -> number | undefined`**

Returns the smallest key.

**`SortedSet.last() -> number | undefined`**

Returns the largest key.

**`SortedSet.floor(x) -> number | undefined`**

- `x` *(number)* — the probe.

Returns the largest key ≤ `x`; `undefined` if none or `x` is NaN.

**`SortedSet.ceil(x) -> number | undefined`**

- `x` *(number)* — the probe.

Returns the smallest key ≥ `x`; `undefined` if none or `x` is NaN.

**`SortedSet.rangeQuery(lo, hi) -> number[]`**

- `lo` *(number)* — the inclusive lower bound.
- `hi` *(number)* — the inclusive upper bound.

Returns ascending keys in `[lo, hi]`; empty for NaN or `lo > hi`.

**`SortedSet.toArray() -> number[]`**

Returns all keys ascending.

**`SortedSet.size -> number`**

The key count. Getter.

**`SortedSet[Symbol.iterator]()`**

Yields the keys in ascending order.

**`SortedSet.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`SortedSet.deserialize(bytes) -> SortedSet`**

- `bytes` *(Uint8Array)* — a record produced by `SortedSet.serialize()`.

Rebuilds a set from its record.

### SortedMap

Map from numeric key to JS value, keys sorted (B+tree-backed, same core as `BTree`). Same operation set as a `SortedSet` plus a payload.

SortedMap vs `BTree`: same method surface, but their `serialize()` formats are DISTINCT — `SortedMap.deserialize` refuses a BTree record ("these bytes are a BTree record, not a SortedMap") and vice versa, so the two are not interchangeable at the persistence boundary. SortedMap has no `[Symbol.iterator]`; `BTree` does. Pick one per codebase and stay with it.

```js
import { SortedMap } from "dyna:structures";

const m = new SortedMap();
m.set(2, "b"); m.set(1, "a"); m.set(3, "c");
console.log(m.rangeQuery(1, 2));   // [[1,"a"],[2,"b"]]
```

**`new SortedMap()`**

Creates an empty map. NaN keys are rejected on insert (`RangeError`).

**`SortedMap.set(k, v) -> this`**

- `k` *(number)* — the key.
- `v` *(any)* — the value.

Inserts or replaces. A NaN key throws `RangeError`.

**`SortedMap.get(k) -> v | undefined`**

- `k` *(number)* — the key.

Returns the value; `undefined` for absent or NaN keys.

**`SortedMap.has(k) -> boolean`**

- `k` *(number)* — the key.

Returns whether the key is present.

**`SortedMap.delete(k) -> boolean`**

- `k` *(number)* — the key.

Removes the key, reporting whether it was present.

**`SortedMap.firstKey() -> number | undefined`**

Returns the smallest key.

**`SortedMap.lastKey() -> number | undefined`**

Returns the largest key.

**`SortedMap.floorKey(k) -> number | undefined`**

- `k` *(number)* — the probe.

Returns the largest key ≤ `k`; `undefined` if none or `k` is NaN.

**`SortedMap.ceilKey(k) -> number | undefined`**

- `k` *(number)* — the probe.

Returns the smallest key ≥ `k`; `undefined` if none or `k` is NaN.

**`SortedMap.rangeQuery(lo, hi) -> [k, v][]`**

- `lo` *(number)* — the inclusive lower bound.
- `hi` *(number)* — the inclusive upper bound.

Returns ascending `[k, v]` pairs in `[lo, hi]`.

**`SortedMap.keys() -> number[]`**

Returns ascending keys.

**`SortedMap.size -> number`**

The key count. Getter.

**`SortedMap.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`SortedMap.deserialize(bytes) -> SortedMap`**

- `bytes` *(Uint8Array)* — a record produced by `SortedMap.serialize()`.

Rebuilds a map from its record.

### BTree

Ordered map on numeric keys stored as a B-tree of order 32 — a whole node of keys per level instead of the skiplist's one pointer per level.

```js
import { BTree } from "dyna:structures";

const bt = new BTree();
bt.set(3, "c"); bt.set(1, "a"); bt.set(2, "b");
console.log(bt.floorKey(2), bt.ceilKey(4));  // 2 undefined
```

**`new BTree()`**

Creates an empty tree. NaN keys throw `TypeError` ("keys must be ordered numbers, not NaN").

BTree vs `SortedMap` (which is otherwise the same surface): distinct `serialize()` formats — cross-deserializing refuses with an error naming the record type — and BTree is the iterable one (`[Symbol.iterator]` over `[key, value]` pairs).

**`BTree.set(k, v) -> this`**

- `k` *(number)* — the key.
- `v` *(any)* — the value.

Inserts or replaces. O(log n).

**`BTree.get(k) -> v | undefined`**

- `k` *(number)* — the key.

Returns the value. O(log n).

**`BTree.has(k) -> boolean`**

- `k` *(number)* — the key.

Returns whether the key is present. O(log n).

**`BTree.delete(k) -> boolean`**

- `k` *(number)* — the key.

Removes the key, reporting whether it was present. O(log n).

**`BTree.firstKey() -> number | undefined`**

Returns the smallest key.

**`BTree.lastKey() -> number | undefined`**

Returns the largest key.

**`BTree.floorKey(k) -> number | undefined`**

- `k` *(number)* — the probe.

Returns the largest key ≤ `k`; `undefined` if none.

**`BTree.ceilKey(k) -> number | undefined`**

- `k` *(number)* — the probe.

Returns the smallest key ≥ `k`; `undefined` if none.

**`BTree.rangeQuery(lo, hi) -> [k, v][]`**

- `lo` *(number)* — the inclusive lower bound.
- `hi` *(number)* — the inclusive upper bound.

Returns ascending `[k, v]` pairs in `[lo, hi]`.

**`BTree.keys() -> number[]`**

Returns ascending keys.

**`BTree.size -> number`**

The key count. Getter.

**`BTree[Symbol.iterator]()`**

Yields `[k, v]` pairs in ascending key order.

**`BTree.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`BTree.deserialize(bytes) -> BTree`**

- `bytes` *(Uint8Array)* — a record produced by `BTree.serialize()`.

Rebuilds a tree from its record.

### Deque

Double-ended queue of JS values with O(1) push/pop at both ends (circular buffer; `Array.shift`/`unshift` are O(n) — this is the gap). A drain halves an oversized buffer opportunistically.

```js
import { Deque } from "dyna:structures";

const d = new Deque();
d.pushBack(1); d.pushBack(2); d.pushFront(0);
console.log(d.toArray(), d.peekFront());  // [0,1,2] 0
```

**`new Deque()`**

Creates an empty deque; the buffer grows from 8.

**`Deque.pushBack(v) -> number`**

- `v` *(any)* — the value.

Appends at the back and returns the new length.

**`Deque.pushFront(v) -> number`**

- `v` *(any)* — the value.

Prepends at the front and returns the new length.

**`Deque.popFront() -> v | undefined`**

Removes and returns the front; `undefined` when empty.

**`Deque.popBack() -> v | undefined`**

Removes and returns the back; `undefined` when empty.

**`Deque.peekFront() -> v | undefined`**

Returns the front without removal.

**`Deque.peekBack() -> v | undefined`**

Returns the back without removal.

**`Deque.get(i) -> v | undefined`**

- `i` *(number)* — 0-indexed from the front.

Returns the element at index `i`.

**`Deque.length -> number`**

The element count. Getter.

**`Deque.toArray() -> v[]`**

Returns a snapshot of the elements.

**`Deque[Symbol.iterator]()`**

Iterates with snapshot semantics: mutation during `for...of` is well defined.

**`Deque.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`Deque.deserialize(bytes) -> Deque`**

- `bytes` *(Uint8Array)* — a record produced by `Deque.serialize()`.

Rebuilds a deque from its record.

### List

Doubly-linked list of JS values with O(1) operations at both ends. Each node is its own allocation, so element identity is stable across the list's lifetime.

```js
import { List } from "dyna:structures";

const l = new List();
l.pushBack(1); l.pushBack(2); l.pushFront(0);
console.log(l.front(), l.back(), l.length);  // 0 2 3
```

**`new List()`**

Creates an empty list.

**`List.pushFront(v) -> number`**

- `v` *(any)* — the value.

Prepends and returns the new length.

**`List.pushBack(v) -> number`**

- `v` *(any)* — the value.

Appends and returns the new length.

**`List.popFront() -> v | undefined`**

Removes and returns the front.

**`List.popBack() -> v | undefined`**

Removes and returns the back.

**`List.front() -> v | undefined`**

Returns the front without removal.

**`List.back() -> v | undefined`**

Returns the back without removal.

**`List.length -> number`**

The element count. Getter.

**`List.toArray() -> v[]`**

Returns a head-to-tail snapshot.

**`List[Symbol.iterator]()`**

Yields the elements head-to-tail.

**`List.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`List.deserialize(bytes) -> List`**

- `bytes` *(Uint8Array)* — a record produced by `List.serialize()`.

Rebuilds a list from its record.

### RingBuffer

Fixed-capacity circular buffer; `push` overwrites (and frees) the oldest element when full, keeping the most recent `capacity` items.

```js
import { RingBuffer } from "dyna:structures";

const rb = new RingBuffer(2);
rb.push(1); rb.push(2); rb.push(3);
console.log(rb.toArray(), rb.full);  // [2,3] true
```

**`new RingBuffer(capacity)`**

- `capacity` *(number)* — an integer in `1..2^24`, else `RangeError`.

Creates a fixed-capacity circular buffer.

**`RingBuffer.push(v) -> number`**

- `v` *(any)* — the value.

Appends, evicting the oldest when full; returns the count.

**`RingBuffer.get(i) -> v | undefined`**

- `i` *(number)* — 0-indexed, oldest first.

Returns the element at index `i`.

**`RingBuffer.length -> number`**

The current count. Getter.

**`RingBuffer.capacity -> number`**

The capacity. Getter.

**`RingBuffer.full -> boolean`**

Whether the buffer holds `capacity` elements. Getter.

**`RingBuffer.toArray() -> v[]`**

Returns the elements in insertion order.

**`RingBuffer[Symbol.iterator]()`**

Yields the elements in insertion order.

**`RingBuffer.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`RingBuffer.deserialize(bytes) -> RingBuffer`**

- `bytes` *(Uint8Array)* — a record produced by `RingBuffer.serialize()`.

Rebuilds a buffer from its record.

### BitSet

Dynamic bit set backed by 64-bit words; grows on demand.

```js
import { BitSet } from "dyna:structures";

const b = new BitSet();
b.set(0); b.set(3); b.set(63);
console.log(b.count, b.nextSet(1), b.toArray());  // 3 3 [0,3,63]
```

**`new BitSet(nbits?)`**

- `nbits` *(number, default `0`)* — an initial capacity hint, up to 2^30 bits.

Creates a bit set.

**`BitSet.set(i) -> this`**

- `i` *(number)* — the bit index.

Sets the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`.

**`BitSet.clear(i) -> this`**

- `i` *(number)* — the bit index.

Clears the bit.

**`BitSet.flip(i) -> this`**

- `i` *(number)* — the bit index.

Flips the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`.

**`BitSet.get(i) -> boolean`**

- `i` *(number)* — the bit index.

Returns false when the bit is past the allocated size.

**`BitSet.nextSet(from) -> number`**

- `from` *(number)* — the start position.

Returns the index of the first set bit at position ≥ `from`, or `-1`.

**`BitSet.count -> number`**

The number of set bits (popcount over the words). Getter.

**`BitSet.and(other) -> this`**

- `other` *(BitSet)* — the operand.

In-place word AND; bits past `other` clear.

**`BitSet.or(other) -> this`**

- `other` *(BitSet)* — the operand.

In-place word OR; `this` grows as needed.

**`BitSet.xor(other) -> this`**

- `other` *(BitSet)* — the operand.

In-place word XOR; `this` grows as needed.

**`BitSet.toArray() -> number[]`**

Returns ascending indices of set bits.

**`BitSet[Symbol.iterator]()`**

Yields the set-bit indices in ascending order.

**`BitSet.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`BitSet.deserialize(bytes) -> BitSet`**

- `bytes` *(Uint8Array)* — a record produced by `BitSet.serialize()`.

Rebuilds a bit set from its record.

### UnionFind

Disjoint-set forest over elements `0..n-1` with path halving and union by rank.

```js
import { UnionFind } from "dyna:structures";

const uf = new UnionFind(5);
uf.union(0, 1); uf.union(2, 3);
console.log(uf.connected(0, 1), uf.connected(0, 2), uf.count);  // true false 3
```

**`new UnionFind(n?)`**

- `n` *(number, default `0`)* — an integer in `0..2^26`.

Creates a disjoint-set forest over elements `0..n-1`.

**`UnionFind.find(x) -> number`**

- `x` *(number)* — the element.

Returns the component root of `x`. Out-of-range throws `RangeError`.

**`UnionFind.union(x, y) -> boolean`**

- `x` *(number)* — an element.
- `y` *(number)* — an element.

Merges two components; true if it merged distinct sets, false if already connected.

**`UnionFind.connected(x, y) -> boolean`**

- `x` *(number)* — an element.
- `y` *(number)* — an element.

Returns whether both elements are in the same component.

**`UnionFind.count -> number`**

The number of disjoint components. Getter.

**`UnionFind.size -> number`**

The element count. Getter.

**`UnionFind.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`UnionFind.deserialize(bytes) -> UnionFind`**

- `bytes` *(Uint8Array)* — a record produced by `UnionFind.serialize()`.

Rebuilds a forest from its record.

### Fenwick

Fenwick tree (binary indexed tree): O(log n) point add and prefix/range sum over a fixed-size vector of doubles.

```js
import { Fenwick } from "dyna:structures";

const f = new Fenwick(5);
f.update(0, 1); f.update(1, 2); f.update(4, 4);
console.log(f.prefixSum(4), f.rangeQuery(1, 4));  // 7 6
```

**`new Fenwick(n)`**

- `n` *(number)* — an integer in `0..2^26`; the tree has `n` zeroed positions `0..n-1`.

Creates a Fenwick tree.

**`Fenwick.update(i, delta) -> this`**

- `i` *(number)* — the position.
- `delta` *(number)* — the addend.

Adds `delta` to position `i`. Out-of-range throws `RangeError`.

**`Fenwick.prefixSum(i) -> number`**

- `i` *(number)* — the inclusive upper bound.

Returns the sum of positions `[0..i]`.

**`Fenwick.rangeQuery(lo, hi) -> number`**

- `lo` *(number)* — the inclusive lower bound.
- `hi` *(number)* — the inclusive upper bound.

Returns the sum of `[lo..hi]`; 0 for an empty (`lo > hi`) range.

**`Fenwick.size -> number`**

The number of positions. Getter.

**`Fenwick.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`Fenwick.deserialize(bytes) -> Fenwick`**

- `bytes` *(Uint8Array)* — a record produced by `Fenwick.serialize()`.

Rebuilds a tree from its record.

### SegTree

Iterative segment tree over doubles with an associative fold: `"sum"` (default), `"min"` or `"max"`. O(log n) point update and range query.

```js
import { SegTree } from "dyna:structures";

const t = new SegTree(4, "sum");
t.update(0, 1); t.update(1, 2); t.update(2, 3); t.update(3, 4);
console.log(t.rangeQuery(0, 3));  // 10
```

**`new SegTree(n, op?)`**

- `n` *(number)* — an integer in `1..2^26`; the tree has `n` identity-filled slots (`0` for sum, `+Infinity` for min, `-Infinity` for max).
- `op` *(string, default `"sum"`)* — one of `"sum"|"min"|"max"`, else `RangeError`.

Creates a segment tree.

**`SegTree.update(i, value) -> this`**

- `i` *(number)* — the leaf index.
- `value` *(number)* — the new value.

Assigns leaf `i`, re-folding the path to the root.

**`SegTree.rangeQuery(lo, hi) -> number`**

- `lo` *(number)* — the inclusive lower bound.
- `hi` *(number)* — the inclusive upper bound.

Returns the fold over `[lo..hi]`; the identity for an empty (`lo > hi`) range.

**`SegTree.size -> number`**

The number of leaves. Getter.

**`SegTree.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`SegTree.deserialize(bytes) -> SegTree`**

- `bytes` *(Uint8Array)* — a record produced by `SegTree.serialize()`.

Rebuilds a tree from its record.

### BloomFilter

Probabilistic set membership over string keys: no false negatives; false positives bounded by (bits, hashes). Double hashing `h_i = h1 + i·h2` (Kirsch-Mitzenmacher) with an incremental probe stepper so persisted filters keep mapping keys to the same bits.

```js
import { BloomFilter } from "dyna:structures";

const bf = new BloomFilter(1024, 4);
bf.add("apple"); bf.add("banana");
console.log(bf.mayContain("apple"), bf.mayContain("cherry"));  // true false
```

**`new BloomFilter(bits, hashes?)`**

- `bits` *(number)* — `> 0`, up to 2^30.
- `hashes` *(number, default `3`)* — clamped into `1..32` (0 becomes 1).

Creates a Bloom filter.

**`BloomFilter.add(key) -> this`**

- `key` *(string)* — the key.

Sets the key's `hashes` bits.

**`BloomFilter.mayContain(key) -> boolean`**

- `key` *(string)* — the key.

False means *definitely absent*; true means *possibly present*.

**`BloomFilter.bits -> number`**

The number of bits. Getter.

**`BloomFilter.hashes -> number`**

The number of hashes. Getter.

**`BloomFilter.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`BloomFilter.deserialize(bytes) -> BloomFilter`**

- `bytes` *(Uint8Array)* — a record produced by `BloomFilter.serialize()`.

Rebuilds a filter from its record.

### Trie

Set of byte strings with prefix queries. First-child/next-sibling nodes from bump-allocated chunks with lazy tail compression; teardown and prefix walks are iterative, so deep keys cannot overflow the C stack.

```js
import { Trie } from "dyna:structures";

const trie = new Trie();
trie.insert("apple"); trie.insert("app"); trie.insert("apricot");
console.log(trie.has("app"), trie.longestPrefix("applesauce"));  // true apple
```

**`new Trie()`**

Creates an empty trie.

**`Trie.insert(key) -> this`**

- `key` *(string)* — the key.

Stores the key; re-inserting an existing key is a no-op.

**`Trie.has(key) -> boolean`**

- `key` *(string)* — the key.

Returns whether the key is present.

**`Trie.delete(key) -> boolean`**

- `key` *(string)* — the key.

Removes the key, reporting whether it was present; nodes are retained, only the marker clears.

**`Trie.keysWithPrefix(prefix) -> string[]`**

- `prefix` *(string)* — the prefix to match.

Returns every stored key starting with `prefix`. Ascending order is *not* guaranteed (sibling insertion order). A prefix that ends inside a compressed tail still matches.

**`Trie.longestPrefix(str) -> string`**

- `str` *(string)* — the string to probe.

Returns the longest stored key that is a prefix of `str`, or `""`.

**`Trie.size -> number`**

The number of stored keys. Getter.

**`Trie[Symbol.iterator]()`**

Yields the stored keys.

**`Trie.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`Trie.deserialize(bytes) -> Trie`**

- `bytes` *(Uint8Array)* — a record produced by `Trie.serialize()`.

Rebuilds a trie from its record.

### Multiset

String key → uint64 count. Counts saturate at 2^64−1 rather than wrap.

```js
import { Multiset } from "dyna:structures";

const m = new Multiset();
m.add("a"); m.add("a"); m.add("b");
console.log(m.count("a"), m.entrySet());   // 2 [["a",2],["b",1]]
```

**`new Multiset()`**

Creates an empty multiset.

**`Multiset.add(key, n?) -> number`**

- `key` *(string)* — the key.
- `n` *(number, default `1`)* — the increment; negative `n` throws `RangeError`.

Returns the key's new count.

**`Multiset.remove(key, n?) -> number`**

- `key` *(string)* — the key.
- `n` *(number, default `1`)* — the decrement; negative `n` throws `RangeError`.

Subtracts, returning the new count (flooring at 0; the record is dropped at 0).

**`Multiset.count(key) -> number`**

- `key` *(string)* — the key.

Returns the key's count.

**`Multiset.has(key) -> boolean`**

- `key` *(string)* — the key.

Returns whether the key is present.

**`Multiset.setCount(key, count) -> this`**

- `key` *(string)* — the key.
- `count` *(number)* — required, non-negative; 0 deletes the key.

Sets the count exactly.

**`Multiset.delete(key) -> boolean`**

- `key` *(string)* — the key.

Removes the key entirely, reporting whether it was present.

**`Multiset.clear()`**

Drops all entries.

**`Multiset.elementSet() -> string[]`**

Returns the distinct keys.

**`Multiset.entrySet() -> [key, count][]`**

Returns the keys with their counts.

**`Multiset.size -> number`**

The distinct key count. Getter.

**`Multiset.totalSize -> number`**

The sum of all counts. Getter.

**`Multiset[Symbol.iterator]()`**

Yields `[key, count]` entries.

**`Multiset.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`Multiset.deserialize(bytes) -> Multiset`**

- `bytes` *(Uint8Array)* — a record produced by `Multiset.serialize()`.

Rebuilds a multiset from its record.

### Multimap

String key → list of values (a map from key to a growing value array). `size` is the total value count.

```js
import { Multimap } from "dyna:structures";

const m = new Multimap();
m.put("k", 1); m.put("k", 2); m.put("j", 3);
console.log(m.get("k"), m.count("k"), m.keyCount);  // [1,2] 2 2
```

**`new Multimap()`**

Creates an empty multimap.

**`Multimap.put(key, value) -> this`**

- `key` *(string)* — the key.
- `value` *(any)* — the value to append.

Appends one value.

**`Multimap.get(key) -> v[]`**

- `key` *(string)* — the key.

Returns a fresh array of all values for the key (empty if none).

**`Multimap.count(key) -> number`**

- `key` *(string)* — the key.

Returns the number of values for the key.

**`Multimap.delete(key) -> number`**

- `key` *(string)* — the key.

Removes every value for the key; returns how many.

**`Multimap.removeAt(key, index) -> v | undefined`**

- `key` *(string)* — the key.
- `index` *(number)* — the value index.

Removes the value at `index`. Both arguments required (`TypeError` otherwise).

**`Multimap.keys() -> string[]`**

Returns the distinct keys.

**`Multimap.entries() -> [key, value][]`**

Returns flattened pairs.

**`Multimap.size -> number`**

The total value count. Getter.

**`Multimap.keyCount -> number`**

The distinct key count. Getter.

**`Multimap[Symbol.iterator]()`**

Yields `[key, value]` entries.

**`Multimap.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`Multimap.deserialize(bytes) -> Multimap`**

- `bytes` *(Uint8Array)* — a record produced by `Multimap.serialize()`.

Rebuilds a multimap from its record.

### BiMap

Two-way string→string map: every key maps to a value and every value maps back to exactly one key. Backed by two hash tables.

```js
import { BiMap } from "dyna:structures";

const b = new BiMap();
b.set("a", "1"); b.set("b", "2");
console.log(b.keyOf("2"), b.inverseEntries());   // b [["1","a"],["2","b"]]
```

**`new BiMap()`**

Creates an empty bimap.

**`BiMap.set(key, value) -> this`**

- `key` *(string)* — the key.
- `value` *(string)* — the value.

Throws `TypeError` if the value is already bound to another key.

**`BiMap.forceSet(key, value) -> this`**

- `key` *(string)* — the key.
- `value` *(string)* — the value.

Rebinds, dropping the pair that held the value.

**`BiMap.get(key) -> string | undefined`**

- `key` *(string)* — the key.

Forward lookup.

**`BiMap.keyOf(value) -> string | undefined`**

- `value` *(string)* — the value.

Inverse lookup.

**`BiMap.has(key) -> boolean`**

- `key` *(string)* — the key.

Returns whether the key is bound.

**`BiMap.hasValue(value) -> boolean`**

- `value` *(string)* — the value.

Returns whether the value is bound.

**`BiMap.delete(key) -> boolean`**

- `key` *(string)* — the key.

Drops the key and its value, reporting whether it was present.

**`BiMap.deleteValue(value) -> boolean`**

- `value` *(string)* — the value.

Drops the value and its key, reporting whether it was present.

**`BiMap.entries() -> [key, value][]`**

Returns the forward pairs.

**`BiMap.inverseEntries() -> [value, key][]`**

Returns the inverse pairs.

**`BiMap.clear()`**

Drops all pairs.

**`BiMap.size -> number`**

The number of pairs. Getter.

**`BiMap[Symbol.iterator]()`**

Yields the forward `[key, value]` entries.

**`BiMap.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`BiMap.deserialize(bytes) -> BiMap`**

- `bytes` *(Uint8Array)* — a record produced by `BiMap.serialize()`.

Rebuilds a bimap from its record.

### Table

Sparse two-dimensional string→string→value map (row and column keys with a cell value). Backed by a flat cell array.

```js
import { Table } from "dyna:structures";

const tb = new Table();
tb.put("r1", "c1", 1); tb.put("r1", "c2", 2); tb.put("r2", "c1", 3);
console.log(tb.row("r1"), tb.column("c1"));   // [["c1",1],["c2",2]] [["r1",1],["r2",3]]
```

**`new Table()`**

Creates an empty table.

**`Table.put(row, col, value) -> this`**

- `row` *(string)* — the row key.
- `col` *(string)* — the column key.
- `value` *(any)* — the cell value.

Sets a cell, replacing the old value.

**`Table.get(row, col) -> v | undefined`**

- `row` *(string)* — the row key.
- `col` *(string)* — the column key.

Returns the cell value.

**`Table.has(row, col) -> boolean`**

- `row` *(string)* — the row key.
- `col` *(string)* — the column key.

Returns whether the cell is present.

**`Table.delete(row, col) -> boolean`**

- `row` *(string)* — the row key.
- `col` *(string)* — the column key.

Removes a cell, reporting whether it was present.

**`Table.row(r) -> [col, value][]`**

- `r` *(string)* — the row key.

Returns the row's cells; a linear scan of the sparse cell array.

**`Table.column(c) -> [row, value][]`**

- `c` *(string)* — the column key.

Returns the column's cells; a linear scan of the sparse cell array.

**`Table.cells() -> [row, col, value][]`**

Returns every occupied cell.

**`Table.size -> number`**

The cell count. Getter.

**`Table[Symbol.iterator]()`**

Yields `[row, col, value]` cells.

**`Table.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`Table.deserialize(bytes) -> Table`**

- `bytes` *(Uint8Array)* — a record produced by `Table.serialize()`.

Rebuilds a table from its record.

### RangeSet

Set of closed numeric intervals, kept disjoint and merged on insert. O(n) operations over the interval array.

```js
import { RangeSet } from "dyna:structures";

const r = new RangeSet();
r.add(1, 5); r.add(10, 20);
console.log(r.contains(3), r.encloses(2, 4), r.measure);   // true true 14
```

**`new RangeSet()`**

Creates an empty range set.

**`RangeSet.add(lo, hi) -> this`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound.

Unions an interval; overlapping neighbours merge as needed. NaN bounds throw `RangeError`.

**`RangeSet.remove(lo, hi) -> this`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound.

Subtracts an interval; overlapping neighbours split as needed. NaN bounds throw `RangeError`.

**`RangeSet.contains(x) -> boolean`**

- `x` *(number)* — the point.

Returns whether the point is covered.

**`RangeSet.encloses(lo, hi) -> boolean`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound.

Returns whether `[lo, hi]` is fully covered.

**`RangeSet.intersects(lo, hi) -> boolean`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound.

Returns whether the interval overlaps any stored range.

**`RangeSet.ranges() -> [lo, hi][]`**

Returns the disjoint ranges in order.

**`RangeSet.complement(lo, hi) -> [lo, hi][]`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound.

Returns the gaps of `[lo, hi]` outside the set.

**`RangeSet.clear()`**

Drops all ranges.

**`RangeSet.size -> number`**

The number of disjoint ranges. Getter.

**`RangeSet.measure -> number`**

The total covered length. Getter.

**`RangeSet[Symbol.iterator]()`**

Yields `[lo, hi]` ranges.

**`RangeSet.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`RangeSet.deserialize(bytes) -> RangeSet`**

- `bytes` *(Uint8Array)* — a record produced by `RangeSet.serialize()`.

Rebuilds a range set from its record.

### RangeMap

Map from closed numeric intervals to JS values; `get(x)` returns the value of the range containing the point. Overlapping puts split the overlapped ranges (later values win on the overlap); `remove` clears a span.

```js
import { RangeMap } from "dyna:structures";

const rm = new RangeMap();
rm.put(0, 10, "low"); rm.put(20, 30, "high");
console.log(rm.get(5), rm.get(15), rm.get(25));  // low undefined high
```

**`new RangeMap()`**

Creates an empty range map.

**`RangeMap.put(lo, hi, value) -> this`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound.
- `value` *(any)* — the value to store.

Assigns the whole span; an empty range (`hi <= lo`) stores nothing. Overlapping puts split the overlapped ranges (later values win on the overlap).

**`RangeMap.get(x) -> v | undefined`**

- `x` *(number)* — the point.

Returns the value of the range containing `x`, or `undefined` if uncovered.

**`RangeMap.remove(lo, hi) -> this`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound.

Clears a span, splitting neighbour ranges.

**`RangeMap.entries() -> [lo, hi, value][]`**

Returns the stored ranges in order.

**`RangeMap.size -> number`**

The number of stored ranges. Getter.

**`RangeMap[Symbol.iterator]()`**

Yields `[lo, hi, value]` entries.

**`RangeMap.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`RangeMap.deserialize(bytes) -> RangeMap`**

- `bytes` *(Uint8Array)* — a record produced by `RangeMap.serialize()`.

Rebuilds a range map from its record.

### IntervalTree

Closed-interval store with overlap enumeration: intervals sorted by `lo` with a max-`hi` segment tree pruned per query — O(log n + k) for k results. The index is rebuilt lazily, so a bulk load pays one sort, and inserts amortise to O(1).

```js
import { IntervalTree } from "dyna:structures";

const it = new IntervalTree();
it.insert(0, 10, "a"); it.insert(5, 15, "b"); it.insert(20, 30, "c");
console.log(it.overlapping(8, 12));   // [[0,10,"a"],[5,15,"b"]]
```

**`new IntervalTree()`**

Creates an empty interval tree.

**`IntervalTree.insert(lo, hi, value) -> this`**

- `lo` *(number)* — the closed lower bound.
- `hi` *(number)* — the closed upper bound; clamped to at least `lo`.
- `value` *(any)* — the value to store.

Stores an interval. NaN bounds throw `RangeError`.

**`IntervalTree.overlapping(lo, hi) -> [lo, hi, value][]`**

- `lo` *(number)* — the query lower bound.
- `hi` *(number)* — the query upper bound.

Returns every interval intersecting `[lo, hi]`; an inverted range answers `[]`.

**`IntervalTree.at(x) -> [lo, hi, value][]`**

- `x` *(number)* — the point.

The degenerate point query `[x, x]`.

**`IntervalTree.size -> number`**

The interval count. Getter.

**`IntervalTree.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`IntervalTree.deserialize(bytes) -> IntervalTree`**

- `bytes` *(Uint8Array)* — a record produced by `IntervalTree.serialize()`.

Rebuilds an interval tree from its record.

### CountMinSketch

Count-min sketch: `depth` rows of `width` saturating counters; `count(key)` is an overestimate (an upper bound) with error bounded by the width/depth pair. Uses two hashes with Kirsch-Mitzenmacher double hashing.

```js
import { CountMinSketch } from "dyna:structures";

const c = new CountMinSketch(64, 4);
c.add("apple"); c.add("apple"); c.add("pear");
console.log(c.count("apple"));   // 2
```

**`new CountMinSketch(width, depth?)`**

- `width` *(number)* — the row width; `> 0`.
- `depth` *(number, default `5`)* — `> 0`, `<= 64`; `width * depth <= 2^24` counters (else `RangeError`).

Creates a count-min sketch.

**`CountMinSketch.add(key, n?) -> this`**

- `key` *(string)* — the key.
- `n` *(number, default `1`)* — the increment; negative `n` throws (a sketch cannot be decremented).

Counts `n` occurrences.

**`CountMinSketch.count(key) -> number`**

- `key` *(string)* — the key.

Returns the estimate for the key; an overestimate (an upper bound).

**`CountMinSketch.merge(other) -> this`**

- `other` *(CountMinSketch)* — the sketch to merge.

Sums counters. Requires identical `width` and `depth` (else `TypeError`); merging into itself is refused.

**`CountMinSketch.width -> number`**

The number of counters per row. Getter.

**`CountMinSketch.depth -> number`**

The number of rows. Getter.

**`CountMinSketch.totalCount -> number`**

The total number of occurrences added. Getter.

**`CountMinSketch.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`CountMinSketch.deserialize(bytes) -> CountMinSketch`**

- `bytes` *(Uint8Array)* — a record produced by `CountMinSketch.serialize()`.

Rebuilds a sketch from its record. Probing hashes with the fixed seed 0 — the same portability choice `HyperLogLog` makes — so a record written in one process counts correctly in any other.

### HyperLogLog

Cardinality estimator over string keys: `2^precision` one-byte registers; standard error ≈ `1.04/sqrt(2^precision)` — 0.81% at the default. The estimate is cached and recomputed lazily.

```js
import { HyperLogLog } from "dyna:structures";

const h = new HyperLogLog(10);
for (let i = 0; i < 1000; i++) h.add("item" + i);
console.log(Math.round(h.count()));   // ≈ 1000
```

**`new HyperLogLog(precision?)`**

- `precision` *(number, default `14`)* — an integer in `4..18` (default 14 = 16 KiB of registers).

Creates a cardinality estimator.

**`HyperLogLog.add(key) -> this`**

- `key` *(string)* — the key.

Folds the key's hash into one register.

**`HyperLogLog.count() -> number`**

Returns the cardinality estimate.

**`HyperLogLog.merge(other) -> this`**

- `other` *(HyperLogLog)* — the estimator to merge.

Unions the registers. Requires equal precision (else `TypeError`).

**`HyperLogLog.precision -> number`**

The exponent: the estimator holds `2^precision` registers. Getter.

**`HyperLogLog.registers -> number`**

The number of registers (`2^precision`). Getter.

**`HyperLogLog.serialize() -> Uint8Array`**

Returns the contents as a compact type-tagged record.

**`HyperLogLog.deserialize(bytes) -> HyperLogLog`**

- `bytes` *(Uint8Array)* — a record produced by `HyperLogLog.serialize()`.

Rebuilds an estimator from its record.

### Serialization

Every container persists as a compact type-tagged record: `obj.serialize() -> Uint8Array`, and a per-class static `Class.deserialize(bytes, opts?)` that refuses a record of any other type with `TypeError`. `Heap.deserialize(bytes, cmp)` additionally takes the comparator (order is not data). The reader validates every length against the bytes remaining before allocating.

```js
import { Graph, Fenwick } from "dyna:structures";

const g = new Graph({ directed: true, weighted: true });
const a = g.addNode(), b = g.addNode();
g.addEdge(a, b, 2);
console.log(Graph.deserialize(g.serialize()).dijkstra(a));  // [0, 2]

const f = new Fenwick(3);
f.update(0, 1);
console.log(Fenwick.deserialize(f.serialize()).prefixSum(0));  // 1
```

---

# dyna:simd

Runtime-dispatched vector kernels over typed arrays: reductions, element-wise arithmetic, activations, distances, BLAS-2/3, statistics, and f64/i32 families. Each call is one JS→C transition and a native loop — never interpreted steps.

Width prefixes: the plain names run the Float32Array kernels (the `f32` default); `f64*` runs Float64Array; `i32*` runs Int32Array (strict class check, never bit-reinterpreted). Any 4-byte typed array is accepted by the f32 family as a bit-cast reinterpret of its raw 32-bit patterns.

`import { sum, dot, scale, axpy, add, sub, mul, div, abs, fma, addScalar, affine, normL1, normL2, max, min, argmax, argmin, sigmoid, relu, relu6, leakyRelu, elu, tanhFast, gelu, silu, softmax, logSoftmax, vexp, vlog, vsqrt, vrsqrt, vinv, distL1, distL2, distCos, distCheb, gemv, gemvT, gemm, mean, variance, normalize, clamp, threshold, topkIndices, f64Sum, f64Dot, f64Min, f64Max, f64Scale, f64Axpy, f64Mean, f64Variance, i32Sum, i32Min, i32Max, i32Dot, i32Add, i32Mul, i32Scale, i32Mean, cumsum, cummax } from "dyna:simd";`

Windows (offset,length): every vector kernel except `gemv`/`gemvT`/`gemm`/`topkIndices` accepts a trailing `(offset, length)` window or `{ offset, length|limit }` bag (limit aliases length): the same slice applies to every array arg without a `subarray()` copy. Out of range is a `RangeError`.

```js
import { sum } from "dyna:simd";
const a = new Float32Array([1, 2, 3, 4]);
print(sum(a, 1, 2)); // 5
```

### Dispatch model

Kernels are selected once at runtime startup from a shared dispatch table. `cpu_features()` detects the ISA (CPUID on x86-64, `getauxval`/sysctl on ARM64) and installs overrides in the order scalar → SSE4.2 → NEON → SVE → AVX2+FMA → AVX-512 (F+BW+DQ); each override fills only the slots it accelerates, so a higher ISA inherits un-overridden kernels from a lower one, and every kernel has a scalar fallback so the table is always fully populated. The same table serves the engine core. Typed-array dtypes are strict per family:

- **f32 family** (the plain-named kernels): the backing element must be 4 bytes — a `Float32Array` (or any 4-byte-element typed array; the check is bytes-per-element).
- **f64 family** (`f64*`): 8-byte elements, i.e. `Float64Array`.
- **i32 family** (`i32*`): the element type is verified by class id — only an `Int32Array` is accepted; a same-stride `Float32Array`/`Uint32Array` is rejected with `TypeError`, never reinterpreted.

A mismatched dtype throws `TypeError`, a length mismatch throws `RangeError`, and empty-array reductions (`max`/`min`/`argmax`/`argmin`, `softmax`/`logSoftmax`, `mean`/`variance`/`normalize`, `f64Max`/`f64Min`, `f64Mean`/`f64Variance`, `i32Max`/`i32Min`, `i32Mean`) throw `RangeError`. `sum`/`dot`/`mean`/`variance` reorder float additions, so they match a sequential sum only to a relative tolerance; the i32 reductions are exact. Every length `n >= 1` is safe (the kernels guard their seed loads — an earlier binding detoured small arrays through a scalar loop; that split is gone), and NaN in the input propagates to NaN through every reduction and in-place transform. Scalar arguments are coerced to C locals *before* any buffer is resolved, so no user code can run between resolve and kernel.

### Reductions

```js
import { sum, max, argmax, normL2 } from "dyna:simd";

const x = new Float32Array([1, 2, 3, 4]);
console.log(sum(x), max(x), argmax(x), normL2(x));  // 10 4 3 5.477...
```

**`sum(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the arithmetic sum of a float array. `sum` reorders float additions, so it matches a sequential sum only to a relative tolerance.

**`max(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the maximum value. Throws `RangeError` on an empty array.

**`min(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the minimum value. Throws `RangeError` on an empty array.

**`argmax(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the index of the maximum. Throws `RangeError` on an empty array.

**`argmin(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the index of the minimum. Throws `RangeError` on an empty array.

**`normL1(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the 1-norm.

**`normL2(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the Euclidean norm.

### Vector–vector

`add`/`sub`/`mul`/`div`/`abs`/`fma` write into a separate `out` array; `dot` returns a scalar; all lengths must match.

```js
import { add, dot } from "dyna:simd";

const a = new Float32Array([1, 2, 3, 4]), b = new Float32Array([4, 3, 2, 1]);
console.log(Array.from(add(new Float32Array(4), a, b)));  // [5,5,5,5]
console.log(dot(a, b));                                    // 20
```

**`add(out, a, b) -> out`**

- `out` *(Float32Array or 4-byte-element typed array)* — the destination.
- `a`, `b` *(Float32Array or 4-byte-element typed array)* — the operands.

Computes `out[i] = a[i] + b[i]` and returns `out`. All lengths must match.

**`sub(out, a, b) -> out`**

- `out` *(Float32Array or 4-byte-element typed array)* — the destination.
- `a`, `b` *(Float32Array or 4-byte-element typed array)* — the operands.

Computes `out[i] = a[i] - b[i]` and returns `out`. All lengths must match.

**`mul(out, a, b) -> out`**

- `out` *(Float32Array or 4-byte-element typed array)* — the destination.
- `a`, `b` *(Float32Array or 4-byte-element typed array)* — the operands.

Computes `out[i] = a[i] * b[i]` and returns `out`. All lengths must match.

**`div(out, a, b) -> out`**

- `out` *(Float32Array or 4-byte-element typed array)* — the destination.
- `a`, `b` *(Float32Array or 4-byte-element typed array)* — the operands.

Computes `out[i] = a[i] / b[i]` and returns `out`. All lengths must match.

**`abs(out, a) -> out`**

- `out` *(Float32Array or 4-byte-element typed array)* — the destination.
- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Computes `out[i] = |a[i]|` and returns `out`.

**`fma(z, a, b) -> z`**

- `z` *(Float32Array or 4-byte-element typed array)* — the accumulator.
- `a`, `b` *(Float32Array or 4-byte-element typed array)* — the operands.

In-place `z[i] += a[i] * b[i]` (one rounding, like `axpy`).

**`dot(a, b) -> number`**

- `a`, `b` *(Float32Array or 4-byte-element typed array)* — the inputs.

Returns the inner product. `dot` reorders float additions, so it matches a sequential sum only to a relative tolerance.

### Scalar–vector

All in-place on the single array; the scalar is coerced first.

**`scale(a, s) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.
- `s` *(number)* — the scalar.

Computes `a[i] *= s` in place and returns `a`.

**`addScalar(a, s) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.
- `s` *(number)* — the scalar.

Computes `a[i] += s` in place and returns `a`.

**`axpy(y, alpha, x) -> y`**

- `y` *(Float32Array or 4-byte-element typed array)* — the destination.
- `alpha` *(number)* — the scalar multiplier.
- `x` *(Float32Array or 4-byte-element typed array)* — the source.

Computes `y[i] += alpha * x[i]` in place and returns `y`. `y` and `x` must match length.

**`affine(a, alpha, beta) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.
- `alpha` *(number)* — the slope.
- `beta` *(number)* — the intercept.

Computes `a[i] = alpha * a[i] + beta` in place and returns `a`.

### Activations

In-place (the underlying kernels allow `out` to alias `in`).

```js
import { sigmoid } from "dyna:simd";

console.log(Array.from(sigmoid(new Float32Array([0, 1]))).map(v => +v.toFixed(4)));  // [0.5, 0.7198]
```

**`sigmoid(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place `1 / (1 + exp(-x))`; returns `a`.

**`relu(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place `max(x, 0)`; returns `a`.

**`relu6(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place `min(max(x, 0), 6)`; returns `a`.

**`leakyRelu(a, slope) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.
- `slope` *(number)* — the negative-side slope.

In-place `x < 0 ? slope * x : x`; returns `a`.

**`elu(a, alpha) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.
- `alpha` *(number)* — the negative-side scale.

In-place `x < 0 ? alpha * (exp(x) - 1) : x`; returns `a`.

**`tanhFast(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place fast tanh via the sigmoid identity; returns `a`.

**`gelu(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place Gaussian error linear unit; returns `a`.

**`silu(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place `x * sigmoid(x)`, composed from `sigmoid` + `mul` so it is correct on every ISA; returns `a`.

### Softmax family

**`softmax(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

Stable softmax (max-shifted) in place; returns `a`. Throws on an empty array.

**`logSoftmax(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

Log-softmax in place; returns `a`. Throws on an empty array.

### Unary math

In-place; NaN propagates.

**`vexp(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place `e^x`; returns `a`.

**`vlog(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place natural log; returns `a`.

**`vsqrt(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place square root; returns `a`.

**`vrsqrt(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place reciprocal square root; returns `a`.

**`vinv(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place reciprocal; returns `a`.

### Distances

Vector→scalar over equal-length pairs; a length mismatch throws `RangeError`.

```js
import { distL2, distL1 } from "dyna:simd";

const a = new Float32Array([1, 0]), b = new Float32Array([0, 1]);
console.log(distL2(a, b), distL1(a, b));  // 1.414... 2
```

**`distL2(a, b) -> number`**

- `a`, `b` *(Float32Array or 4-byte-element typed array)* — equal-length vectors.

Returns the Euclidean distance.

**`distL1(a, b) -> number`**

- `a`, `b` *(Float32Array or 4-byte-element typed array)* — equal-length vectors.

Returns the Manhattan distance.

**`distCos(a, b) -> number`**

- `a`, `b` *(Float32Array or 4-byte-element typed array)* — equal-length vectors.

Returns the cosine distance (`1 - cos`).

**`distCheb(a, b) -> number`**

- `a`, `b` *(Float32Array or 4-byte-element typed array)* — equal-length vectors.

Returns the Chebyshev (max-coordinate) distance.

### BLAS-2/3

Row-major, explicit dimensions, in-place on the output argument. Dimension products are checked for overflow, and the buffer lengths are validated against the declared dims before any kernel runs.

```js
import { gemv } from "dyna:simd";

const A = new Float32Array([1, 2, 3, 4]);            // 2x2 row-major
console.log(Array.from(gemv(new Float32Array(2), A, new Float32Array([1, 1]), 2, 2, 0)));  // [3,7]
```

**`gemv(y, a, x, m, n, beta) -> y`**

**`gemv(y, a, x, m, n, alpha, beta) -> y`**

- `y` *(Float32Array or 4-byte-element typed array)* — the length-`m` output.
- `a` *(Float32Array or 4-byte-element typed array)* — the `m×n` matrix (`a.length == m*n`).
- `x` *(Float32Array or 4-byte-element typed array)* — the length-`n` input.
- `m`, `n` *(number)* — the matrix dimensions.
- `alpha`, `beta` *(number)* — the accumulation scales.

The 6-arg legacy form computes `y = beta*y + A*x`; the 7-arg form (dispatched on the argument count) computes `y = alpha*A*x + beta*y` with gemm's alpha/beta semantics. `beta == 0` skips the `y` read in both (a NaN-poisoned `y` is safe). Returns `y`.

```js
import { gemv } from "dyna:simd";

const A = new Float32Array([1, 2, 3, 4]);            // 2x2 row-major
console.log(Array.from(gemv(new Float32Array(2), A, new Float32Array([1, 1]), 2, 2, 0)));  // [3,7]
console.log(Array.from(gemv(new Float32Array([5, 6]), A, new Float32Array([1, 1]), 2, 2, 2, 3)));  // 2*[3,7] + 3*[5,6] = [21,32]
```

**`gemvT(y, a, x, m, n, beta) -> y`**

- `y` *(Float32Array or 4-byte-element typed array)* — the length-`n` output.
- `a` *(Float32Array or 4-byte-element typed array)* — the `m×n` matrix (`a.length == m*n`).
- `x` *(Float32Array or 4-byte-element typed array)* — the length-`m` input.
- `m`, `n` *(number)* — the matrix dimensions.
- `beta` *(number)* — the accumulation scale.

Computes `y = beta*y + Aᵀ*x` and returns `y`.

**`gemm(c, a, b, m, n, k, alpha, beta) -> c`**

- `c` *(Float32Array or 4-byte-element typed array)* — the `m×n` output.
- `a` *(Float32Array or 4-byte-element typed array)* — the `m×k` matrix.
- `b` *(Float32Array or 4-byte-element typed array)* — the `k×n` matrix.
- `m`, `n`, `k` *(number)* — the matrix dimensions.
- `alpha`, `beta` *(number)* — the scales.

Computes `C = alpha*A*B + beta*C` and returns `c`.

### Comparison / selection

**`clamp(a, lo, hi) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.
- `lo`, `hi` *(number)* — the bounds.

In-place `min(max(x, lo), hi)`; returns `a`.

**`threshold(a, t) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.
- `t` *(number)* — the threshold.

In-place binarise: `x > t ? 1.0 : 0.0`; returns `a`.

**`topkIndices(vals, k) -> Uint32Array`**

- `vals` *(Float32Array or 4-byte-element typed array)* — the values.
- `k` *(number)* — how many largest indices.

Returns the indices of the `min(k, vals.length)` largest values as a fresh array (unspecified order — min-heap selection).

### Statistics

Composed from the `sum`/`dot` kernels, so the f32 forms inherit their relative-tolerance regime; NaN in the input propagates to NaN. Empty input throws `RangeError` (a mean/variance of nothing is undefined, like `max`). `normalize` joins the softmax family in taking one array and working in place.

```js
import { mean, variance, normalize } from "dyna:simd";

const x = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
console.log(mean(x), variance(x));   // 4.5 5.25
console.log(Array.from(normalize(x)).map(v => +v.toFixed(3)));  // [-1.528,-1.091,-0.655,-0.218,0.218,0.655,1.091,1.528], mean 0 / std 1
```

**`mean(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns `sum(a)/n`. Throws `RangeError` on an empty array.

**`variance(a) -> number`**

- `a` *(Float32Array or 4-byte-element typed array)* — the input.

Returns the two-pass centered population variance: `mean`, then `dot(a-mean, a-mean)/n`. The centered form avoids the `sum(a²)/n - mean²` cancellation; one scratch allocation. Throws `RangeError` on an empty array.

**`normalize(a) -> a`**

- `a` *(Float32Array or 4-byte-element typed array)* — the array.

In-place z-score `a[i] = (a[i]-mean)/std`; returns `a`. A constant array has no meaningful z-score: when the centering cancels exactly the output is all-NaN (IEEE `0/0`), else ±1-shaped rounding noise.

### f64 kernels

Zero-copy over a `Float64Array` (JS `Number` is f64). `f64Sum`/`f64Dot` reorder additions and round slightly differently from a sequential sum; `f64Max`/`f64Min`/`f64Scale`/`f64Axpy` are bit-exact.

```js
import { f64Sum } from "dyna:simd";

console.log(f64Sum(new Float64Array([1, 2, 3])));   // 6
```

**`f64Sum(a) -> number`**

- `a` *(Float64Array)* — the input.

Returns the sum. Reorders additions and rounds slightly differently from a sequential sum.

**`f64Dot(a, b) -> number`**

- `a`, `b` *(Float64Array)* — the inputs.

Returns the inner product. Reorders additions and rounds slightly differently from a sequential sum. Throws on a length mismatch.

**`f64Max(a) -> number`**

- `a` *(Float64Array)* — the input.

Returns the maximum; throws on an empty array. Bit-exact.

**`f64Min(a) -> number`**

- `a` *(Float64Array)* — the input.

Returns the minimum; throws on an empty array. Bit-exact.

**`f64Scale(a, s) -> a`**

- `a` *(Float64Array)* — the array.
- `s` *(number)* — the scalar.

In-place `a[i] *= s`; returns `a`. Bit-exact.

**`f64Axpy(y, alpha, x) -> y`**

- `y` *(Float64Array)* — the destination.
- `alpha` *(number)* — the scalar multiplier.
- `x` *(Float64Array)* — the source.

In-place `y[i] += alpha * x[i]`; returns `y`. Non-fused, so bit-identical to the scalar reference on every ISA.

**`f64Mean(a) -> number`**

- `a` *(Float64Array)* — the input.

Returns `f64Sum(a)/n`; throws `RangeError` on an empty array.

**`f64Variance(a) -> number`**

- `a` *(Float64Array)* — the input.

Two-pass centered population variance (`f64Sum` for the mean, a centered copy, `f64Dot` for the spread); throws `RangeError` on an empty array.

### i32 kernels

Zero-copy over an `Int32Array` only. Wrapping ops match JS two's-complement: `i32Add` wraps mod 2³² like `(a+b)|0`; `i32Mul`/`i32Scale` keep the low 32 bits like `Math.imul`. Reductions are exact and bit-identical to a sequential scalar loop.

```js
import { i32Sum } from "dyna:simd";

console.log(i32Sum(new Int32Array([1, 2, 3])));   // 6
```

**`i32Sum(a) -> number`**

- `a` *(Int32Array)* — the input.

Exact (the kernel accumulates in int64) for `|sum| <= 2^53`.

**`i32Min(a) -> number`**

- `a` *(Int32Array)* — the input.

Exact minimum; throws on an empty array.

**`i32Max(a) -> number`**

- `a` *(Int32Array)* — the input.

Exact maximum; throws on an empty array.

**`i32Mean(a) -> number`**

- `a` *(Int32Array)* — the input.

`i32Sum(a)/n` in double: the kernel's int64 accumulation cannot overflow for any array that fits memory, is exact as a double while `|sum| <= 2^53`, and the divide is one double rounding. Throws on an empty array.

**`i32Dot(a, b) -> number`**

- `a`, `b` *(Int32Array)* — the inputs.

Sum of double products; matches a sequential dot to a relative tolerance.

**`i32Add(out, a, b) -> out`**

- `out` *(Int32Array)* — the destination.
- `a`, `b` *(Int32Array)* — the operands.

Writes into a separate `out`, wrapping per element (mod 2³², like `(a+b)|0`); returns `out`.

**`i32Mul(out, a, b) -> out`**

- `out` *(Int32Array)* — the destination.
- `a`, `b` *(Int32Array)* — the operands.

Writes into a separate `out`, keeping the low 32 bits per element (like `Math.imul`); returns `out`.

**`i32Scale(a, s) -> a`**

- `a` *(Int32Array)* — the array.
- `s` *(number)* — the scalar.

In-place low-32-bit multiply; returns `a`.

### Prefix scans

```js
import { cumsum } from "dyna:simd";

console.log(Array.from(cumsum(new Int32Array([1, 2, 3]))));   // [1,3,6]
```

**`cumsum(a) -> a`**

- `a` *(Int32Array or Float32Array)* — the array; the element type is selected by class id, not bytes-per-element.

Inclusive prefix sum, in place; returns `a`. The i32 scan is exact; the f32 scan reorders additions vs a left fold, so it matches only to a relative tolerance.

**`cummax(a) -> a`**

- `a` *(Int32Array or Float32Array)* — the array; the element type is selected by class id, not bytes-per-element.

Inclusive prefix maximum, in place; returns `a`. Exact for both (max rounds nothing).

Showcase — reductions, element-wise ops, activations, distances and prefix scans:

```js
import { sum, max, min, argmax, argmin, normL1, normL2, add, dot, scale, axpy, clamp, threshold, sigmoid, softmax, topkIndices, distL2, distCos, gemm, cumsum, cummax, f64Sum, f64Dot, i32Sum, i32Add, i32Mul } from "dyna:simd";

const x = new Float32Array([1, 2, 3, 4]);
print(sum(x), max(x), min(x), argmax(x), argmin(x), normL1(x), normL2(x).toFixed(3));
// 10 4 1 3 0 10 5.477

// element-wise with a separate out; all lengths must match
const a = new Float32Array([1, 2, 3]), b = new Float32Array([4, 5, 6]);
print(Array.from(add(new Float32Array(3), a, b)));   // [5,7,9]
print(dot(a, b));                                    // 32
print(Array.from(scale(new Float32Array([1,2]), 10))); // [10,20]
const y = new Float32Array([0,0]);
axpy(y, 2, new Float32Array([1,2]));
print(Array.from(y));                                // [2,4]

// in-place activations (out may alias in)
print(Array.from(sigmoid(new Float32Array([0, 1]))).map(v => +v.toFixed(4))); // [0.5,0.7198]
print(Array.from(softmax(new Float32Array([1, 2]))).map(v => +v.toFixed(3))); // [0.28,0.72]
print(Array.from(clamp(new Float32Array([-5, 0, 5]), -1, 1))); // [-1,0,1]
print(Array.from(threshold(new Float32Array([0.1, 0.9]), 0.5))); // [0,1]
print(Array.from(topkIndices(new Float32Array([1, 5, 3]), 2)));  // indices of the 2 largest

// distances over equal-length pairs
print(distL2(new Float32Array([0,0]), new Float32Array([3,4]))); // 5
print(distCos(new Float32Array([1,0]), new Float32Array([0,1]))); // 0 (orthogonal)

// BLAS-2/3, row-major, explicit dims: c = a*b (2x2)
const c = new Float32Array(4);
gemm(c, new Float32Array([1,0,0,1]), new Float32Array([1,2,3,4]), 2, 2, 2, 1, 0);
print(Array.from(c)); // [1,2,3,4] — I * M

// f64 + i32 families (zero-copy, exact reductions)
print(f64Sum(new Float64Array([1,2,3])), f64Dot(new Float64Array([1,2]), new Float64Array([3,4])));
// 6 11
print(i32Sum(new Int32Array([1,2,3])));                                // 6
print(Array.from(i32Mul(new Int32Array(2), new Int32Array([2,3]), new Int32Array([4,5])))); // [8,15]
print(Array.from(cumsum(new Int32Array([1,2,3]))));  // [1,3,6]
print(Array.from(cummax(new Float32Array([3,1,2]))));// [3,3,3]
```

# dyna:dataframe

Columnar tables over TypedArrays, with reductions, group-by, windowing,
quantiles and frame reshaping. Numeric columns alias a TypedArray's buffer;
string columns are dictionary-encoded (the one place input is copied).

`import { DataFrame, isDataFrame } from "dyna:dataframe";`

The runtime type test `isDataFrame(v)` is `true` only for frames this engine
built (the constructor, or any verb returning a frame) — a brand check, not
duck typing, so frame-shaped plain objects and `DataFrame.prototype`-chained
objects without a brand are `false`.

```js
import { DataFrame, isDataFrame } from "dyna:dataframe";

const df = new DataFrame({ k: new Int32Array([0, 1, 0]),
                           x: new Float64Array([1, 2, 3]) });
print(df.SUM("x"));                                  // 6
print(JSON.stringify(df.GROUP_BY_SUM("k", "x").keys)); // [0,1]
print(isDataFrame(df));                              // true
print(isDataFrame({ ROWS: 2, COLUMNS: ["a"] }));     // false (duck look-alike)
print(isDataFrame([1, 2, 3]), isDataFrame("df"));    // false false
```

### DataFrame

**`new DataFrame(columns) -> DataFrame`**

- `columns` *(Object)* — maps a column name to a TypedArray (Float64/Float32/Int32/Uint32/Int16/Uint16/Int8/Uint8Array) or a JS array (of strings, or of numbers — see below).

Returns a new frame. All columns must share one length; more than 1024 columns, a name containing U+0000, or a Uint8ClampedArray/BigInt array is refused. Numeric columns ALIAS the buffer you pass — mutating the source array after construction changes the frame (`TO_COLUMNS()` and `COPY()` hand out fresh copies); string columns are dictionary-encoded and owned by the frame. A plain `number[]` column is accepted but stored as a STRING column (`DTYPES` reports `"str"` and the numeric verbs refuse it), so use TypedArrays for numeric data.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    id: new Int32Array([1, 2, 3, 4]),
    x: new Float64Array([3.0, 1.0, 2.0, 4.0]),
    cat: ["a", "b", "a", "b"]
});
print(df.ROWS, df.COLS, df.COLUMNS.join(","));          // 4 3 id,x,cat
print(df.DTYPES().x, JSON.stringify(df.SCHEMA()[1]));   // f64 {"name":"x","type":"f64"}
print(df.TO_CSV().split("\n")[0], df.INFO().rows);
print(JSON.stringify(df.TO_RECORDS()[0]));              // {"id":1,"x":3,"cat":"a"}
print(df.SELECT(["cat", "x"]).COLUMNS.join(","));       // cat,x
print(df.DROP_COLUMNS(["cat"]).COLUMNS.join(","));      // id,x
print(df.RENAME({ x: "score" }).COLUMNS.join(","));     // id,score,cat
print(df.FILTER(new Uint8Array([1, 0, 1, 1])).ROWS);    // 3
print(df.SLICE(1, 3).ROWS, df.SAMPLE(2, 7).ROWS, df.COPY().ROWS);
print(df.ISIN("cat", ["a"]).join(""));                  // 1010
```

Showcase — schema inspection and zero-copy ownership:

```js
import { DataFrame } from "dyna:dataframe";

const buf = new Float64Array([10, 20, 30]);
const df = new DataFrame({ id: new Int32Array([1,2,3]), v: buf, tag: ["x","y","x"] });
print(df.INFO().total_bytes > 0, df.MEMORY_USAGE().total > 0); // true true
print(JSON.stringify(df.DTYPES())); // {"id":"i32","v":"f64","tag":"str"}
print(df.SCHEMA().map(c=>c.name+":"+c.type).join(",")); // id:i32,v:f64,tag:str
// numeric aliases the buffer — mutating the buffer mutates the frame
buf[0] = 99; print(df.FILTER(new Uint8Array([1,1,1])).TO_COLUMNS().v[0]); // 99 (live)
print(df.COPY().TO_COLUMNS().v[0]); // 99 — copy freezes current values
buf[0] = 10; print(df.TO_COLUMNS().v[0]); // 10 — v still aliases buf; TO_COLUMNS snapshots at call time
// FROM_RECORDS — union of keys, NaN/"" fill, first-seen column order
const df2 = new DataFrame({}).FROM_RECORDS([{a:1,b:"x"}, {a:2}, {b:"y",a:3}]);
print(df2.COLUMNS.join(","), df2.ROWS); // a,b 3
print(df2.TO_RECORDS()[1].b === ""); // true (missing b)
print(df2.MASK(new Uint8Array([1,0,1]), 0).ROWS); // 3 (masked rows become 0)
```

**`DataFrame.ROWS -> Number`**

Returns the row count. A getter.

**`DataFrame.COLS -> Number`**

Returns the column count. A getter.

**`DataFrame.COLUMNS -> Array<String>`**

Returns the column names in column order.

**`DataFrame.DTYPES() -> Object`**

Returns a map of column name to a short type tag: `f64`, `f32`, `i32`, `u32`, `i16`, `u16`, `i8`, `u8`, or `str` for a string column.

**`DataFrame.SCHEMA() -> Array<{name, type}>`**

Returns one entry per column, in order, each holding `name` and `type`.

**`DataFrame.INFO() -> Object`**

Returns `{rows, cols, dtypes, bytes, total_bytes}`. `bytes` maps each column to the bytes it holds (for a string column: the code array plus its dictionary).

**`DataFrame.MEMORY_USAGE() -> Object`**

Returns `{columns: {name: bytes}, total}`.

**`DataFrame.TO_COLUMNS() -> Object`**

Returns a map of column name to a fresh TypedArray copy (numeric) or an array of strings. Copies, never aliases.

**`DataFrame.TO_RECORDS() -> Array<Object>`**

Returns one object per row. The largest output this family emits.

**`DataFrame.TO_JSON() -> String`**

Returns the `TO_RECORDS` array serialised once. NaN and Infinity become `null`.

**`DataFrame.TO_CSV([opts]) -> String`**

- `opts.escapeFormulas` *(boolean)* — prefixes a `'` to cells starting with `=`, `+`, `-` or `@` so spreadsheet applications do not execute them as formulas when the CSV is opened (CSV/formula injection).

Returns a header row plus one row per frame row, with RFC 4180 quoting. A NaN numeric value becomes an empty field.

**`DataFrame.FROM_RECORDS(rows) -> DataFrame`**

- `rows` *(Array<Object>)* — the row objects to build the frame from.

Returns a new frame. The column set is the union of keys in first-seen order; a column is numeric only if EVERY value in every row is a JS number (stored as Float64), else a string column. A row missing a key gives NaN or `""`. Zero rows is refused.

**`DataFrame.COPY() -> DataFrame`**

Returns a fresh frame whose columns are exact copies, never aliased.

**`DataFrame.SELECT(names[]) -> DataFrame`**

- `names` *(Array<string>)* — the columns to keep, in the order given.

Returns the named columns in the order given, rows unchanged. A name listed twice is refused.

**`DataFrame.DROP_COLUMNS(names[]) -> DataFrame`**

- `names` *(Array<string>)* — the columns to drop.

Returns the complement frame, in column order.

**`DataFrame.RENAME(map) -> DataFrame`**

- `map` *(Object)* — an `{old: new}` mapping of old column names to new ones.

Returns a frame with columns renamed per the map. An unknown old name, a target colliding with an unrenamed column, or a duplicate target is refused.

**`DataFrame.FILTER(mask) -> DataFrame`**

- `mask` *(Uint8Array)* — a row mask of `ROWS` bytes.

Returns the rows where mask is nonzero, in order.

**`DataFrame.SLICE(start[, end]) -> DataFrame`**

- `start` *(Number)* — the first row to keep.
- `end` *(Number, optional)* — the first row to exclude.

Returns rows `[start, end)`, clamped to `[0, ROWS]`. Negative indices count from the end like `Array.prototype.slice`.

**`DataFrame.SAMPLE(n[, seed]) -> DataFrame`**

- `n` *(Number)* — the number of rows to sample.
- `seed` *(Number, optional)* — a seed for reproducible sampling.

Returns `n` rows without replacement via a Fisher-Yates partial shuffle. `n` above `ROWS` is refused. With a seed, the same seed reproduces the same rows; without one, the seed is time-derived.

**`DataFrame.ISIN(col, values[]) -> Uint8Array`**

- `col` *(string)* — the column to test.
- `values` *(Array)* — the values to match.

Returns a mask that is 1 where the column's value is in `values`. Numeric columns match numbers, string columns match strings.

**`DataFrame.MASK(mask[, fill]) -> DataFrame`**

- `mask` *(Uint8Array)* — a row mask of `ROWS` bytes.
- `fill` *(Number | String, optional)* — the value rows where mask is 0 become; default NaN for numeric, `""` for string.

Returns a frame of the same shape; rows where mask is 0 become `fill`. This is pandas' `where`.

### DataFrame.reductions

Every reduction takes `(col[, mask])`, where mask is an optional Uint8Array of `ROWS` bytes; masked-out rows do not contribute. MIN/MAX ignore NaN and return `undefined` on an empty selection; MEAN returns NaN; SUM returns 0. A string column is refused. Float folds keep their stated reassociation tolerance.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    x: new Float64Array([1, 2, 3, 4]),
    id: new Int32Array([1, 2, 3, 4])
});
print(df.SUM("x"), df.MIN("x"), df.MAX("x"), df.MEAN("x"), df.COUNT("x"));
print(df.PRODUCT("x"), df.DOT_PRODUCT("x", "x"));        // 24 30
print(df.VARIANCE("x"), df.STDDEV("x"));                 // 1.666... 1.29099...
print(df.VARIANCE_POP("x"), df.STDDEV_POP("x"), df.SKEW("x"), df.KURTOSIS("x"));
print(df.SKEW_SAMP("x"), df.KURT_SAMP("x"), df.SEM("x"), df.COUNT_NULLS("x"));
print(df.MEAN_WEIGHTED("x", "id"), df.SUM_CHECKED("id")); // 2.7 10
print(df.DESCRIBE("x").count, df.DESCRIBE("x").mean);    // 4 2.5
print(df.ENTROPY("x"), df.MAD("x"), df.MEDIAN_ABSOLUTE_DEVIATION("x"));
const m = new Uint8Array([1, 0, 1, 1]);
print(df.SUM("x", m), df.MEAN("x", m));                  // 8 2.666...
```

**`DataFrame.SUM(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum. Integer columns accumulate in int64/uint64, float columns in double.

**`DataFrame.MIN(col, mask?) -> Number | undefined`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the minimum, ignoring NaN. `undefined` when nothing is selected.

**`DataFrame.MAX(col, mask?) -> Number | undefined`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the maximum, ignoring NaN.

**`DataFrame.MEAN(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum over the count. NaN on an empty selection.

**`DataFrame.COUNT(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the number of rows, or of nonzero mask bytes.

**`DataFrame.PRODUCT(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the product, accumulated in double for every column type.

**`DataFrame.DOT_PRODUCT(a, b, mask?) -> Number`**

- `a` *(string)* — the first column.
- `b` *(string)* — the second column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum of `a[i] * b[i]` over the selection, via specialised same-type kernels and block-widened mixed pairs.

**`DataFrame.VARIANCE(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sample variance (n-1), two-pass. NaN with fewer than two selected rows.

**`DataFrame.STDDEV(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sample standard deviation.

**`DataFrame.VARIANCE_POP(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the population variance (/n), defined from one row up.

**`DataFrame.STDDEV_POP(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the population standard deviation.

**`DataFrame.SKEW(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the population skewness.

**`DataFrame.KURTOSIS(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the population excess kurtosis.

**`DataFrame.SKEW_SAMP(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sample skewness. 0 below n=3.

**`DataFrame.KURT_SAMP(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sample excess kurtosis. 0 below n=4.

**`DataFrame.SEM(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the standard error of the mean: sample stddev over sqrt(n). NaN when n <= 1.

**`DataFrame.COUNT_NULLS(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the number of NaN values in the selection.

**`DataFrame.MEAN_WEIGHTED(valueCol, weightCol, mask?) -> Number`**

- `valueCol` *(string)* — the column of values.
- `weightCol` *(string)* — the column of weights.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum of `w*x` over sum of `w`. A zero weight contributes nothing (it is not in the input set), and all-zero weights give NaN.

**`DataFrame.SUM_CHECKED(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the exact integer sum. Integer columns only (a float column is refused by name); throws a RangeError when the total cannot be held by a Number rather than returning the rounded value `SUM` would.

**`DataFrame.DESCRIBE(col, mask?) -> Object`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns `{count, sum, mean, min, max, variance, stddev, skew, kurtosis}` in one moments pass.

**`DataFrame.ENTROPY(col, mask?) -> Number`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the Shannon entropy in bits over the empirical value distribution.

**`DataFrame.MAD(col, mask?) -> Number | undefined`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the mean absolute deviation from the mean. `undefined` when nothing is selected.

**`DataFrame.MEDIAN_ABSOLUTE_DEVIATION(col, mask?) -> Number | undefined`**

- `col` *(string)* — the column to reduce.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the median of `|x - median|`. `undefined` when nothing is selected.

### DataFrame.bitwise

Integer columns only; a float column is refused by name rather than coerced. Folds run in uint32 with the same empty-selection identities the reductions publish.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ id: new Int32Array([1, 2, 3, 4]), cat: ["a", "b", "a", "b"] });
print(df.BITWISE_AND("id"), df.BITWISE_OR("id"), df.BITWISE_XOR("id")); // 0 7 4
print(JSON.stringify(Array.from(df.GROUP_BIT_AND("cat", "id").values)));
print(JSON.stringify(Array.from(df.GROUP_BIT_OR("cat", "id").values)));
print(JSON.stringify(Array.from(df.GROUP_BIT_XOR("cat", "id").values)));
print(df.GROUP_BITMAP("id"));  // 4
```

**`DataFrame.BITWISE_AND(col, mask?) -> Number`**

- `col` *(string)* — the integer column to fold.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the bitwise AND of the selection. Identity ~0 over an empty one.

**`DataFrame.BITWISE_OR(col, mask?) -> Number`**

- `col` *(string)* — the integer column to fold.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the bitwise OR of the selection. Identity 0.

**`DataFrame.BITWISE_XOR(col, mask?) -> Number`**

- `col` *(string)* — the integer column to fold.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the bitwise XOR of the selection. Identity 0.

**`DataFrame.GROUP_BIT_AND(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column.
- `val` *(string)* — the integer value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group bitwise AND of the integer value column. An untouched group keeps ~0.

**`DataFrame.GROUP_BIT_OR(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column.
- `val` *(string)* — the integer value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group bitwise OR of the integer value column. An untouched group keeps 0.

**`DataFrame.GROUP_BIT_XOR(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column.
- `val` *(string)* — the integer value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group bitwise XOR of the integer value column. An untouched group keeps 0.

**`DataFrame.GROUP_BITMAP(col, mask?) -> Number`**

- `col` *(string)* — the integer column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the count of DISTINCT non-negative integer values, one bit per value. A negative value is refused, and a value at or above 2^26 is refused with a pointer to N_UNIQUE (which is bounded by the row count, not the value range).

### DataFrame.positional

`HEAD`/`TAIL` peek at a column, `FIRST`/`LAST` read one selected value, and `ARG_MIN`/`ARG_MAX` return a row index; `col[i]` equals exactly what `MIN`/`MAX` returned for the same column and mask.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ x: new Float64Array([3, 1, 2, 4]) });
print(JSON.stringify(Array.from(df.HEAD("x", 2))));  // [3,1]
print(JSON.stringify(Array.from(df.TAIL("x", 2))));  // [2,4]
print(df.FIRST("x"), df.LAST("x"));                  // 3 4
print(df.ARG_MIN("x"), df.ARG_MAX("x"));             // 1 3
```

**`DataFrame.HEAD(col[, n[, mask]]) -> Float64Array`**

- `col` *(string)* — the column to peek.
- `n` *(Number, optional)* — the number of values; defaults to 5.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the first `n` selected values. `n` is clamped to the frame, never silently truncated below it, and must be a non-negative number.

**`DataFrame.TAIL(col[, n[, mask]]) -> Float64Array`**

- `col` *(string)* — the column to peek.
- `n` *(Number, optional)* — the number of values; defaults to 5.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the last `n` selected values, with the same clamping.

**`DataFrame.FIRST(col[, mask]) -> Number | undefined`**

- `col` *(string)* — the column to read.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the value of the first SELECTED row. `undefined` when nothing is selected (never NaN, so it cannot be confused with a NaN value).

**`DataFrame.LAST(col[, mask]) -> Number | undefined`**

- `col` *(string)* — the column to read.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the value of the last selected row.

**`DataFrame.ARG_MIN(col[, mask]) -> Number | undefined`**

- `col` *(string)* — the column to search.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the row index of the minimum. Ties go to the FIRST occurrence, NaN values are skipped.

**`DataFrame.ARG_MAX(col[, mask]) -> Number | undefined`**

- `col` *(string)* — the column to search.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the row index of the maximum. Ties go to the first occurrence.

### DataFrame.mask operations

Masks are Uint8Arrays of `ROWS` bytes. The comparison verbs return one; ALL, ANY, BITMASK and the BOOL_* reductions consume one.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    x: new Float64Array([1, 2, 3, 4]),
    f: new Float64Array([1, NaN, 3, NaN])
});
print(df.GT("x", 2).join(""), df.GE("x", 2).join(""));  // 0011 0111
print(df.LT("x", 2).join(""), df.LE("x", 2).join(""));  // 1000 1100
print(df.EQ("x", 2).join(""), df.NE("x", 2).join(""));  // 0100 1011
print(df.BETWEEN("x", 2, 3).join(""), df.IS_NA("f").join(""));
print(df.NOT_NA("f").join(""), df.ALL(df.GT("x", 0)), df.ANY(df.GT("x", 3)));
print(df.BITMASK(df.GT("x", 2))[0], df.BOOL_AND("f"), df.BOOL_OR("f"), df.BOOL_XOR("f"));
print(df.DROP_DUPLICATES("x").join(""), df.DROP_NA().join("")); // 1111 1010
```

**`DataFrame.GT(col, value) -> Uint8Array`**

- `col` *(string)* — the column to compare.
- `value` *(Number)* — the threshold.

Returns a mask that is 1 where `col[i] > value`; one pass, no per-row JS values.

**`DataFrame.GE(col, value) -> Uint8Array`**

- `col` *(string)* — the column to compare.
- `value` *(Number)* — the threshold.

Returns a mask that is 1 where `col[i] >= value`.

**`DataFrame.LT(col, value) -> Uint8Array`**

- `col` *(string)* — the column to compare.
- `value` *(Number)* — the threshold.

Returns a mask that is 1 where `col[i] < value`.

**`DataFrame.LE(col, value) -> Uint8Array`**

- `col` *(string)* — the column to compare.
- `value` *(Number)* — the threshold.

Returns a mask that is 1 where `col[i] <= value`.

**`DataFrame.EQ(col, value) -> Uint8Array`**

- `col` *(string)* — the column to compare.
- `value` *(Number)* — the threshold.

Returns a mask that is 1 where `col[i] == value`.

**`DataFrame.NE(col, value) -> Uint8Array`**

- `col` *(string)* — the column to compare.
- `value` *(Number)* — the threshold.

Returns a mask that is 1 where `col[i] != value`.

**`DataFrame.BETWEEN(col, lo, hi) -> Uint8Array`**

- `col` *(string)* — the column to test.
- `lo` *(Number)* — the lower bound.
- `hi` *(Number)* — the upper bound.

Returns a mask that is 1 where `lo <= col[i] <= hi`, inclusive at both ends. An inverted range selects nothing.

**`DataFrame.IS_NA(col) -> Uint8Array`**

- `col` *(string)* — the column to test.

Returns a mask that is 1 where the value is NaN.

**`DataFrame.NOT_NA(col) -> Uint8Array`**

- `col` *(string)* — the column to test.

Returns a mask that is 1 where the value is not NaN.

**`DataFrame.ALL(mask) -> Boolean`**

- `mask` *(Uint8Array)* — a row mask of `ROWS` bytes.

Returns true when every mask byte is nonzero.

**`DataFrame.ANY(mask) -> Boolean`**

- `mask` *(Uint8Array)* — a row mask of `ROWS` bytes.

Returns true when any mask byte is nonzero.

**`DataFrame.BITMASK(mask) -> Uint32Array`**

- `mask` *(Uint8Array)* — a row mask of `ROWS` bytes.

Returns the mask packed into `ceil(ROWS/32)` words, LSB first. Bits past `ROWS` in the last word are zero.

**`DataFrame.BOOL_AND(col, mask?) -> Boolean`**

- `col` *(string)* — the column to fold.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the AND of the JS truthiness of the stored values (a float NaN is false, any nonzero int is true). Vacuous over zero selected rows: true.

**`DataFrame.BOOL_OR(col, mask?) -> Boolean`**

- `col` *(string)* — the column to fold.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the OR of the JS truthiness of the stored values. Vacuous over zero selected rows: false.

**`DataFrame.BOOL_XOR(col, mask?) -> Boolean`**

- `col` *(string)* — the column to fold.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the parity of the true count. Vacuous over zero selected rows: false.

**`DataFrame.DROP_DUPLICATES(col, mask?) -> Uint8Array`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns a mask that is 1 on the FIRST occurrence of each distinct value in the selection (SameValueZero; NaN is one value).

**`DataFrame.DROP_NA(...cols) -> Uint8Array`**

- `cols` *(...string)* — the columns to test; with no arguments every numeric column counts.

Returns a mask that is 1 where none of the named columns is NaN. String columns are refused (they cannot hold NaN).

### DataFrame.elementwise

Elementwise verbs return a Float64Array the same length as `ROWS`, so a result can be handed straight back to `new DataFrame`. A string column is refused on the left of every arithmetic verb.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    x: new Float64Array([-1.5, 2.5, 3.0, -0.0]),
    y: new Float64Array([10, 20, 30, 40])
});
print(df.ABS("x").join(","), df.SIGN("x").join(","));  // 1.5,2.5,3,0 -1,1,1,0
print(df.ROUND("x").join(","), df.FLOOR("x").join(","), df.CEIL("x").join(","));
print(df.CLIP("x", -1, 2).join(","), df.FILL_NA("x", 0).join(","));
print(df.ADD("x", 10).join(","), df.SUB("x", 1).join(","));
print(df.MUL("x", 2).join(","), df.DIV("x", 2).join(","));
print(df.POW("x", 2).join(","), df.RSUB("x", 10).join(","), df.RDIV("x", 10).join(","));
print(df.ADD("x", "y").join(","));                     // 8.5,22.5,33,40
print(df.WHERE(df.GT("x", 0), "x", "y").join(","));    // 10,2.5,3,-0
print(df.MASK(df.GT("x", 0), 0).TO_COLUMNS().x.join(",")); // 0,2.5,3,0
```

**`DataFrame.ABS(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns the absolute value of each element.

**`DataFrame.ROUND(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns each element rounded half away from zero.

**`DataFrame.FLOOR(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns the largest integer <= each value.

**`DataFrame.CEIL(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns the smallest integer >= each value.

**`DataFrame.SQRT(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns the square root of each element.

**`DataFrame.LOG(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns the natural logarithm of each element.

**`DataFrame.EXP(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns e raised to each value.

**`DataFrame.SIGN(col) -> Float64Array`**

- `col` *(string)* — the column to transform.

Returns -1, 0 or 1 per element.

**`DataFrame.CLIP(col, lo, hi) -> Float64Array`**

- `col` *(string)* — the column to clamp.
- `lo` *(Number)* — the lower bound.
- `hi` *(Number)* — the upper bound.

Returns each element clamped into `[lo, hi]`. NaN elements pass through unchanged. NaN bounds or `lo > hi` is refused.

### The `{out}` in-place form

Every elementwise verb below — the arithmetic family (`ABS` `ROUND` `FLOOR`
`CEIL` `SQRT` `LOG` `EXP` `SIGN`, `ADD` `SUB` `MUL` `DIV` `POW` `RSUB` `RDIV`,
`CLIP`, `FILL_NA`, in camelCase and SCREAMING spellings alike) and the window
family (`ROLLING_MIN/MAX/MEAN/SUM/VAR/STD`, `EMA`, `PCT_CHANGE`, `ZSCORE`) —
also accepts a trailing options bag `{out: colName}`. The result is written
INTO the named column and the FRAME is returned, so a pipeline stops
allocating a fresh `Float64Array` per step:

```js
const f = new DataFrame({
    x: new Float64Array([1, 2, 3]),
    x1: new Float64Array(3),
    x2: new Float64Array(3),
});
// out-of-place: a fresh Float64Array per step
const a = f.ADD("x", 1);
// in-place: results land in existing columns, f flows through
f.ADD("x", 1, { out: "x1" }).MUL("x1", 2, { out: "x2" });
console.log([...f.TO_COLUMNS().x2]);   // [4, 6, 8]
```

The contract is strict:

  - `out` must name an EXISTING column. It is never created.
  - The target column must be a `Float64Array` of exactly `nrows` entries.
    The verbs compute f64, so any other dtype would lose precision or
    silently truncate — a `TypeError` names the column and its actual type.
  - The column's BACKING BUFFER is mutated in place. When the frame was
    constructed over your own `Float64Array`, your array changes.
  - The bag is strict: `{"out"}` is its only key.
  - `out === source` (and `out` naming the other operand column) is safe:
    every verb computes into a C scratch buffer and stores at the end.
  - For the window family the bag may sit in the mask slot or after it
    (`pctChange("v", {out: "w"})` uses the default periods).
  - Where an element has no value (short source columns, masked-out rows,
    leading windows), the stored value is `NaN`, exactly what the
    out-of-place form would have returned.

Not covered: verbs whose output is not a `nrows`-long f64 vector (reductions,
`SORT`/`RANK`/quantiles/histogram) have no in-place form.

**`DataFrame.FILL_NA(col, value) -> Float64Array`**

- `col` *(string)* — the column to fill.
- `value` *(Number)* — the replacement value.

Returns the column with NaN replaced by `value`. A NaN `value` is legal and is a no-op.

**`DataFrame.ADD(col, x) -> Float64Array`**

- `col` *(string)* — the left operand column.
- `x` *(Number | string)* — a number, or the NAME OF ANOTHER COLUMN: a
  string operand is a column REFERENCE, so `ADD("a", "b")` is elementwise
  column + column (both operands are the frame's length — unequal column
  lengths are refused at construction, before any arithmetic). Unknown or
  string-dtype operand columns throw. `RSUB`/`RDIV` are number-only (two-column
  `k - col` is just `SUB(b, a)` with the arguments swapped).

Returns the elementwise sum.

**`DataFrame.SUB(col, x) -> Float64Array`**

- `col` *(string)* — the left operand column.
- `x` *(Number | string)* — a number or a column name.

Returns the elementwise difference.

**`DataFrame.MUL(col, x) -> Float64Array`**

- `col` *(string)* — the left operand column.
- `x` *(Number | string)* — a number or a column name.

Returns the elementwise product.

**`DataFrame.DIV(col, x) -> Float64Array`**

- `col` *(string)* — the left operand column.
- `x` *(Number | string)* — a number or a column name.

Returns the elementwise quotient.

**`DataFrame.POW(col, x) -> Float64Array`**

- `col` *(string)* — the base column.
- `x` *(Number | string)* — a number or a column name.

Returns the elementwise power. The five column-capable verbs (ADD/SUB/MUL/DIV/POW) widen operands once and run one f64 kernel per type pair.

**`DataFrame.RSUB(col, k) -> Float64Array`**

- `col` *(string)* — the column.
- `k` *(Number)* — the left operand; must be a number (`rsub(a, b)` on two columns is `sub(b, a)`).

Returns `k - col` elementwise.

**`DataFrame.RDIV(col, k) -> Float64Array`**

- `col` *(string)* — the column.
- `k` *(Number)* — the left operand; number-only.

Returns `k / col` elementwise.

**`DataFrame.WHERE(mask, a, b) -> Float64Array`**

- `mask` *(Uint8Array)* — a row mask of `ROWS` bytes.
- `a` *(string | Number)* — a column name or number for the nonzero rows.
- `b` *(string | Number)* — a column name or number for the zero rows.

Returns `a` where the mask byte is nonzero, else `b`.

### DataFrame.groupBy

Grouped verbs take a key column plus a value column and return `{keys, values}`: keys is an Array (the dictionary strings for a string key column, the integer group codes otherwise) and values a Float64Array aligned to it. The key column must be integer or string; a float key is refused, as is a negative key, and the group cardinality is capped at 2^20.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    cat: ["a", "b", "a", "b"],
    x: new Float64Array([3, 1, 2, 4]),
    id: new Int32Array([1, 2, 3, 4])
});
const g = df.GROUP_BY_SUM("cat", "x");
print(JSON.stringify(g.keys), JSON.stringify(Array.from(g.values))); // ["a","b"] [5,5]
print(JSON.stringify(Array.from(df.GROUP_BY_MEAN("cat", "x").values)));
print(JSON.stringify(Array.from(df.GROUP_BY_MIN("cat", "x").values)));
print(JSON.stringify(Array.from(df.GROUP_BY_MAX("cat", "x").values)));
print(JSON.stringify(Array.from(df.GROUP_BY_COUNT("cat").values)));  // [2,2]
print(JSON.stringify(Array.from(df.GROUP_ARRAY("cat", "x").values[0]))); // [3,2]
print(JSON.stringify(Array.from(df.GROUP_UNIQ_ARRAY("cat", "x").values[0])));
print(JSON.stringify(Array.from(df.GROUP_ARRAY_MOVING_SUM("cat", "x", 2).values[0])));
print(JSON.stringify(Array.from(df.GROUP_ARRAY_MOVING_AVG("cat", "x", 2).values[0])));
print(JSON.stringify(Array.from(df.GROUP_ARRAY_SORTED("cat", "x").values[0]))); // [2,3]
print(JSON.stringify(Array.from(df.GROUP_ARRAY_LAST("cat", "x", 1).values[0])));
print(JSON.stringify(Array.from(df.GROUP_ARRAY_SAMPLE("cat", "x", 2).values[0])));
print(JSON.stringify(Array.from(df.GROUP_ARRAY_INTERSECT("cat", "x")))); // []
print(JSON.stringify(Array.from(df.GROUP_ARRAY_INSERT_AT("x", "id", 4))));
print(df.GROUP_CONCAT("cat"), df.JSON_AGG("cat", "x"), df.JSON_OBJECT_AGG("cat", "x"));
```

**`DataFrame.GROUP_BY_SUM(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group sum of `val`. An empty group keeps the sum identity 0.

**`DataFrame.GROUP_BY_MEAN(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group mean. A group with no contributing row is NaN.

**`DataFrame.GROUP_BY_MIN(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group minimum, ignoring NaN. An empty group is NaN.

**`DataFrame.GROUP_BY_MAX(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group maximum, ignoring NaN. An empty group is NaN.

**`DataFrame.GROUP_BY_COUNT(key[, mask]) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns rows per group. It takes no value column (passing one is refused by name).

**`DataFrame.SUM_MAP(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns GROUP_BY_SUM under the name the map API uses. Identical call and result.

**`DataFrame.MIN_MAP(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns GROUP_BY_MIN by its map name.

**`DataFrame.MAX_MAP(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns GROUP_BY_MAX by its map name.

**`DataFrame.GROUP_ARRAY(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns values gathered per group; values is an Array of one Float64Array per key, in key order. An empty group is a zero-length array, never a hole.

**`DataFrame.GROUP_UNIQ_ARRAY(key, val, mask?) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns GROUP_ARRAY keeping only the first occurrence of each (group, value) pair.

**`DataFrame.GROUP_ARRAY_MOVING_SUM(key, val[, w][, mask]) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `w` *(Number, optional)* — the window width; an omitted `w` is EXPANDING, a given one must be a positive integer, and windows are partial at a group's start.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group moving sum. O(m) block decomposition above `w` = 256, O(m*w) re-sum below, one shared threshold.

**`DataFrame.GROUP_ARRAY_MOVING_AVG(key, val[, w][, mask]) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `w` *(Number, optional)* — the window width.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the per-group moving average. Divides by what CONTRIBUTED, never by `w`.

**`DataFrame.GROUP_ARRAY_SORTED(key, val[, mask]) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns each group's values sorted ascending, NaN last. Groups are qsorted, so one large group stays O(m log m).

**`DataFrame.GROUP_ARRAY_LAST(key, val, k[, mask]) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `k` *(Number)* — the number of rows kept; in [1, 65536].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the last `k` rows seen per group (a circular window, O(n) instead of O(n*k)).

**`DataFrame.GROUP_ARRAY_SAMPLE(key, val, k[, mask]) -> {keys, values}`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `k` *(Number)* — the sample stride count; in [1, 65536].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns a deterministic every-nth stride, so a rerun gives the same rows.

**`DataFrame.GROUP_ARRAY_INTERSECT(key, val[, mask]) -> Float64Array`**

- `key` *(string)* — the group key column (integer or string).
- `val` *(string)* — the value column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the values present in EVERY group. A repeat inside one group counts once. Only numeric value columns can be collected.

**`DataFrame.GROUP_ARRAY_INSERT_AT(value, position, size[, fill][, mask]) -> Float64Array`**

- `value` *(string)* — the column of values to insert.
- `position` *(string)* — the column of target positions.
- `size` *(Number)* — the number of slots; in 0..2^20.
- `fill` *(Number, optional)* — the value for empty slots; default 0.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns a dense array of `size` slots; each row writes its value at its `position`, a later row overwriting an earlier one. Out-of-range positions are dropped, never grown into; empty slots hold `fill`.

**`DataFrame.GROUP_CONCAT(col[, sep][, mask]) -> String`**

- `col` *(string)* — the column to join.
- `sep` *(String, optional)* — the separator; default `","`.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the selected values joined in row order with `sep`. String columns join their dictionary strings, numeric columns their numbers.

**`DataFrame.JSON_AGG(key, value[, mask]) -> String`**

- `key` *(string)* — the column of JSON keys.
- `value` *(string)* — the column of values.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns one JSON object mapping each key to an array of that group's values. Non-finite values serialise as `null`; a `__proto__` key is an own property, not a prototype write.

**`DataFrame.JSON_OBJECT_AGG(key, value[, mask]) -> String`**

- `key` *(string)* — the column of JSON keys.
- `value` *(string)* — the column of values.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns one JSON object mapping each key to the LAST value that appeared.

**`DataFrame.JSON_AGG_STRICT(key, value[, mask]) -> String`**

- `key` *(string)* — the column of JSON keys.
- `value` *(string)* — the column of values.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns JSON_AGG, but throws a RangeError on any non-finite value instead of serialising null.

**`DataFrame.JSON_OBJECT_AGG_STRICT(key, value[, mask]) -> String`**

- `key` *(string)* — the column of JSON keys.
- `value` *(string)* — the column of values.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns JSON_OBJECT_AGG with the same strictness.

### DataFrame.ordering

Sorting, ranking, frequency and approximate frequency. NaN sorts LAST in both directions; it is a missing value, not a large one.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    x: new Float64Array([3, 1, 2, 4]),
    cat: ["a", "b", "a", "c"]
});
print(JSON.stringify(Array.from(df.SORT("x"))));          // [1,2,3,4]
print(JSON.stringify(Array.from(df.ARG_SORT("x"))));      // [1,2,0,3]
print(JSON.stringify(Array.from(df.N_LARGEST("x", 2))));  // [4,3]
print(JSON.stringify(Array.from(df.N_SMALLEST("x", 2)))); // [1,2]
print(JSON.stringify(Array.from(df.RANK("x"))));          // [3,1,2,4]
print(JSON.stringify(Array.from(df.DENSE_RANK("x"))));    // [3,1,2,4]
print(JSON.stringify(Array.from(df.PERCENT_RANK("x"))));
print(JSON.stringify(Array.from(df.NTILE("x", 2))));      // [2,1,1,2]
print(df.N_UNIQUE("x"), df.UNIQ_UP_TO("x", 3));           // 4 4
print(JSON.stringify(df.UNIQUE("cat")));                  // ["a","b","c"]
print(df.MODE("cat"), df.APPROX_COUNT_DISTINCT("x"));     // "a" ~4
print(JSON.stringify(Array.from(df.APPROX_TOP_K("cat", 2).values)));
print(JSON.stringify(Array.from(df.APPROX_TOP_SUM("x", "x", 2).values)));
print(JSON.stringify(Array.from(df.TOP_K_WEIGHTED("x", "x", 2).values)));
print(df.ANY_HEAVY("x", "x"), df.APPROX_SIMILARITY("cat", "cat")); // undefined 1
```

**`DataFrame.SORT(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to sort.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the selected values ascending (ties by row index), NaN last.

**`DataFrame.ARG_SORT(col, mask?) -> Uint32Array`**

- `col` *(string)* — the column to sort.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns row indices in ascending value order.

**`DataFrame.RANK(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to rank.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns average ranks: ties share the mean of their positions, the only rule whose column sum stays invariant at n(n+1)/2. NaN rows stay NaN.

**`DataFrame.DENSE_RANK(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to rank.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns ranks counting DISTINCT values (no gap after a tie). NaN rows stay NaN.

**`DataFrame.PERCENT_RANK(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to rank.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns (min rank - 1) / (n - 1); a single valued row is 0. NaN rows stay NaN.

**`DataFrame.NTILE(col, buckets[, mask]) -> Float64Array`**

- `col` *(string)* — the column to tile.
- `buckets` *(Number)* — the number of tiles; a positive integer.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns SQL NTILE: the first `n % buckets` tiles take one extra row, so tile sizes differ by at most one. Ties are NOT kept together.

**`DataFrame.N_LARGEST(col, k, mask?) -> Float64Array`**

- `col` *(string)* — the column to select from.
- `k` *(Number)* — the number of values; a non-negative integer (fractional, negative or NaN is refused).
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the `k` largest values in descending order. A `k` larger than the selection clamps.

**`DataFrame.N_SMALLEST(col, k, mask?) -> Float64Array`**

- `col` *(string)* — the column to select from.
- `k` *(Number)* — the number of values; a non-negative integer (same validation as N_LARGEST).
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the `k` smallest values in ascending order.

**`DataFrame.UNIQUE(col, mask?) -> Float64Array | Array`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the distinct values in first-seen order; an Array of strings for a string column. SameValueZero: NaN is one value.

**`DataFrame.N_UNIQUE(col, mask?) -> Number`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the count of distinct values PRESENT in the selection (a string column's dictionary may hold more).

**`DataFrame.UNIQ_UP_TO(col, n[, mask]) -> Number`**

- `col` *(string)* — the column to scan.
- `n` *(Number)* — the distinct-count threshold; in [0, 65536].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the exact distinct count, or `n+1` meaning "more than n". Bounded work: it stops the moment the threshold is passed.

**`DataFrame.VALUE_COUNTS(col, mask?) -> {keys, values}`**

- `col` *(string)* — the column to count.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns each distinct value with its frequency, descending by frequency (ties by first appearance).

**`DataFrame.TOP_K(col, k[, mask]) -> {keys, values}`**

- `col` *(string)* — the column to rank.
- `k` *(Number)* — the number of values; required and must not be negative.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the `k` most FREQUENT values (the frequency question, not the magnitude one).

**`DataFrame.MODE(col, mask?) -> Number | String | undefined`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the most frequent value; ties go to the first in row order. `undefined` when nothing is selected, matching min/max.

**`DataFrame.APPROX_COUNT_DISTINCT(col, mask?) -> Number`**

- `col` *(string)* — the column to count.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns a HyperLogLog estimate (precision 14), unrounded. A string column is exact (its codes index a counter).

**`DataFrame.APPROX_TOP_K(col, k[, mask]) -> {keys, values}`**

- `col` *(string)* — the column to rank.
- `k` *(Number)* — the number of values; in [1, 1024].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns a Space-Saving estimate of the `k` most frequent values. String columns are exact. Same shape as the exact TOP_K, so the two are diffable.

**`DataFrame.APPROX_TOP_SUM(col, weightCol, k[, mask]) -> {keys, values}`**

- `col` *(string)* — the column to rank.
- `weightCol` *(string)* — the column of weights.
- `k` *(Number)* — the number of values.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns ranks by SUMMED WEIGHT, not count; the same answer TOP_K_WEIGHTED gives under the sketch API's name.

**`DataFrame.TOP_K_WEIGHTED(col[, weightCol], k[, mask]) -> {keys, values}`**

- `col` *(string)* — the column to rank.
- `weightCol` *(string, optional)* — the column of weights.
- `k` *(Number)* — the number of values; in [1, 65536].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the `k` values with the largest summed weight (or frequency when the weight column is omitted).

**`DataFrame.ANY_HEAVY(col[, weightCol][, mask]) -> Number | undefined`**

- `col` *(string)* — the column to scan.
- `weightCol` *(string, optional)* — the column of weights.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the value holding strictly more than half the total weight, or `undefined`.

**`DataFrame.APPROX_SIMILARITY(a, b, mask?) -> Number`**

- `a` *(string)* — the first column.
- `b` *(string)* — the second column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns a MinHash (bottom-k, 256 hashes) Jaccard estimate between two columns. Below 256 distinct values in the union the answer is exact. Both empty gives NaN.

### DataFrame.quantiles

Quantiles select rather than sort, so one q is O(n) with fixed passes. Exact forms return a value the column may not contain when they interpolate; the `_EXACT_*` forms return an order statistic.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ x: new Float64Array([1, 2, 3, 4, 5]) });
print(df.QUANTILE("x", 0.5));                    // 3
print(df.PERCENTILE_CONT("x", 0.5), df.PERCENTILE_DISC("x", 0.5)); // 3 3
print(df.MEDIAN("x"), df.QUANTILE_EXACT_LOW("x", 0.5), df.QUANTILE_EXACT_HIGH("x", 0.5));
print(JSON.stringify(Array.from(df.QUANTILES("x", [0.25, 0.5, 0.75]))));
print(JSON.stringify(Array.from(df.QUANTILES_TDIGEST("x", [0.25, 0.5, 0.75]))));
print(df.APPROX_PERCENTILE("x", 0.5));           // ~3
print(df.QUANTILE_EXACT_WEIGHTED("x", "x", 0.5), df.QUANTILE_TDIGEST_WEIGHTED("x", "x", 0.5));
const h = df.HISTOGRAM("x", 2);
print(JSON.stringify(Array.from(h.edges)), JSON.stringify(Array.from(h.counts)));
print(JSON.stringify(Array.from(df.HISTOGRAM_NORMALIZED("x", 2).counts)));
```

**`DataFrame.QUANTILE(col, q, mask?) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `q` *(Number)* — the quantile; must be in [0, 1].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the linear interpolation between order statistics (R type 7, pandas/numpy default). `undefined` when nothing is selected.

**`DataFrame.PERCENTILE_CONT(col, q, mask?) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `q` *(Number)* — the quantile.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the same interpolating form under the SQL name.

**`DataFrame.PERCENTILE_DISC(col, q, mask?) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `q` *(Number)* — the quantile.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the order statistic at `ceil(q * n)`, so the answer IS a value the column holds.

**`DataFrame.MEDIAN(col, mask?) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns QUANTILE at 0.5. Pandas-style NaN for an empty selection.

**`DataFrame.QUANTILE_EXACT_LOW(col, q, mask?) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `q` *(Number)* — the quantile.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the order statistic at `floor(q * (n-1))`, never interpolated.

**`DataFrame.QUANTILE_EXACT_HIGH(col, q, mask?) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `q` *(Number)* — the quantile.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the order statistic at `ceil(q * (n-1))`, never interpolated.

**`DataFrame.QUANTILES(col, qs[], mask?) -> Float64Array`**

- `col` *(string)* — the column to select from.
- `qs` *(Array<Number>)* — the quantiles to compute; bounded at 2^20.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns many interpolating quantiles from ONE gather: `k` selects are O(k*n) against O(n log n) for a sort. Answers come back in the caller's order.

**`DataFrame.QUANTILES_TDIGEST(col, qs[], mask?) -> Float64Array`**

- `col` *(string)* — the column to select from.
- `qs` *(Array<Number>)* — the quantiles to compute.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns many approximate quantiles off ONE t-digest (compression 100, shipped |rank - q| <= 0.01). Memory is fixed regardless of row count.

**`DataFrame.APPROX_PERCENTILE(col, q, mask?) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `q` *(Number)* — the quantile; in [0, 1].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns one t-digest quantile. `undefined` for no values.

**`DataFrame.QUANTILE_EXACT_WEIGHTED(col, weightCol, q[, mask]) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `weightCol` *(string)* — the column of weights.
- `q` *(Number)* — the quantile.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the value at which cumulative weight first reaches `q` of the total, so a row weighing three counts as three rows. Exact (sorts).

**`DataFrame.QUANTILE_TDIGEST_WEIGHTED(col, weightCol, q[, mask]) -> Number | undefined`**

- `col` *(string)* — the column to select from.
- `weightCol` *(string)* — the column of weights.
- `q` *(Number)* — the quantile.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the approximate form in bounded memory.

**`DataFrame.HISTOGRAM(col, bins[, mask]) -> {edges, counts}`**

- `col` *(string)* — the column to bin.
- `bins` *(Number)* — the number of bins; a positive integer up to 2^20.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns equal-width bins over the observed (non-NaN) range. `edges` has `bins+1` entries and the top edge is inclusive so the maximum is not lost.

**`DataFrame.HISTOGRAM_NORMALIZED(col, bins[, mask]) -> {edges, counts}`**

- `col` *(string)* — the column to bin.
- `bins` *(Number)* — the number of bins; a positive integer up to 2^20.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the same bins with counts divided by the number of contributing rows.

### DataFrame.scans

Windowed and sequential verbs return a Float64Array of exactly `ROWS` entries so results line up with the frame.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ x: new Float64Array([1, 2, 3, 4]) });
print(JSON.stringify(Array.from(df.CUM_SUM("x"))));    // [1,3,6,10]
print(JSON.stringify(Array.from(df.CUM_PROD("x"))));   // [1,2,6,24]
print(JSON.stringify(Array.from(df.CUM_MAX("x"))));    // [1,2,3,4]
print(JSON.stringify(Array.from(df.CUM_MIN("x"))));    // [1,1,1,1]
print(JSON.stringify(Array.from(df.SHIFT("x", 1))));   // [null,1,2,3]
print(JSON.stringify(Array.from(df.DIFF("x", 1))));    // [null,1,1,1]
print(JSON.stringify(Array.from(df.ROLLING_SUM("x", 2))));  // [null,3,5,7]
print(JSON.stringify(Array.from(df.ROLLING_MEAN("x", 2)))); // [null,1.5,2.5,3.5]
print(JSON.stringify(Array.from(df.ROLLING_VAR("x", 2))));
print(JSON.stringify(Array.from(df.ROLLING_STD("x", 2))));
print(JSON.stringify(Array.from(df.EMA("x", 0.5))));
print(JSON.stringify(Array.from(df.PCT_CHANGE("x")))); // [null,1,0.5,0.33...]
print(JSON.stringify(Array.from(df.ZSCORE("x"))));
print(df.DELTA_SUM("x"), df.DELTA_SUM_TIMESTAMP("x", "x")); // 3 3
```

Showcase — time-series toolkit on one frame (masks compose with every verb):

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
  t: new Int32Array([0, 1, 2, 3, 4, 5]),
  v: new Float64Array([1, 2, 4, 7, 11, 16]), // triangular-ish
  g: ["a","a","b","b","a","b"]
});
// running + rolling in one pass
print(Array.from(df.CUM_SUM("v")).join(",")); // 1,3,7,14,25,41
print(Array.from(df.DIFF("v")).slice(1).join(",")); // 1,2,3,4,5
print(Array.from(df.ROLLING_MEAN("v", 3)).slice(2).join(",")); // 2.33...,4.33...,7.33...,11.33...
print(Array.from(df.EMA("v", 0.5)).map(x=>x.toFixed(1)).join(",")); // 1.0,1.5,2.8,4.9,7.9,12.0
// z-score + pct change — NaN heads are expected, not bugs
print(Array.from(df.ZSCORE("v")).map(x=>isNaN(x)?"na":x.toFixed(2)).join(","));
print(Array.from(df.PCT_CHANGE("v")).slice(1).join(","));
// counter semantics — only positive deltas survive
print(df.DELTA_SUM("v")); // 15
print(df.DELTA_SUM_TIMESTAMP("v","t")); // same, t already sorted — shuffle t and retry to see reorder
// masking composes: same window over subset
const m = df.GT("v", 3); // 000111
print(Array.from(df.CUM_SUM("v", m)).join(",")); // 0,0,0,7,18,34
print(Array.from(df.ROLLING_SUM("v", 2, m)).join(","));
// per-group windows
print(JSON.stringify(Array.from(df.GROUP_ARRAY_MOVING_SUM("g","v",2).values[0]))); // [1,3,13] (group a)
```

**`DataFrame.CUM_SUM(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the running sum. A masked-out row folds in the identity 0.

**`DataFrame.CUM_PROD(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the running product (identity 1).

**`DataFrame.CUM_MAX(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the running maximum (identity -Infinity).

**`DataFrame.CUM_MIN(col, mask?) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the running minimum (identity +Infinity).

**`DataFrame.SHIFT(col[, periods]) -> Float64Array`**

- `col` *(string)* — the column to shift.
- `periods` *(Number, optional)* — the shift distance; default 1, must be an integer.

Returns `out[i] = col[i-periods]`. The vacated head/tail is NaN, never 0.

**`DataFrame.DIFF(col[, periods]) -> Float64Array`**

- `col` *(string)* — the column to difference.
- `periods` *(Number, optional)* — the lag distance.

Returns `col[i] - col[i-periods]`. An out-of-range period leaves the whole answer NaN.

**`DataFrame.ROLLING_SUM(col, w[, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `w` *(Number)* — the window width; a positive integer (a window longer than the column simply never fills).
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns a trailing window sum of width `w`; the head is NaN where a window never fills. Never a subtractive running sum: O(n) block decomposition above `w` = 256, per-window re-sum below.

**`DataFrame.ROLLING_MEAN(col, w[, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `w` *(Number)* — the window width; a positive integer.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the trailing window mean. Divides by what CONTRIBUTED, never by `w`.

**`DataFrame.ROLLING_MIN(col, w[, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `w` *(Number)* — the window width; a positive integer.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the trailing window minimum. Monotonic-deque O(n); NaN never enters the deque.

**`DataFrame.ROLLING_MAX(col, w[, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `w` *(Number)* — the window width; a positive integer.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the trailing window maximum. Same deque.

**`DataFrame.ROLLING_VAR(col, w[, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `w` *(Number)* — the window width; a positive integer.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sample variance (ddof=1) per window, two passes, never a sum of squares around an uncentred mean. A window with fewer than two selected rows is NaN.

**`DataFrame.ROLLING_STD(col, w[, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `w` *(Number)* — the window width; a positive integer.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sqrt of the rolling variance.

**`DataFrame.EMA(col, alpha[, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `alpha` *(Number)* — the smoothing factor; must be in (0, 1].
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the exponential moving average: `acc = alpha * v + (1-alpha) * acc`, seeded on the first selected row. A masked-out row carries the running value forward.

**`DataFrame.PCT_CHANGE(col[, periods][, mask]) -> Float64Array`**

- `col` *(string)* — the column to scan.
- `periods` *(Number, optional)* — the lag distance; default 1.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns `(x[i] - x[i-p]) / x[i-p]`. A zero previous value gives +/-Inf (the honest answer); 0/0 stays NaN.

**`DataFrame.ZSCORE(col[, mask]) -> Float64Array`**

- `col` *(string)* — the column to standardise.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns `(x - mean) / sample stddev`, so it composes with STDDEV. A constant column is all NaN, not 0.

**`DataFrame.DELTA_SUM(col[, mask]) -> Number`**

- `col` *(string)* — the column to scan.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum of POSITIVE consecutive differences, the monotonic-increase total a counter accumulates across resets.

**`DataFrame.DELTA_SUM_TIMESTAMP(valueCol, timeCol[, mask]) -> Number`**

- `valueCol` *(string)* — the column of values.
- `timeCol` *(string)* — the column of timestamps.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns DELTA_SUM in TIMESTAMP order, not row order: rows are sorted by the time column first, so out-of-order input still sums the positive value-deltas along the timeline.

### DataFrame.statistics

Pairwise verbs return a Number; a string column in either position is refused in the argument's terms. The regression verbs take (y, x); the first argument is the dependent variable.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    x: new Float64Array([3, 1, 2, 4]),
    y: new Float64Array([30, 10, 20, 40])
});
print(df.COV_POP("x", "y"), df.COV_SAMP("x", "y"), df.CORR("x", "y"));
print(df.REGR_SLOPE("y", "x"), df.REGR_INTERCEPT("y", "x"), df.REGR_R2("y", "x"));
print(df.REGR_AVG_X("y", "x"), df.REGR_AVG_Y("y", "x"));
print(df.REGR_COUNT("x", "y"), df.REGR_SXX("y", "x"), df.REGR_SYY("y", "x"), df.REGR_SXY("y", "x"));
print(df.RANK_CORR("x", "y"));
const cm = df.CORR_MATRIX(["x", "y"]);
print(JSON.stringify(Array.from(cm.matrix)), cm.n);   // [1,0.4,0.4,1] 2
print(JSON.stringify(Array.from(df.COV_MATRIX(["x", "y"]).matrix)));
print(df.RATE("x", "y"), df.IRATE("x", "y"));         // 3.333... 10
print(df.BOUNDING_RATIO("x", "y"));
print(df.EXPONENTIAL_TIME_DECAYED_AVG("x", "y", 1));
print(df.EXPONENTIAL_TIME_DECAYED_SUM("x", "y", 1));
print(df.EXPONENTIAL_TIME_DECAYED_COUNT("x", "y", 1));
print(df.EXPONENTIAL_TIME_DECAYED_MAX("x", "y", 1));
const ra = df.RANGE_AGG("x", "y");
print(JSON.stringify(Array.from(ra.starts)), JSON.stringify(Array.from(ra.ends)));
print(JSON.stringify(df.RANGE_INTERSECT_AGG("x", "y")));
```

**`DataFrame.COV_POP(a, b, mask?) -> Number`**

- `a` *(string)* — the first column.
- `b` *(string)* — the second column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the population covariance (/n), defined from one row up.

**`DataFrame.COV_SAMP(a, b, mask?) -> Number`**

- `a` *(string)* — the first column.
- `b` *(string)* — the second column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sample covariance (n-1). NaN below two selected rows.

**`DataFrame.CORR(a, b, mask?) -> Number`**

- `a` *(string)* — the first column.
- `b` *(string)* — the second column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the Pearson correlation.

**`DataFrame.REGR_SLOPE(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the regression slope. NaN when `x` has zero variance.

**`DataFrame.REGR_INTERCEPT(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the regression intercept.

**`DataFrame.REGR_R2(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the squared correlation, clamped to 1.0. A constant `y` gives NaN (SQL's special case of 1 is not followed).

**`DataFrame.REGR_AVG_X(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the mean of the x column, under the (y, x) naming.

**`DataFrame.REGR_AVG_Y(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the mean of the y column.

**`DataFrame.REGR_COUNT(x, y, mask?) -> Number`**

- `x` *(string)* — the first column.
- `y` *(string)* — the second column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the number of selected rows.

**`DataFrame.REGR_SXX(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum of (x - mean x)^2. Note the argument order: SXX is the INDEPENDENT column's sum of squares.

**`DataFrame.REGR_SYY(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum of (y - mean y)^2.

**`DataFrame.REGR_SXY(y, x, mask?) -> Number`**

- `y` *(string)* — the dependent variable column.
- `x` *(string)* — the independent variable column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the sum of (y - mean y)(x - mean x).

**`DataFrame.RANK_CORR(x, y[, mask]) -> Number`**

- `x` *(string)* — the first column.
- `y` *(string)* — the second column.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns Spearman: Pearson over the AVERAGE ranks, the definition that stays correct with ties. A row missing in either column is out of BOTH rankings.

**`DataFrame.CORR_MATRIX(cols[], mask?) -> {columns, matrix, n}`**

- `cols` *(Array<string>)* — the columns to include.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the n*n row-major correlation matrix. Every cell goes through the same moments as CORR, so a cell cannot disagree with the pairwise call. The diagonal is pinned to 1.0 where variance exists. Duplicate column names are computed once.

**`DataFrame.COV_MATRIX(cols[], mask?) -> {columns, matrix, n}`**

- `cols` *(Array<string>)* — the columns to include.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the covariance matrix over the same moments as COV_SAMP.

**`DataFrame.RATE(valueCol, timeCol[, mask]) -> Number`**

- `valueCol` *(string)* — the column of values.
- `timeCol` *(string)* — the column of times.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns change per unit time across the WHOLE selection: (last - first) / (t last - t first). NaN below two selected rows.

**`DataFrame.IRATE(valueCol, timeCol[, mask]) -> Number`**

- `valueCol` *(string)* — the column of values.
- `timeCol` *(string)* — the column of times.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the most recent interval only: (last - previous) / (t last - t previous).

**`DataFrame.BOUNDING_RATIO(x, y[, mask]) -> Number`**

- `x` *(string)* — the column of x values.
- `y` *(string)* — the column of y values.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the slope of the line joining the LEFTMOST and RIGHTMOST points, chosen by x value (unlike RATE, which reads the first and last ROWS). NaN for one point or a vertical line.

**`DataFrame.EXPONENTIAL_TIME_DECAYED_AVG(value, time, tau[, mask]) -> Number | undefined`**

- `value` *(string)* — the column of values.
- `time` *(string)* — the column of times.
- `tau` *(Number)* — the decay time constant; must be positive.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the weighted mean. Each row weighs `exp(-(tMax - t) / tau)`, relative to the LATEST selected time so the exponent stays non-positive. `undefined` when nothing is selected.

**`DataFrame.EXPONENTIAL_TIME_DECAYED_SUM(value, time, tau[, mask]) -> Number | undefined`**

- `value` *(string)* — the column of values.
- `time` *(string)* — the column of times.
- `tau` *(Number)* — the decay time constant; must be positive.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the weighted sum.

**`DataFrame.EXPONENTIAL_TIME_DECAYED_COUNT(value, time, tau[, mask]) -> Number | undefined`**

- `value` *(string)* — the column of values.
- `time` *(string)* — the column of times.
- `tau` *(Number)* — the decay time constant; must be positive.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the decayed row count (the weight denominator; the value is ignored).

**`DataFrame.EXPONENTIAL_TIME_DECAYED_MAX(value, time, tau[, mask]) -> Number | undefined`**

- `value` *(string)* — the column of values.
- `time` *(string)* — the column of times.
- `tau` *(Number)* — the decay time constant; must be positive.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the largest weighted value.

**`DataFrame.RANGE_AGG(loCol, hiCol[, mask]) -> {starts, ends}`**

- `loCol` *(string)* — the column of range starts.
- `hiCol` *(string)* — the column of range ends.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns half-open [lo, hi) ranges merged into their union; [1,2) and [2,3) join into [1,3). An empty or inverted range covers nothing and is dropped.

**`DataFrame.RANGE_INTERSECT_AGG(loCol, hiCol[, mask]) -> {start, end} | undefined`**

- `loCol` *(string)* — the column of range starts.
- `hiCol` *(string)* — the column of range ends.
- `mask` *(Uint8Array, optional)* — a row mask of `ROWS` bytes; masked-out rows do not contribute.

Returns the interval common to ALL ranges, or `undefined` when they do not all overlap.

### DataFrame.reshape

Frames produced by these verbs carry fresh copies of the data; the original frames are never mutated.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ id: new Int32Array([1, 2, 3]), x: new Float64Array([1, 2, 3]) });
const other = new DataFrame({ id: new Int32Array([2, 3, 5]), v: new Float64Array([20, 30, 50]) });
print(df.JOIN(other, "id", "id").ROWS);          // 2 (inner)
print(df.JOIN(other, "id", "id", "left").ROWS);  // 3
print(df.CONCAT(df).ROWS);                       // 6
const t1 = new DataFrame({ t: new Int32Array([1, 3, 5]), v: new Int32Array([10, 30, 50]) });
const t2 = new DataFrame({ t: new Int32Array([2, 4]), w: new Int32Array([20, 40]) });
print(t1.ASOF_JOIN(t2, "t", "t").ROWS);         // 3
const r = new DataFrame({
    t: new Float64Array([0, 1.5, 3, 4.5]),
    v: new Float64Array([1, 2, 4, 8])
});
print(r.RESAMPLE("t", 3).ROWS, r.RESAMPLE("t", 3, "mean").ROWS); // 2 2
const pv = new DataFrame({
    id: new Int32Array([1, 1, 2]),
    k: ["a", "b", "a"],
    val: new Float64Array([10, 20, 30])
});
print(pv.PIVOT("id", "k", "val").COLUMNS.join(",")); // id,a,b
const mt = new DataFrame({
    id: new Int32Array([1, 2]),
    a: new Float64Array([10, 20]),
    b: new Float64Array([30, 40])
});
print(mt.MELT(["id"], ["a", "b"]).ROWS);         // 4
```

**`DataFrame.JOIN(other, leftKey, rightKey[, how]) -> DataFrame`**

- `other` *(DataFrame)* — the frame to join with.
- `leftKey` *(string)* — the left join column.
- `rightKey` *(string)* — the right join column.
- `how` *(String, optional)* — `"inner"` (default), `"left"`, `"right"` or `"outer"`.

Returns a hash join of the two frames. The keys must be INTEGER columns; float keys are refused because NaN could never match. The right side is indexed once and the left scanned once; duplicate right keys multiply rows and are refused beyond the row limit. A colliding right column name gets a `_right` suffix; a string column on a side a join can leave missing is refused, a numeric one is carried with NaN fill, and an outer join appends `matched`.

**`DataFrame.ASOF_JOIN(other, leftTime, rightTime) -> DataFrame`**

- `other` *(DataFrame)* — the frame to join with.
- `leftTime` *(string)* — the left time column.
- `rightTime` *(string)* — the right time column.

Returns each left row matched to the right row with the LARGEST rightTime <= leftTime. No match: NaN fill; a right-side string column is refused. Both time columns INTEGER and both frames SORTED ASCENDING; unsorted input is refused. O(n+m) two-pointer, not a per-row scan.

**`DataFrame.CONCAT(other) -> DataFrame`**

- `other` *(DataFrame)* — the frame to stack underneath.

Returns a frame with `other`'s rows stacked underneath. The column sets must match EXACTLY, by name in the same order; a string column beside a numeric one is refused. Mixed numeric types widen to f64; two same-type integer columns stay that type; two string columns merge dictionaries.

**`DataFrame.RESAMPLE(timeCol, interval[, agg]) -> DataFrame`**

- `timeCol` *(string)* — the numeric time column.
- `interval` *(Number)* — the bucket width.
- `agg` *(String, optional)* — `sum`/`mean`/`min`/`max`/`count` (default `sum`).

Returns a frame bucketing the time column into half-open `[t0 + k*interval, t0 + (k+1)*interval)`, `t0` being the minimum time aligned DOWN to a multiple of `interval`. The time column MUST be sorted ascending (refused otherwise). Only occupied buckets are emitted, as `bucket` (the bucket START) and `value`.

**`DataFrame.PIVOT(index, columns, values[, agg]) -> DataFrame`**

- `index` *(string)* — the column of distinct index values.
- `columns` *(string)* — the column of distinct pivot-column names.
- `values` *(string)* — the column of values.
- `agg` *(String, optional)* — `sum` (default), `mean`, `min`, `max`, `count`, `first` or `last`.

Returns one row per distinct `index` value, one column per distinct `columns` value. The width is DATA-DEPENDENT, bounded by the 1024-column cap and refused beyond, never truncated. An empty cell is NaN (count: 0). A pivot value colliding with the index column name is refused.

**`DataFrame.MELT(idVars[], valueVars[]) -> DataFrame`**

- `idVars` *(Array<string>)* — the columns kept as-is per output row.
- `valueVars` *(Array<string>)* — the columns melted into `variable`/`value` pairs; must be numeric.

Returns the long form: each (row, valueVar) pair becomes one output row of the id columns plus `variable` (the valueVar name) and `value`. Output rows = `ROWS * valueVars.length` (computed wide, refused past the row limit). A column in both lists is refused.

# dyna:ml

Native machine learning, in-repo: classifiers, regressors, clustering, decomposition, scalers, model selection and scoring metrics, all in C over contiguous doubles.

Every options bag in this module is strict: an unknown option key throws a `TypeError` naming the key and the valid set (`unknown option "nItr" (valid: scoring, seed, nIter, shuffle, k, folds)`), instead of being silently ignored. This covers the estimator constructor bags (`LogisticRegression`, the tree/booster family, `SVC`, `GaussianMixture`), every `fit()`'s trailing `{sampleWeight}` bag, and the model-selection bags (`trainTestSplit`, `kFold`, `stratifiedKFold`, `crossValScore`, `gridSearch`, `randomSearch`). The positional constructors (`LinearRegression`, `KMeans`, `PCA`, `GaussianNB`, `KNClassifier`/`KNRegressor`, `DBScan`, the scalers, `Pipeline`) take no options object.

`import { CSR, LinearRegression, LogisticRegression, KMeans, SVC, GaussianMixture, GaussianNB, DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier, RandomForestRegressor, GradientBoostingRegressor, GradientBoostingClassifier, XGBClassifier, XGBRegressor, PCA, KNClassifier, KNRegressor, DBScan, StandardScaler, MinMaxScaler, Pipeline, accuracy, precision, r2Score, rocAuc, confusionMatrix, trainTestSplit, kFold, stratifiedKFold, crossValScore, gridSearch, randomSearch, imputeMean, dropMissing } from "dyna:ml";`

### Inputs and shared rules

Every `fit` and `predict` takes a matrix `X` in one of two forms: an Array of rows (each row a plain Array or a Float64Array, all rows the same length), or a flat Float64Array plus the shape as trailing `(rows, cols)` arguments. `y` is an Array or Float64Array of the same length as `X` has rows. A flat Float64Array is read zero-copy (the model never keeps an alias past the call); an Array is copied in.

Outputs mirror the input: `predict` returns an Array of numbers, and `predictProba`/`transform` return an Array of rows (or a flat Float64Array when `X` arrived flat).

All inputs must be finite: every fit rejects NaN/infinity with a `RangeError` naming the exact cell (impute them first — see `imputeMean`/`dropMissing` below); the second-order `XGB*` models are the one exception, giving NaN the meaning "missing". `predict` follows each model's own semantics rather than one blanket rule: the tree/forest/boosting family (including `XGB*`) and the linear models ACCEPT a non-finite input silently — a tree routes a NaN the way it routes a missing value, `LinearRegression` answers NaN — while `KMeans`, `KN*`, `GaussianNB`, `SVC` and `GaussianMixture` reject it with a `RangeError`. The policy is per estimator and is identical whichever form `X` arrived in. The check runs on the matrix's *reading*, not its storage: a row whose duplicate column indices sum past the finite range (`[1e308, 1e308]` at one column reads `Infinity`) is refused exactly where a dense `Infinity` row is, and accepted where that is accepted. A CSR cannot carry a non-finite value in the first place — both `new CSR(...)` and `CSR.fromDense(...)` refuse one at construction (see below).

Numerical results are equivalent across platforms but not guaranteed bit-equal (a vectorised multi-accumulator reduction can move a value by an ULP); the integer labels a fit produces are exact. Within one build the two input forms are bit-identical to each other: a dense `X` and a CSR `X` of the same reading run one computation, and a `LinearRegression` sparse fit is bit-equal to the dense fit of the same matrix. Every seeded fit draws from the same deterministic SplitMix64 stream, so a given seed reproduces a given fit, split or search exactly.

Model objects are native resources. Each has `model.close()`, `model.dispose()`, `model[Symbol.dispose]` (all idempotent, deterministic free of native state) and a `model.closed` boolean; every method on a closed model throws. Fitted models add `serialize()`/`save(path)` on the instance and `deserialize(bytes)`/`load(path)` on the constructor — see *Persistence and lifecycle*.

### LinearRegression

**`new LinearRegression()`**

Closed-form ordinary least squares via the normal equations with Gaussian elimination and partial pivoting. The un-ridged system is solved first, so for any design whose elimination completes the coefficients are the exact OLS solution up to rounding of the normal equations — pinned at ≤ 1e-9 relative against `numpy.linalg.lstsq` on an exactly-solvable battery (integer designs with integer targets, feature scales 1 … 1e3; the residual is the normal equations' own conditioning, error ≈ eps·cond(AᵀA)). A design whose elimination is numerically rank-deficient falls back to a ridge-stabilized solve (`AᵀA + λI`, `λ = 1e-9` × mean sample weight, added after the system is divided by that mean weight): every exactly-singular design (duplicate, constant or collinear columns) and any design whose smallest elimination pivot falls below ~1000·eps (≈ 2.2e-13) of `AᵀA`'s largest diagonal entry is routed there, so the fit stays solvable instead of throwing; the coefficients are then ridge-biased and only identifiable quantities (predictions on consistent targets) are meaningful. A well-posed design is not ridged, however close its columns sit: above that pivot cut the coefficients are the exact OLS solution to rounding, and near-singular designs with null-heavy targets are pinned at ≤ 1e-12 relative. The accumulation of `AᵀA` runs on the raw products (sample weight × feature², sample weight × feature × target) before the weight normalization runs, so a single product beyond ~1.8e308 (1e300 weights on 1e6-scale features) reaches the solve as +Inf, where no load can repair it: the fit refuses with `InternalError` instead of returning a model — a stated finite-input limit. Solving costs O(rows·cols²) in the dense form; a CSR `X` accumulates only nonzero pairs of each row's summed reading — O(sum nnz_i²) over the row's distinct columns — and produces bit-identical coefficients to the dense fit of the same matrix, whatever order the row's entries were stored in (duplicate column indices sum in storage order, unsorted columns are sorted).

**`LinearRegression.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array | CSR)* — the training matrix, rows × features; a CSR takes the sparse path.
- `y` *(Array | Float64Array)* — the targets, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum; gives weighted least squares.

Returns the model. The ridge scales with the mean weight, so scaling all weights leaves the fit unchanged.

**`LinearRegression.predict(X [, rows, cols] [, {as: "f64"}]) -> number[] | Float64Array`**

- `X` *(Array | Float64Array | CSR)* — the matrix to predict. A CSR is
  expanded one row at a time into a cols-sized scratch (duplicate column
  indices summed at expansion) and fed to the same dot product the dense form
  runs — the matrix itself is never expanded.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `as` *(string)* — `"f64"` returns the predictions as a flat `Float64Array`
  instead of a `number[]`. The bag is strict.

Returns `intercept + coef·x` per row. The column count must match fit.

**`LinearRegression.predictInto(out, X [, rows, cols]) -> number`** —

- `out` *(Float64Array)* — the caller's buffer, written in place.

Writes one prediction per row of `X` and returns the number of rows written.
contract: nothing is allocated on the JS side (C-level scratch aside); a
buffer shorter than the row count throws `RangeError` BEFORE anything is
written; extra capacity is left untouched; `out` must be exactly a
`Float64Array` (a BigInt64Array shares the 8-byte width and is refused by
type). `out` MUST NOT ALIAS `X`: the walk writes earlier rows before it reads
later ones, so a shared buffer would corrupt the features still unread. `X`
may be a CSR, as with `predict`.

The same three forms — `predict(..., {as: "f64"})`, `predictInto`, and the
`LinearRegression`-style CSR acceptance — exist on every estimator with a
`predict`: `LogisticRegression` (whose `predictProba` also takes a CSR), the
tree/forest/boosting family, `XGBRegressor`/`XGBClassifier`, `SVC`,
`GaussianMixture`, `GaussianNB`, `KMeans`, `KNClassifier`, `KNRegressor`
(every `predictProba`, and SVC's `decisionFunction`, take a CSR too). A CSR
is expanded ONE ROW AT A TIME into a cols-sized scratch and fed to the same
kernel the dense form runs, so sparse and dense calls on the same data give
the same answers and the whole matrix is never expanded. Sparse input is
LIMITED to the prediction side plus the `LinearRegression`/
`LogisticRegression` fits: every other `fit`, `apply`, and the scaler/PCA
`transform`s, walk dense rows and refuse (`... cannot take a CSR ... pass
X.toDense()`) — densifying behind the caller's back is the memory the sparse
form exists to avoid.

**`LinearRegression.coef`** — Array of fitted weights (`[]` before fit).

**`LinearRegression.intercept`** — fitted intercept (`0` before fit).

A singular system throws `InternalError`.

```js
import { LinearRegression } from "dyna:ml";

const m = new LinearRegression();
m.fit([[1],[2],[3],[4]], [3,5,7,9]);
m.predict([[5]]);          // ~11
m.coef.length === 1 && Math.abs(m.intercept) < 1e-6;
m.close();
```

### LogisticRegression

**`new LogisticRegression({learningRate=0.1, maxIter=3000, tol=1e-4, l1=0, l2=0, C, penalty, classWeight})`**

Full-batch gradient descent on the mean cross-entropy with an L1/L2/elastic-net penalty applied as the proximal update, unconditionally stable for any penalty strength (L1 produces exact zeros).

- `learningRate` *(number, default 0.1)* — the step size.
- `maxIter` *(number, default 3000, capped at 100000)* — the iteration budget.
- `tol` *(number, default 1e-4)* — the gradient-norm convergence threshold.
- `l1`, `l2` *(number, default 0)* — the penalty strengths.
- `C` *(number)* — inverse penalty strength when `penalty` is given, as in scikit-learn (a `1/rows` factor is folded in to match sklearn's convention).
- `penalty` *(string)* — `"l1"`, `"l2"`, `"elasticnet"` or `"none"`.
- `classWeight` *(string)* — `"balanced"` weights each class by `rows/(nClasses·count)`.

Convergence is checked on the gradient infinity norm, so `tol` means the same thing at any learning rate; separable data has no finite optimum and honestly runs to `maxIter` with `converged === false`. Binary labels use one weight vector; more than two use one softmax vector per class (up to 256 classes).

**`LogisticRegression.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array | CSR)* — the training matrix; the CSR sparse path costs O(nnz) per iteration.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`LogisticRegression.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the class labels.

**`LogisticRegression.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns per-class probabilities in `classes` order.

**`LogisticRegression.classes`** — distinct labels, ascending.

**`LogisticRegression.coef`** — weight matrix (1 row binary, one per class multinomial).

**`LogisticRegression.intercept`** — scalar (binary) or Array (multinomial).

**`LogisticRegression.nIter`** — iterations run.

**`LogisticRegression.converged`** — whether the gradient-norm stop fired.

```js
import { LogisticRegression } from "dyna:ml";

const m = new LogisticRegression({maxIter: 200, penalty: "l2", C: 1});
m.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
m.predict([[0.1,0.1]]);            // [0]
m.predictProba([[5.5,5.5]])[0][1] > 0.5;
m.converged && m.nIter > 0;
m.close();
```

### KMeans

**`new KMeans(nClusters=8, seed=-1)`**

Lloyd's algorithm with k-means++ seeding, at most 300 iterations. `seed < 0` (or omitted) uses a fixed deterministic value; `seed >= 0` makes the run reproducible. `fit` needs at least `nClusters` rows and refuses `nClusters` above 64 when it exceeds half the rows (the O(rows²·cols) adversarial regime). Empty clusters reseed from a deterministic pseudo-random point.

**`KMeans.fit(X [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; affects the centroid means and the inertia, not the assignment.

Returns the model.

**`KMeans.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns nearest-centroid cluster indices. Predictions are deterministic for a fixed seed; a point exactly equidistant from two centroids may land in either across platforms.

**`KMeans.inertia`** — summed squared distance to assigned centroids of the last fit (`0` before fit).

```js
import { KMeans } from "dyna:ml";

const km = new KMeans(2, 1);
km.fit([[0,0],[1,0],[0,1],[1,1],[10,10],[11,10],[10,11],[11,11]]);
km.predict([[0,0.2]]);             // [0]
km.predict([[10.5,10]]);           // [1]
km.inertia >= 0;
km.close();
```

### SVC

**`new SVC({kernel="rbf", C=1, gamma, coef0=0, degree=3, tol=1e-3, maxIter=1000})`**

Support-vector classifier trained by Sequential Minimal Optimization (pairwise closed-form updates with an incremental decision cache).

- `kernel` *(string)* — `"linear"`, `"rbf"` or `"poly"`.
- `C` *(number, default 1)* — the margin slack penalty.
- `gamma` *(number)* — defaults to `1/cols`, resolved at fit.
- `coef0` *(number, default 0)* — the polynomial kernel constant.
- `degree` *(number, default 3, capped at 1000)* — the polynomial degree.
- `tol` *(number, default 1e-3)* — the convergence tolerance.
- `maxIter` *(number, default 1000, capped at 100000)* — the iteration budget.

Two classes train one machine, more use one-vs-rest (up to 256 classes). The kernel matrix is recomputed per step rather than cached (O(n²) memory is against the module contract). No weighted fit — passing `sampleWeight` throws.

**`SVC.fit(X, y [, rows, cols]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns the model. Needs at least two distinct labels.

**`SVC.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the labels; binary predicts `classes[1]` when the decision value is >= 0.

**`SVC.decisionFunction(X) -> number[] | number[][]`**

- `X` *(Array | Float64Array)* — the matrix to score.

Returns the raw margin per row (binary) or per-class one-vs-rest values (multi-class). A polynomial kernel that overflows to inf/NaN throws rather than predicting garbage.

**`SVC.nSupportVectors`** — total support vectors across all binary machines (`0` before fit).

**`SVC.classes`** — labels in ascending order.

```js
import { SVC } from "dyna:ml";

const svc = new SVC({kernel: "linear", C: 1});
svc.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
svc.predict([[0.1,0.1]]);          // [0]
svc.decisionFunction([[6,5]])[0] > 0;
svc.nSupportVectors >= 1;
svc.close();
```

### GaussianMixture

**`new GaussianMixture(k=3, {seed=12345, maxIter=200, tol=1e-3, regCovar=1e-6})`**

Soft clustering by Expectation-Maximisation over `k` Gaussians with diagonal covariances.

- `k` *(number, default 3)* — the number of components.
- `seed` *(number, default 12345)* — the deterministic seed.
- `maxIter` *(number, default 200, capped at 10000)* — the iteration budget.
- `tol` *(number, default 1e-3)* — convergence on the relative total log-likelihood change.
- `regCovar` *(number, default 1e-6)* — added to every variance so a collapsed component cannot produce an infinite density.

Initialised by k-means++ plus one hard assignment, so results are reproducible from `seed`. A component with no mass keeps weight `1e-300`. No weighted fit — `sampleWeight` throws.

**`GaussianMixture.fit(X [, rows, cols]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns the model. Needs at least `k` rows.

**`GaussianMixture.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the most-likely component index per row.

**`GaussianMixture.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the responsibilities per component.

**`GaussianMixture.weights`** — mixing weights (sum 1).

**`GaussianMixture.means`** — k×cols.

**`GaussianMixture.variances`** — k×cols, regularised.

**`GaussianMixture.logLikelihood`** — total log-likelihood of the last fit (`0` before fit).

**`GaussianMixture.nIter`** — EM iterations run.

```js
import { GaussianMixture } from "dyna:ml";

const gmm = new GaussianMixture(2, {seed: 3});
gmm.fit([[0,0],[1,0],[0,1],[1,1],[10,10],[11,10],[10,11],[11,11]]);
gmm.predict([[10.5,10.5]]);        // [1]
gmm.predictProba([[0.2,0.2]])[0][0] > 0.9;
gmm.weights.length === 2 && gmm.logLikelihood < 0;
gmm.close();
```

### GaussianNB

**`new GaussianNB(varSmoothing=1e-9)`**

Naive Bayes with per-class, per-feature Gaussian densities. Everything is computed in log space (a direct density product underflows), with log-sum-exp used to normalise probabilities. Variances get `varSmoothing · largestVariance` added, scikit-learn's floor so a constant feature never divides by zero.

**`GaussianNB.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`GaussianNB.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the predicted labels.

**`GaussianNB.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns per-class probabilities in ascending-label column order.

**`GaussianNB.classes`** — labels seen at fit.

```js
import { GaussianNB } from "dyna:ml";

const nb = new GaussianNB();
nb.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
nb.predict([[0,0]]);               // [0]
nb.classes;                        // [0,1]
nb.close();
```

### DecisionTreeClassifier

**`new DecisionTreeClassifier({nEstimators, maxDepth=0, minSamplesSplit=2, minSamplesLeaf=1, maxFeatures=0, maxBins=0, seed=12345})`**

CART over Gini impurity.

- `nEstimators` *(number)* — accepted for API symmetry; a single tree stays one tree.
- `maxDepth` *(number, default 0)* — `0` means unlimited, capped at 1024.
- `minSamplesSplit` *(number, default 2)* — the minimum samples to split a node.
- `minSamplesLeaf` *(number, default 1)* — the minimum samples per leaf.
- `maxFeatures` *(number, default 0)* — the candidate features per node.
- `maxBins` *(number, default 0)* — `0` selects the exact split finder (incremental statistics over a sorted sweep, O(rows·cols·log rows)); a bin count in [2, 255] selects the histogram splitter.
- `seed` *(number, default 12345)* — the deterministic seed.

Split thresholds are midpoints between bracketing values, so a test value equal to a training value is never on the boundary.

**`DecisionTreeClassifier.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`DecisionTreeClassifier.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the predicted labels.

**`DecisionTreeClassifier.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the leaf's class distribution, so a single tree reports real probability mass rather than 0/1.

**`DecisionTreeClassifier.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns the leaf index per tree (rows × nTrees), the tree's own encoding of each row.

**`DecisionTreeClassifier.featureImportances`** — Gini-importance per feature, normalised to sum 1 (zeros when nothing ever split).

**`DecisionTreeClassifier.depth`** — deepest tree (0 before fit).

```js
import { DecisionTreeClassifier } from "dyna:ml";

const dt = new DecisionTreeClassifier({maxDepth: 3});
dt.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
dt.predict([[5.5,5]]);             // [1]
dt.predictProba([[0,0]])[0][0] >= 0.5;
dt.featureImportances.length === 2;
dt.close();
```

### DecisionTreeRegressor

**`new DecisionTreeRegressor({...})`**

The same tree implementation minimising variance (MSE). Methods and options as the classifier: `fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`, `predict(X) -> number[]`, `apply(X) -> number[][]`, `featureImportances`, `depth`. `predictProba` on a regressor throws.

**`DecisionTreeRegressor.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the targets, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`DecisionTreeRegressor.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the predicted values.

**`DecisionTreeRegressor.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns the leaf index per tree (rows × nTrees).

**`DecisionTreeRegressor.featureImportances`** — Gini-importance per feature.

**`DecisionTreeRegressor.depth`** — deepest tree (0 before fit).

```js
import { DecisionTreeRegressor } from "dyna:ml";

const dtr = new DecisionTreeRegressor({maxDepth: 2});
dtr.fit([[1],[2],[3],[4]], [3,5,7,9]);
dtr.predict([[2.5]]);              // between 3 and 9
dtr.close();
```

### RandomForestClassifier

**`new RandomForestClassifier({nEstimators=100, maxDepth=0, minSamplesSplit=2, minSamplesLeaf=1, maxFeatures=0, maxBins=0, seed=12345})`**

Bagged decision trees: each tree bootstraps `rows` indices with replacement and draws its candidate features per node without replacement from the same seed, so a fixed `seed` reproduces a forest exactly. `nEstimators` is capped at 100000. `fit` trains synchronously on the calling thread — the legal `nEstimators` caps mean minutes of single-threaded compute for a large forest or gradient-boosted ensemble — so offload long fits to a worker.

- `nEstimators` *(number, default 100, capped at 100000)* — the number of trees.
- `maxDepth` *(number, default 0)* — `0` means unlimited, capped at 1024.
- `minSamplesSplit` *(number, default 2)* — the minimum samples to split a node.
- `minSamplesLeaf` *(number, default 1)* — the minimum samples per leaf.
- `maxFeatures` *(number, default 0)* — the candidate features per node.
- `maxBins` *(number, default 0)* — `0` selects the exact split finder; a bin count in [2, 255] selects the histogram splitter.
- `seed` *(number, default 12345)* — the deterministic seed.

**`RandomForestClassifier.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`RandomForestClassifier.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the argmax of the averaged leaf distributions (identical to the argmax of `predictProba`, never a hard-label majority vote, so the two APIs cannot disagree).

**`RandomForestClassifier.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the mean leaf distribution across trees.

**`RandomForestClassifier.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns rows × nTrees leaf indices.

**`RandomForestClassifier.featureImportances`** — Gini-importance per feature.

**`RandomForestClassifier.depth`** — deepest tree.

`RandomForestRegressor` is the same class over mean-squared-error leaves; its `predict` averages the tree predictions and its `predictProba` throws.

```js
import { RandomForestClassifier } from "dyna:ml";

const rf = new RandomForestClassifier({nEstimators: 10, seed: 7});
rf.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
rf.predict([[6,6]]);               // [1]
rf.predictProba([[0,0]])[0].length === 2;
rf.apply([[0,0]]).length === 10;   // one leaf index per tree
rf.close();
```

### RandomForestRegressor

**`new RandomForestRegressor({nEstimators=100, maxDepth=0, minSamplesSplit=2, minSamplesLeaf=1, maxFeatures=0, maxBins=0, seed=12345})`**

Bagged trees for regression; `predict` averages the trees' leaf values.

**`RandomForestRegressor.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the targets, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`RandomForestRegressor.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the mean of the trees' leaf values.

**`RandomForestRegressor.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns rows × nTrees leaf indices.

**`RandomForestRegressor.featureImportances`** — Gini-importance per feature.

**`RandomForestRegressor.depth`** — deepest tree.

`predictProba` throws (it predicts a number).

```js
import { RandomForestRegressor } from "dyna:ml";

const rfr = new RandomForestRegressor({nEstimators: 10, seed: 7});
rfr.fit([[1],[2],[3],[4]], [3,5,7,9]);
rfr.predict([[3]]).length === 1;
rfr.close();
```

### GradientBoostingRegressor

**`new GradientBoostingRegressor({nEstimators=100, maxDepth=3, learningRate=0.1, subsample=1, minSamplesSplit=2, minSamplesLeaf=1, maxFeatures=0, maxBins=0, seed=12345})`**

First-order boosting: each round fits a least-squares tree to the negative gradient, shrunk by `learningRate` (in [1e-12, 1e12]); classifier variants repair leaf values with a one-step Newton line search against the true loss.

- `nEstimators` *(number, default 100)* — the number of boosting rounds.
- `maxDepth` *(number, default 3)* — the tree depth.
- `learningRate` *(number, default 0.1, in [1e-12, 1e12])* — the shrinkage applied to each round.
- `subsample` *(number, default 1)* — in (0, 1], draws a row fraction per round.
- `minSamplesSplit` *(number, default 2)*, `minSamplesLeaf` *(number, default 1)*, `maxFeatures` *(number, default 0)*, `maxBins` *(number, default 0)*, `seed` *(number, default 12345)* — the shared tree options.

`earlyStoppingRounds`/`validationFraction` throw here — they belong to the XGB models only.

**`GradientBoostingRegressor.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the targets, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`GradientBoostingRegressor.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the base constant plus the shrunk tree sum.

**`GradientBoostingRegressor.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns rows × nTrees leaf indices.

**`GradientBoostingRegressor.featureImportances`** — Gini-importance per feature.

**`GradientBoostingRegressor.depth`** — deepest tree.

```js
import { GradientBoostingRegressor } from "dyna:ml";

const gb = new GradientBoostingRegressor({nEstimators: 10, learningRate: 0.2, seed: 9});
gb.fit([[1],[2],[3],[4]], [3,5,7,9]);
gb.predict([[2.5]]);               // ~6
gb.close();
```

### GradientBoostingClassifier

**`new GradientBoostingClassifier({nEstimators=100, maxDepth=3, learningRate=0.1, subsample=1, ...})`**

The classifier twin: binomial logistic for two classes, softmax for more, trees fit on the gradient with Friedman's one-step Newton leaf values.

**`GradientBoostingClassifier.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`GradientBoostingClassifier.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the argmax of the raw scores (monotone in the softmax, so it always agrees with `predictProba`).

**`GradientBoostingClassifier.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the link function of the raw score — sigmoid for binary, shifted softmax for multi-class.

**`GradientBoostingClassifier.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns rows × nTrees leaf indices.

**`GradientBoostingClassifier.featureImportances`** — Gini-importance per feature.

**`GradientBoostingClassifier.depth`** — deepest tree.

```js
import { GradientBoostingClassifier } from "dyna:ml";

const gbc = new GradientBoostingClassifier({nEstimators: 10, seed: 11});
gbc.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
gbc.predictProba([[5.5,5]])[0][1] > 0.5;
gbc.close();
```

### XGBRegressor

**`new XGBRegressor({nEstimators=100, maxDepth=6, learningRate=0.3, subsample=1, colsampleByTree=1, lambda=1, alpha=0, gamma=0, minChildWeight=1, validationFraction=0.1, earlyStoppingRounds=0, maxBins, seed=12345})`**

Second-order (Newton) boosting: the split criterion itself uses the curvature, `G²/(H+λ)` with L1 soft-thresholding and a `minChildWeight` floor on the hessian.

- `nEstimators` *(number, default 100)* — the number of boosting rounds.
- `maxDepth` *(number, default 6)* — the tree depth.
- `learningRate` *(number, default 0.3)* — the shrinkage.
- `subsample` *(number, default 1)* — the row fraction per round.
- `colsampleByTree` *(number, default 1)* — the column fraction per tree.
- `lambda` *(number, default 1)*, `alpha` *(number, default 0)* — the L2 and L1 regularisation.
- `gamma` *(number, default 0)* — the split-gain threshold.
- `minChildWeight` *(number, default 1)* — the floor on the hessian.
- `validationFraction` *(number, default 0.1)* — in [0, 0.5]; the held-out row fraction when early stopping is active.
- `earlyStoppingRounds` *(number, default 0)* — when > 0, holds out `validationFraction` of rows and keeps the round with the lowest validation loss.
- `maxBins` *(number)* — the split finder is the histogram one and is mandatory; defaults to the histogram maximum, `0` is refused.
- `seed` *(number, default 12345)* — the deterministic seed.

A NaN in `X` is "missing" and is given a learned direction (`fit` accepts it; the predict walker sends a missing row left or right per the trained flag).

**`XGBRegressor.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the targets, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`XGBRegressor.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the predicted values.

**`XGBRegressor.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns rows × nTrees leaf indices.

**`XGBRegressor.featureImportances`** — Gini-importance per feature.

**`XGBRegressor.depth`** — deepest tree.

**`XGBRegressor.bestRounds`** — the rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit).

```js
import { XGBRegressor } from "dyna:ml";

const xgb = new XGBRegressor({nEstimators: 10, maxDepth: 3, seed: 5});
xgb.fit([[1],[2],[3],[4]], [3,5,7,9]);
xgb.predict([[2.5]])[0] > 0;
xgb.bestRounds === 10;
xgb.close();
```

### XGBClassifier

**`new XGBClassifier({...})`**

The classifier twin of `XGBRegressor` with the same Newton options, histogram splitter, missing-value handling and early stopping.

**`XGBClassifier.fit(X, y [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; finite, non-negative, positive sum.

Returns the model.

**`XGBClassifier.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the argmax of the raw scores.

**`XGBClassifier.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the sigmoid / shifted softmax of the raw scores.

**`XGBClassifier.apply(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to route.

Returns rows × nTrees leaf indices.

**`XGBClassifier.featureImportances`** — Gini-importance per feature.

**`XGBClassifier.depth`** — deepest tree.

**`XGBClassifier.bestRounds`** — the rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit).

```js
import { XGBClassifier } from "dyna:ml";

const xgb = new XGBClassifier({nEstimators: 10, maxDepth: 3, seed: 5});
xgb.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
xgb.predict([[2.5,2.5]]);          // [0]
xgb.close();
```

### PCA

**`new PCA(nComponents=0, whiten=false)`**

Principal components by cyclic Jacobi diagonalisation of the sample covariance (ddof=1), unconditionally convergent and orthogonal to machine precision.

- `nComponents` *(number, default 0)* — `0` means all features; a value above the feature count throws.
- `whiten` *(boolean, default false)* — scales components by `1/sqrt(eigenvalue)`.

Each component is normalised with its largest-magnitude entry positive (scikit-learn's `svd_flip` rule), so output is reproducible and diffable. Needs at least two rows; a covariance overflow throws (scale X first). No weighted fit — `sampleWeight` throws.

**`PCA.fit(X [, rows, cols]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns the model.

**`PCA.transform(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to project.

Returns centred rows projected onto the components.

**`PCA.fitTransform(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to project.

Returns fit then transform in one call.

**`PCA.inverseTransform(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the projected matrix.

Returns the projection back into feature space (used with `nComponents` < features to reconstruct).

**`PCA.components`** — nComponents × features (unit rows).

**`PCA.mean`** — per-feature training mean.

**`PCA.explainedVariance`** — the retained eigenvalues.

**`PCA.explainedVarianceRatio`** — each divided by the total variance (sums to ~1).

```js
import { PCA } from "dyna:ml";

const pca = new PCA(1, true);
pca.fit([[1,2],[2,3],[3,4],[4,5],[5,6]]);
pca.transform([[2,3]]).length === 1;
pca.explainedVarianceRatio[0] >= 0.9;
pca.close();
```

### KNClassifier

**`new KNClassifier(k=5, weights="uniform")`**

K-nearest-neighbours classifier. Lazy: `fit` copies the training set (an owned copy even for a flat Float64Array, so later detaching the buffer cannot corrupt it) and `predict` scans it. Distances stay squared throughout (the k-nearest set is the same); `weights: "distance"` votes by `1/distance` (an exact hit returns that neighbour's label outright). Ties go to the smaller label, so results are deterministic. No weighted fit — `sampleWeight` throws.

**`KNClassifier.fit(X, y [, rows, cols]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns the model. Requires at least `k` rows.

**`KNClassifier.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the predicted labels.

```js
import { KNClassifier } from "dyna:ml";

const knn = new KNClassifier(3, "uniform");
knn.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);
knn.predict([[5.5,5]]);            // [1]
knn.close();
```

### KNRegressor

**`new KNRegressor(k=5, weights="uniform")`**

The same lazy learner predicting the (optionally distance-weighted) mean of the k nearest targets.

**`KNRegressor.fit(X, y [, rows, cols]) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the targets, one per row.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns the model. Requires at least `k` rows.

**`KNRegressor.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns the (optionally distance-weighted) mean of the k nearest targets.

```js
import { KNRegressor } from "dyna:ml";

const knr = new KNRegressor(2, "distance");
knr.fit([[1],[2],[3],[4]], [3,5,7,9]);
knr.predict([[2.5]]);              // ~6
knr.close();
```

### DBScan

**`new DBScan(eps=0.5, minPts=5)`**

Density-based clustering: points with at least `minPts` neighbours within `eps` (counting themselves) are core points; cores within `eps` form transitively one cluster; non-core points reachable from a core are border points; everything else is noise, labelled `-1`. `eps` must be positive and its square finite; `minPts >= 1`. Region queries use a grid index over `eps` when `cols <= 6` and rows are at least `4^cols`, falling back to a linear scan on wide data; memory stays O(rows) — neighbour lists are recomputed into one scratch buffer per query rather than materialised. A border point reachable from two clusters keeps its first assignment.

**`DBScan.fit(X [, rows, cols]) -> this`**

- `X` *(Array | Float64Array)* — the data matrix, rows × features.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns the model. No weighted form; `sampleWeight` throws.

**`DBScan.labels`** — Array of per-row cluster ids, `-1` for noise (`[]` before fit).

**`DBScan.nClusters`** — cluster count (0 before fit).

**`DBScan.eps`** — the eps the model was constructed with.

```js
import { DBScan } from "dyna:ml";

const db = new DBScan(1.0, 2);
db.fit([[0,0],[1,0],[0,1],[1,1],[10,10]]);
db.labels;                         // [0,0,0,0,-1]
db.nClusters === 1;
db.close();
```

### StandardScaler

**`new StandardScaler()`**

Per-column z-score: `(x - mean) / std`, population std (ddof=0), computed in two passes over deviations (the E[x²]−mean² identity cancels on large-mean columns). A constant column reports `std === 1.0` and scales by 1, matching scikit-learn's `scale_` convention, so no column becomes NaN or Inf.

**`StandardScaler.fit(X [, rows, cols] [, {sampleWeight}]) -> this`**

- `X` *(Array | Float64Array)* — the data matrix, rows × features.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.
- `sampleWeight` *(Array | Float64Array)* — optional; weights the mean and variance.

Returns the model.

**`StandardScaler.transform(X [, rows, cols]) -> number[][]`**

**`StandardScaler.transformInto(out, X [, rows, cols]) -> number`** —

Writes the transformed rows into `out` (rows × features doubles, row-major)
and returns the number of rows. Same allocation contract as `predictInto`:
nothing allocated on the JS side, `RangeError` on a short `out` before any
write, extra capacity untouched, Float64Array-only, and `out` must not alias
`X`. Forward transform only
(`inverseTransform` keeps its shape-in shape-out result). `MinMaxScaler`
and `PCA.transformInto` share the contract.

- `X` *(Array | Float64Array)* — the matrix to transform.

Returns the z-scores. Output shape mirrors the input shape (Array of rows, or flat Float64Array if X arrived flat).

**`StandardScaler.fitTransform(X [, rows, cols]) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to transform.

Returns fit then transform. Output shape mirrors the input shape.

**`StandardScaler.inverseTransform(X [, rows, cols]) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to transform.

Returns `x·std + mean`. Output shape mirrors the input shape.

**`StandardScaler.mean`** — per-feature means.

**`StandardScaler.std`** — per-feature standard deviations (`1.0` for constant columns).

Both `[]` before fit.

```js
import { StandardScaler } from "dyna:ml";

const ss = new StandardScaler();
ss.fit([[1,2],[2,3],[3,4],[4,5]]);
ss.transform([[2,3]]);             // ~[-0.45,-0.45]
ss.inverseTransform(ss.transform([[2,3]]))[0][0] === 2;
ss.close();
```

### MinMaxScaler

**`new MinMaxScaler()`**

Per-column min-max scaling to [0, 1]: `(x - dataMin) / (dataMax - dataMin)`. A zero-range column scales by 1.

**`MinMaxScaler.fit(X [, rows, cols]) -> this`**

- `X` *(Array | Float64Array)* — the data matrix, rows × features.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns the model. Refuses `sampleWeight` (min and max are order statistics no positive weight can change).

**`MinMaxScaler.transform(X [, rows, cols]) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to transform.

Returns the scaled values in [0, 1]. Output shape mirrors the input shape.

**`MinMaxScaler.fitTransform(X [, rows, cols]) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to transform.

Returns fit then transform. Output shape mirrors the input shape.

**`MinMaxScaler.inverseTransform(X [, rows, cols]) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to transform.

Returns `x·(max−min) + min`. Output shape mirrors the input shape.

**`MinMaxScaler.dataMin`** — per-column minima.

**`MinMaxScaler.dataMax`** — per-column maxima.

Both `[]` before fit.

```js
import { MinMaxScaler } from "dyna:ml";

const ms = new MinMaxScaler();
ms.fit([[1,2],[2,3],[3,4],[4,5]]);
ms.transform([[1,5]]);             // [[0,1]]
ms.dataMax;                        // [4,5]
ms.close();
```

### CSR

**`new CSR(values, columns, rowPointers, cols)`**

An immutable compressed-sparse-row matrix handle over doubles, the sparse input every `predict`/`predictProba`/`predictInto` takes (and the `LinearRegression`/`LogisticRegression` fits).

- `values` *(Array | Float64Array)* — one entry per nonzero.
- `columns` *(Array | Int32Array)* — one column index per value.
- `rowPointers` *(Array | Int32Array)* — rows+1 non-decreasing boundaries starting at 0 and ending at the value count.
- `cols` *(number)* — the matrix width.

Every field is validated because a malformed index is an out-of-bounds write, not a wrong answer. Fits beyond the two linear models, `apply`, and the scaler/PCA `transform`s refuse a CSR and name `.toDense()` — expanding it silently is the memory the sparse form exists to avoid. Duplicate column indices within a row SUM (scipy's rule), in `row()`, `toDense()`, every sparse predict and both sparse fits alike, in storage order; column order within a row is free. A row whose duplicate sums overflow the finite range (`[1e308, 1e308]` at one column reads `Infinity`) is refused exactly where its dense reading is refused.

**`CSR.fromDense(X [, rows, cols]) -> CSR`**

- `X` *(Array | Float64Array)* — the dense matrix, rows × features.

Returns a CSR dropping exact zeros from a dense matrix. Like the constructor, it refuses a non-finite value with a `RangeError` naming the cell and its class — a CSR never carries one, whichever way it was built. (The only way a finite-valued CSR reads non-finite is a duplicate-sum overflow; every consumer then treats it exactly as its dense reading.)

**`CSR.toDense() -> number[][]`**

Returns the dense form. Duplicate column indices in a row sum. Throws `RangeError` if it does not fit in memory: a dense expansion is budgeted (the measured cost of materialising the JS Arrays), and an admitted size always completes while anything past the budget refuses with this throw rather than taking the process down.

**`CSR.row(i) -> number[]`**

- `i` *(number)* — the row index, in `[0, rows)`.

Returns row `i` as a dense Array. Duplicate column indices in the row sum. Throws `RangeError` under the same memory budget as `toDense()` — the dense row must be admitted by it to be built.

**`CSR.rows`** — row count.

**`CSR.cols`** — column count.

**`CSR.nnz`** — nonzero count.

**`CSR.density`** — `nnz / (rows·cols)`.

```js
import { CSR, LinearRegression } from "dyna:ml";

const s = CSR.fromDense([[1,0,2],[0,3,0],[4,0,0]]);
s.nnz === 4 && s.density === 4 / 9;
const m = new LinearRegression();
m.fit(s, [3,6,12]);                // sparse path
m.predict(s);                      // every predict takes the sparse form
s.close();
```

### Pipeline

**`new Pipeline(stages[])`**

Composes feature stages and a final estimator into one `fit`/`predict`.

- `stages` *(Array)* — an array of model objects (1 to 64); every stage but the last must have a `transform` method, checked at construction so the error names the stage while the caller is looking at the constructor.

`fit` fits and transforms each intermediate stage in sequence (fit then transform, never `fitTransform` on the whole input) and forwards `(X, y)` to the final estimator — so a scaler fitted inside a Pipeline learns only from what reaches it, and passing the Pipeline to `crossValScore` cannot leak test-fold statistics into the training mean. `fit(X, y)` forwards only `(X, y)`; a `sampleWeight` option is refused (set it on the estimator). A Pipeline owns its stages, so `close()` releases them; a stage must not be shared between two Pipelines. The stages are driven through their public JS methods, so anything with the right shape composes, including estimators this module does not know about.

**`Pipeline.fit(X, y) -> this`**

- `X` *(Array | Float64Array)* — the training matrix, rows × features.
- `y` *(Array | Float64Array)* — the labels, one per row.

Returns the model.

**`Pipeline.predict(X) -> number[]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns push X through the stages, ask the last one.

**`Pipeline.predictProba(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to predict.

Returns push X through the stages, ask the last one.

**`Pipeline.transform(X) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to transform.

Returns push through every stage that can transform, stopping before a final bare estimator.

**`Pipeline.stage(i) -> object`**

- `i` *(number)* — the stage index; negative counts from the end.

Returns the i-th stage.

**`Pipeline.length`** — stage count.

**`Pipeline.fitted`** — boolean.

**`Pipeline.estimator`** — the final stage.

```js
import { Pipeline, StandardScaler, LogisticRegression } from "dyna:ml";

const p = new Pipeline([new StandardScaler(), new LogisticRegression({maxIter: 200})]);
p.fit([[1,2],[2,3],[3,4],[4,5],[5,6]], [0,0,1,1,1]);
p.predict([[2,3]]);                // [0]
p.predictProba([[4,5]])[0][1] > 0.5;
p.estimator instanceof LogisticRegression && p.fitted;
p.close();
```

### Metrics

All metric functions take two equal-length vectors (Array or Float64Array) and return a Number; inputs must be finite. Binary metrics take an optional third argument selecting the label counted as positive (`positive`, default `1`), and follow scikit-learn's zero-denominator convention: a precision with no predicted positives is 0, not NaN.

**`meanSquaredError(yTrue, yPred) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.

Returns the mean of squared errors.

**`meanAbsoluteError(yTrue, yPred) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.

Returns the mean absolute error.

**`r2Score(yTrue, yPred) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.

Returns the coefficient of determination; a constant `yTrue` scores 1.0 for an exact prediction else 0.0, never NaN.

**`accuracy(yTrue, yPred) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.

Returns the fraction of equal pairs.

**`logLoss(yTrue, yPred) -> number`**

- `yTrue` *(Array | Float64Array)* — the true labels.
- `yPred` *(Array | Float64Array | Array of Arrays)* — a matrix (rows × classes in ascending label order, exactly what `predictProba` returns) or a binary probability vector.

Returns the mean negative log-likelihood, probabilities clipped to `[1e-15, 1-1e-15]`. A matrix whose columns do not match the labels in `yTrue` throws.

**`confusionMatrix(yTrue, yPred) -> number[][]`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.

Returns the confusion matrix indexed `[true][pred]`; labels must be non-negative integers up to 4095.

**`precision(yTrue, yPred [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the precision.

**`recall(yTrue, yPred [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the recall.

**`f1(yTrue, yPred [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the F1 score.

**`specificity(yTrue, yPred [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the specificity.

**`balancedAccuracy(yTrue, yPred [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `positive` *(number, default 1)* — the label counted as positive.

Returns `(recall + specificity)/2`.

**`matthewsCorrcoef(yTrue, yPred [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the Matthews correlation coefficient, the one number that stays honest on imbalanced data.

**`cohenKappa(yTrue, yPred [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `positive` *(number, default 1)* — the label counted as positive.

Returns agreement over chance.

**`fbeta(yTrue, yPred, beta [, positive]) -> number`**

- `yTrue`, `yPred` *(Array | Float64Array)* — equal-length vectors.
- `beta` *(number)* — must be positive; `beta > 1` weights recall.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the F-beta score.

**`rocAuc(yTrue, yScore [, positive]) -> number`**

- `yTrue` *(Array | Float64Array)* — the true labels.
- `yScore` *(Array | Float64Array)* — the raw scores.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the exact Mann-Whitney U (rank-sum with tie-averaged ranks, O(n log n)). Needs both a positive and a negative sample.

**`averagePrecision(yTrue, yScore [, positive]) -> number`**

- `yTrue` *(Array | Float64Array)* — the true labels.
- `yScore` *(Array | Float64Array)* — the raw scores, sorted descending.
- `positive` *(number, default 1)* — the label counted as positive.

Returns the step-function AP (not the optimistically biased interpolated trapezoid).

```js
import { accuracy, precision, r2Score, rocAuc, confusionMatrix } from "dyna:ml";

accuracy([0,1,1],[0,1,0]) === 1/3;
precision([1,1,1,0],[1,1,0,1]) === 2/3;
r2Score([1,2,3],[1,2,4]);          // 0.5
rocAuc([0,0,1,1],[0.1,0.4,0.35,0.8]);   // 0.75
confusionMatrix([0,1,1,0],[0,1,0,1]);   // [[1,1],[1,1]]
```

### Model selection

**`trainTestSplit(n [, {testSize=0.25, shuffle=true, seed=12345}]) -> {train, test}`**

- `n` *(number | Array | TypedArray)* — the index count; an Array/TypedArray uses its length; `n >= 2`.
- `testSize` *(number, default 0.25)* — in (0,1).
- `shuffle` *(boolean, default true)* — draws from the seeded SplitMix64 stream, so a given `seed` reproduces a split exactly.
- `seed` *(number, default 12345)* — the deterministic seed.

Returns `{train, test}` indices, not data — they work against either matrix representation and cost O(n) instead of O(n·features); both sides are always non-empty.

**`kFold(n [, {k=5, shuffle=false, seed=12345}]) -> [{train, test}, ...]`**

- `n` *(number | Array | TypedArray)* — the index count, or an Array/TypedArray whose length is used.
- `k` *(number, default 5)* — the fold count (2..n); `folds` is accepted as an alias for `k`.
- `shuffle` *(boolean, default false)* — shuffles before folding.
- `seed` *(number, default 12345)* — the deterministic seed.

Returns `k` folds with sizes differing by at most one and every index in exactly one test fold. The output budget is capped at 20000000 indices.

**`stratifiedKFold(y [, {k=5, shuffle=false, seed=12345}]) -> [{train, test}, ...]`**

- `y` *(Array | Float64Array)* — the labels; must be finite.
- `k` *(number, default 5)* — the fold count.
- `shuffle` *(boolean, default false)* — shuffles before folding.
- `seed` *(number, default 12345)* — the deterministic seed.

Returns folds like `kFold` but round-robins each class's members across folds, so per-class counts differ by at most one between folds and a rare class cannot be absent from a fold.

**`crossValScore(estimatorFactory, X, y [, options]) -> number[]`**

The options also take `nJobs` and `onProgress`. `nJobs` must be a
positive integer (ToInt64 semantics: a fractional value truncates silently —
`2.5` runs as 2 — and a huge value such as `1e9` is accepted and simply runs
sequentially); it is VALIDATED and then run sequentially — an `os.Worker`
cannot receive the estimator factory (workers take structured-cloneable data,
not functions), so per-fold parallelism is reserved, not faked. `nJobs = 1`
is the identical fast path. `onProgress({done, total})` fires after each fold
(`gridSearch`/`randomSearch` count across the whole search: total =
grid points × folds), and is the hook a future Worker scheduler would drive.

- `estimatorFactory` *(Function)* — `() => new RandomForest(...)`: a factory, not an instance, so a search never measures a model that has already seen a test fold.
- `X` *(Array | Float64Array)* — the data matrix.
- `y` *(Array | Float64Array)* — the labels.
- `options` *(Object)* — the `kFold` options plus `scoring`.
  - `scoring` *(Function)* — a `(yTrue, yPred) => number` function; the default scorer is accuracy (pass your own for regression).

Returns per-fold scores, not averaged, because the spread is the information. Each fold builds a fresh model, fits on the train indices, predicts the test indices, scores, and closes it.

**`gridSearch(estimatorFactory, X, y, grid [, options]) -> {best, bestScore, results}`**

- `estimatorFactory` *(Function)* — takes the parameter object, `(p) => new RandomForest(p)`.
- `X` *(Array | Float64Array)* — the data matrix.
- `y` *(Array | Float64Array)* — the labels.
- `grid` *(Object)* — `{maxDepth: [2,4,8], ...}`; every combination is tried (at most 2^20 points).
- `options` *(Object)* — `seed` and `scoring` like `crossValScore`.

Returns `results` as every point as `{params, scores, mean}`, `best` the winning params and `bestScore` its mean.

**`randomSearch(estimatorFactory, X, y, grid [, options]) -> {best, bestScore, results}`**

- `estimatorFactory` *(Function)* — takes the parameter object, `(p) => new RandomForest(p)`.
- `X` *(Array | Float64Array)* — the data matrix.
- `y` *(Array | Float64Array)* — the labels.
- `grid` *(Object)* — the parameter grid.
- `options` *(Object)* — `seed` and `scoring` like `crossValScore`, plus `nIter`.

Returns the same shape as `gridSearch` over `nIter` (default 10) sampled points — sampling beats the grid when one parameter matters much more than another.

```js
import { trainTestSplit, kFold, stratifiedKFold, crossValScore, gridSearch, randomSearch, LogisticRegression, DecisionTreeClassifier } from "dyna:ml";

const X = [[0,0],[1,0],[0,1],[5,5],[6,5],[5,6]];
const y = [0,0,0,1,1,1];
trainTestSplit(10, {testSize: 0.3, seed: 1}).test.length === 3;
kFold(X, {k: 3}).length === 3;
stratifiedKFold(y, {k: 3, seed: 2}).length === 3;
crossValScore(() => new LogisticRegression({maxIter: 200}), X, y, {k: 3, seed: 1});
gridSearch((p) => new DecisionTreeClassifier(p), X, y, {maxDepth: [1,3]}, {k: 3, seed: 2}).results.length === 2;
randomSearch((p) => new DecisionTreeClassifier(p), X, y, {maxDepth: [1,2,3]}, {nIter: 2, k: 3}).results.length === 2;
```

### Missing data

**`imputeMean(X [, rows, cols]) -> number[][]`**

- `X` *(Array | Float64Array)* — the matrix to impute.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns a new matrix with every non-finite entry replaced by its column's mean over the finite values. A column with no finite value throws (filling it with 0 would invent data). The input is copied even from a flat Float64Array, since imputation writes.

**`dropMissing(X, y? [, rows, cols]) -> {X, y, kept}`**

- `X` *(Array | Float64Array)* — the data matrix.
- `y` *(Array | Float64Array)* — optional; returned as a Float64Array when given.
- `rows`, `cols` *(number)* — the shape of `X` when it is a flat Float64Array.

Returns every row holding a non-finite value removed, in order; `kept` is the surviving row indices, so other columns (weights, ids) can follow along without re-deriving which rows survived.

```js
import { imputeMean, dropMissing } from "dyna:ml";

imputeMean([[1, NaN], [2, 4], [NaN, 6]]);    // [[1,5],[2,4],[1.5,6]]
const dm = dropMissing([[1, NaN],[2,4]], [10,20]);
dm.X;                              // [[2,4]]
dm.kept;                           // [1]
```

### Persistence and lifecycle

Every fitted model serialises through the same binary DYNS envelope; doubles go out as their IEEE-754 bit patterns, so a loaded model produces bit-identical predictions to the one it was saved from, and a record loaded into the wrong class is refused by name. `serialize()` throws before fit. `save(path)`/`load(path)` take a `Path` from `dyna:file`.

Every model class carries the full surface — instance `serialize() -> Uint8Array`, `save(path) -> number` (bytes written), `close()`, `dispose()`, `[Symbol.dispose]`, `closed`; constructor `deserialize(bytes|ArrayBuffer) -> model`, `load(path) -> model` — on: `LinearRegression`, `LogisticRegression`, `KMeans`, `SVC`, `GaussianMixture`, `GaussianNB`, `DecisionTreeClassifier`, `DecisionTreeRegressor`, `RandomForestClassifier`, `RandomForestRegressor`, `GradientBoostingRegressor`, `GradientBoostingClassifier`, `XGBRegressor`, `XGBClassifier`, `PCA`, `KNClassifier`, `KNRegressor`, `DBScan`, `StandardScaler`, `MinMaxScaler`. `CSR` and `Pipeline` are resources with `close()`/`dispose()`/`[Symbol.dispose]`/`closed` but no persistence.

```js
import * as __ml from "dyna:ml";
import { Path } from "dyna:file";
Object.assign(globalThis, __ml);

const m = new LinearRegression();
m.fit([[1],[2],[3]], [3,5,7]);
const copy = LinearRegression.deserialize(m.serialize());
m.save(new Path("/tmp/linreg.bin"));
LinearRegression.load(new Path("/tmp/linreg.bin")).predict([[4]]);
m.close();
```

### API surface at a glance

`CSR`: `CSR.fromDense`, `CSR.prototype.close`, `CSR.prototype.closed`, `CSR.prototype.cols`, `CSR.prototype.density`, `CSR.prototype.dispose`, `CSR.prototype.nnz`, `CSR.prototype.row`, `CSR.prototype.rows`, `CSR.prototype.toDense`
`DBScan`: `DBScan.deserialize`, `DBScan.load`, `DBScan.prototype.close`, `DBScan.prototype.closed`, `DBScan.prototype.dispose`, `DBScan.prototype.eps`, `DBScan.prototype.fit`, `DBScan.prototype.labels`, `DBScan.prototype.nClusters`, `DBScan.prototype.save`, `DBScan.prototype.serialize`
`DecisionTreeClassifier`: `DecisionTreeClassifier.deserialize`, `DecisionTreeClassifier.load`, `DecisionTreeClassifier.prototype.apply`, `DecisionTreeClassifier.prototype.close`, `DecisionTreeClassifier.prototype.closed`, `DecisionTreeClassifier.prototype.depth`, `DecisionTreeClassifier.prototype.dispose`, `DecisionTreeClassifier.prototype.featureImportances`, `DecisionTreeClassifier.prototype.fit`, `DecisionTreeClassifier.prototype.predict`, `DecisionTreeClassifier.prototype.predictProba`, `DecisionTreeClassifier.prototype.save`, `DecisionTreeClassifier.prototype.serialize`
`DecisionTreeRegressor`: `DecisionTreeRegressor.deserialize`, `DecisionTreeRegressor.load`, `DecisionTreeRegressor.prototype.apply`, `DecisionTreeRegressor.prototype.close`, `DecisionTreeRegressor.prototype.closed`, `DecisionTreeRegressor.prototype.depth`, `DecisionTreeRegressor.prototype.dispose`, `DecisionTreeRegressor.prototype.featureImportances`, `DecisionTreeRegressor.prototype.fit`, `DecisionTreeRegressor.prototype.predict`, `DecisionTreeRegressor.prototype.predictProba`, `DecisionTreeRegressor.prototype.save`, `DecisionTreeRegressor.prototype.serialize`
`GaussianMixture`: `GaussianMixture.deserialize`, `GaussianMixture.load`, `GaussianMixture.prototype.close`, `GaussianMixture.prototype.closed`, `GaussianMixture.prototype.dispose`, `GaussianMixture.prototype.fit`, `GaussianMixture.prototype.logLikelihood`, `GaussianMixture.prototype.means`, `GaussianMixture.prototype.nIter`, `GaussianMixture.prototype.predict`, `GaussianMixture.prototype.predictProba`, `GaussianMixture.prototype.save`, `GaussianMixture.prototype.serialize`, `GaussianMixture.prototype.variances`, `GaussianMixture.prototype.weights`
`GaussianNB`: `GaussianNB.deserialize`, `GaussianNB.load`, `GaussianNB.prototype.classes`, `GaussianNB.prototype.close`, `GaussianNB.prototype.closed`, `GaussianNB.prototype.dispose`, `GaussianNB.prototype.fit`, `GaussianNB.prototype.predict`, `GaussianNB.prototype.predictProba`, `GaussianNB.prototype.save`, `GaussianNB.prototype.serialize`
`GradientBoostingClassifier`: `GradientBoostingClassifier.deserialize`, `GradientBoostingClassifier.load`, `GradientBoostingClassifier.prototype.apply`, `GradientBoostingClassifier.prototype.close`, `GradientBoostingClassifier.prototype.closed`, `GradientBoostingClassifier.prototype.depth`, `GradientBoostingClassifier.prototype.dispose`, `GradientBoostingClassifier.prototype.featureImportances`, `GradientBoostingClassifier.prototype.fit`, `GradientBoostingClassifier.prototype.predict`, `GradientBoostingClassifier.prototype.predictProba`, `GradientBoostingClassifier.prototype.save`, `GradientBoostingClassifier.prototype.serialize`
`GradientBoostingRegressor`: `GradientBoostingRegressor.deserialize`, `GradientBoostingRegressor.load`, `GradientBoostingRegressor.prototype.apply`, `GradientBoostingRegressor.prototype.close`, `GradientBoostingRegressor.prototype.closed`, `GradientBoostingRegressor.prototype.depth`, `GradientBoostingRegressor.prototype.dispose`, `GradientBoostingRegressor.prototype.featureImportances`, `GradientBoostingRegressor.prototype.fit`, `GradientBoostingRegressor.prototype.predict`, `GradientBoostingRegressor.prototype.predictProba`, `GradientBoostingRegressor.prototype.save`, `GradientBoostingRegressor.prototype.serialize`
`KMeans`: `KMeans.deserialize`, `KMeans.load`, `KMeans.prototype.close`, `KMeans.prototype.closed`, `KMeans.prototype.dispose`, `KMeans.prototype.fit`, `KMeans.prototype.inertia`, `KMeans.prototype.predict`, `KMeans.prototype.save`, `KMeans.prototype.serialize`
`KNClassifier`: `KNClassifier.deserialize`, `KNClassifier.load`, `KNClassifier.prototype.close`, `KNClassifier.prototype.closed`, `KNClassifier.prototype.dispose`, `KNClassifier.prototype.fit`, `KNClassifier.prototype.predict`, `KNClassifier.prototype.save`, `KNClassifier.prototype.serialize`
`KNRegressor`: `KNRegressor.deserialize`, `KNRegressor.load`, `KNRegressor.prototype.close`, `KNRegressor.prototype.closed`, `KNRegressor.prototype.dispose`, `KNRegressor.prototype.fit`, `KNRegressor.prototype.predict`, `KNRegressor.prototype.save`, `KNRegressor.prototype.serialize`
`LinearRegression`: `LinearRegression.deserialize`, `LinearRegression.load`, `LinearRegression.prototype.close`, `LinearRegression.prototype.closed`, `LinearRegression.prototype.coef`, `LinearRegression.prototype.dispose`, `LinearRegression.prototype.fit`, `LinearRegression.prototype.intercept`, `LinearRegression.prototype.predict`, `LinearRegression.prototype.save`, `LinearRegression.prototype.serialize`
`LogisticRegression`: `LogisticRegression.deserialize`, `LogisticRegression.load`, `LogisticRegression.prototype.classes`, `LogisticRegression.prototype.close`, `LogisticRegression.prototype.closed`, `LogisticRegression.prototype.coef`, `LogisticRegression.prototype.converged`, `LogisticRegression.prototype.dispose`, `LogisticRegression.prototype.fit`, `LogisticRegression.prototype.intercept`, `LogisticRegression.prototype.nIter`, `LogisticRegression.prototype.predict`, `LogisticRegression.prototype.predictProba`, `LogisticRegression.prototype.save`, `LogisticRegression.prototype.serialize`
`MinMaxScaler`: `MinMaxScaler.deserialize`, `MinMaxScaler.load`, `MinMaxScaler.prototype.close`, `MinMaxScaler.prototype.closed`, `MinMaxScaler.prototype.dataMax`, `MinMaxScaler.prototype.dataMin`, `MinMaxScaler.prototype.dispose`, `MinMaxScaler.prototype.fit`, `MinMaxScaler.prototype.fitTransform`, `MinMaxScaler.prototype.inverseTransform`, `MinMaxScaler.prototype.save`, `MinMaxScaler.prototype.serialize`, `MinMaxScaler.prototype.transform`
`PCA`: `PCA.deserialize`, `PCA.load`, `PCA.prototype.close`, `PCA.prototype.closed`, `PCA.prototype.components`, `PCA.prototype.dispose`, `PCA.prototype.explainedVariance`, `PCA.prototype.explainedVarianceRatio`, `PCA.prototype.fit`, `PCA.prototype.fitTransform`, `PCA.prototype.inverseTransform`, `PCA.prototype.mean`, `PCA.prototype.save`, `PCA.prototype.serialize`, `PCA.prototype.transform`
`Pipeline`: `Pipeline.prototype.close`, `Pipeline.prototype.closed`, `Pipeline.prototype.dispose`, `Pipeline.prototype.estimator`, `Pipeline.prototype.fit`, `Pipeline.prototype.fitted`, `Pipeline.prototype.predict`, `Pipeline.prototype.predictProba`, `Pipeline.prototype.stage`, `Pipeline.prototype.transform`
`RandomForestClassifier`: `RandomForestClassifier.deserialize`, `RandomForestClassifier.load`, `RandomForestClassifier.prototype.apply`, `RandomForestClassifier.prototype.close`, `RandomForestClassifier.prototype.closed`, `RandomForestClassifier.prototype.depth`, `RandomForestClassifier.prototype.dispose`, `RandomForestClassifier.prototype.featureImportances`, `RandomForestClassifier.prototype.fit`, `RandomForestClassifier.prototype.predict`, `RandomForestClassifier.prototype.predictProba`, `RandomForestClassifier.prototype.save`, `RandomForestClassifier.prototype.serialize`
`RandomForestRegressor`: `RandomForestRegressor.deserialize`, `RandomForestRegressor.load`, `RandomForestRegressor.prototype.apply`, `RandomForestRegressor.prototype.close`, `RandomForestRegressor.prototype.closed`, `RandomForestRegressor.prototype.depth`, `RandomForestRegressor.prototype.dispose`, `RandomForestRegressor.prototype.featureImportances`, `RandomForestRegressor.prototype.fit`, `RandomForestRegressor.prototype.predict`, `RandomForestRegressor.prototype.predictProba`, `RandomForestRegressor.prototype.save`, `RandomForestRegressor.prototype.serialize`
`SVC`: `SVC.deserialize`, `SVC.load`, `SVC.prototype.classes`, `SVC.prototype.close`, `SVC.prototype.closed`, `SVC.prototype.decisionFunction`, `SVC.prototype.dispose`, `SVC.prototype.fit`, `SVC.prototype.nSupportVectors`, `SVC.prototype.predict`, `SVC.prototype.save`, `SVC.prototype.serialize`
`StandardScaler`: `StandardScaler.deserialize`, `StandardScaler.load`, `StandardScaler.prototype.close`, `StandardScaler.prototype.closed`, `StandardScaler.prototype.dispose`, `StandardScaler.prototype.fit`, `StandardScaler.prototype.fitTransform`, `StandardScaler.prototype.inverseTransform`, `StandardScaler.prototype.mean`, `StandardScaler.prototype.save`, `StandardScaler.prototype.serialize`, `StandardScaler.prototype.std`, `StandardScaler.prototype.transform`
`XGBClassifier`: `XGBClassifier.deserialize`, `XGBClassifier.load`, `XGBClassifier.prototype.apply`, `XGBClassifier.prototype.bestRounds`, `XGBClassifier.prototype.close`, `XGBClassifier.prototype.closed`, `XGBClassifier.prototype.depth`, `XGBClassifier.prototype.dispose`, `XGBClassifier.prototype.featureImportances`, `XGBClassifier.prototype.fit`, `XGBClassifier.prototype.predict`, `XGBClassifier.prototype.predictProba`, `XGBClassifier.prototype.save`, `XGBClassifier.prototype.serialize`
`XGBRegressor`: `XGBRegressor.deserialize`, `XGBRegressor.load`, `XGBRegressor.prototype.apply`, `XGBRegressor.prototype.bestRounds`, `XGBRegressor.prototype.close`, `XGBRegressor.prototype.closed`, `XGBRegressor.prototype.depth`, `XGBRegressor.prototype.dispose`, `XGBRegressor.prototype.featureImportances`, `XGBRegressor.prototype.fit`, `XGBRegressor.prototype.predict`, `XGBRegressor.prototype.predictProba`, `XGBRegressor.prototype.save`, `XGBRegressor.prototype.serialize`

# dyna:file

Filesystem access and buffered I/O: path values, file handles, streams, locks, watchers, globbing, temp files and platform directories.

`import { Path, File, FileReader, FileWriter, FileLock, Watcher, Glob, readFile, readBytes, writeFile, readDir, makeDir, removeAll, glob } from "dyna:file";`

Every entry point takes a `Path`, never a string — a `Path` is built once and borrowed by every syscall. Every operation is **strict**: the path is walked component-by-component with `O_NOFOLLOW`, so a symlink anywhere in the path is refused with `ELOOP`. `new Path(...)` resolves the longest existing prefix (so `/tmp` works on macOS), but a symlink swapped in after construction is still refused; call `realPath()` once at a trust boundary if you must reach through a system symlink. All fs-mutation calls below create, overwrite, move or delete real filesystem entries — none of the reads (`stat`, `exists`, `readDir`, `glob`, `sniffType`) mutate anything.

**Options bags are strict.** Every options object in this module refuses an unknown key with a `TypeError` naming the key and the valid ones — `unknown option "recursiv" (valid: recursive, mode)`. `null`/`undefined` in the options position still mean "no options". The rule covers every bag in the module: `readFile`, `readFileAsync`, `writeFile`, `writeFileAsync`, `copyFile`, `copyFileAsync`, `makeDir`, `glob`, `File.copyTo`, and the `FileReader`/`FileWriter`/`FileLock`/`Watcher` constructors.

### Path

Value handle over one normalised path. Constructed with its data, immutable, refcounted. A trailing separator is stripped from OS-derived paths so `Path.temp()` and `tempDir()` stringify identically.

**`new Path(...segments) -> Path`**

- `...segments` *(string | Path)* — one or more segments to join; at least one is required.

Joins and normalises the segments. Segments may be strings or `Path`s; a segment containing a NUL byte is refused. `new Path(p)` on an existing `Path` shares its buffer.

**`Path.cwd() -> Path`**

Returns the current working directory.

**`Path.home() -> Path`**

Returns `$HOME`, else the passwd entry.

**`Path.temp() -> Path`**

Returns `$TMPDIR`, else `/tmp`, with the trailing slash stripped and the prefix resolved.

**`Path.isPath(v) -> boolean`**

- `v` *(any)* — the value to test.

Returns `true` when `v` is a `Path`.

**`Path.sep` / `Path.delimiter` -> string**

`"/"` and `":"` on this platform.

**`p.dirname -> Path`**

A new `Path`, a slice of the cached buffer with no re-normalisation.

**`p.basename` / `p.extname` -> string**

Strings.

**`p.isAbsolute -> boolean`**

Whether the path is absolute.

**`p.join(...segments) -> Path`**

- `...segments` *(string | Path)* — segments appended after `this`.

`this` leads, then the arguments; returns a new `Path`.

**`p.resolve(...segments) -> Path`**

- `...segments` *(string | Path)* — segments to resolve against `this`.

A `join` that resolves `.`/`..` components; an absolute argument rebases.

**`p.relativeTo(other) -> Path`**

- `other` *(Path)* — the target path.

The relative path from `this` to `other`; the empty result normalises to `.`.

**`p.equals(other) -> boolean`**

- `other` *(any)* — the value to compare.

Byte equality of the two normalised forms, so `"a//b"` equals `"a/b"`. Returns `false` for a non-`Path` argument.

**`p.basenameWithout(suffix) -> string`**

- `suffix` *(string)* — the suffix to strip.

The basename with the given suffix removed.

**`p.toString()` / `p.toJSON() -> string`**

The normalised path string.

```js
import { Path } from "dyna:file";
const p = new Path("/a/b/c.txt");
const joined = new Path("/a").join("b", "c.txt");
const resolved = p.resolve("..", "d");
const rel = new Path("/a/b/c.txt").relativeTo(new Path("/a"));
print(p.basename, p.extname, String(p.dirname), p.isAbsolute);
print(String(joined) === "/a/b/c.txt");
print(String(resolved) === "/a/b/d");
print(rel.equals(new Path("../../c.txt")), Path.isPath(p));
print(Path.sep, Path.delimiter, String(Path.temp()));
```

### File

A handle over one path: every method is the corresponding free function with the path supplied once. Constructed from a `Path` or, here only, a string.

**`new File(path)`**

- `path` *(Path | string)* — the file to open.

Accepts a `Path` or a string.

**`f.path -> Path`**

The underlying `Path`.

**`f.readText() -> string`**

Returns the whole file as a string. Invalid UTF-8 becomes U+FFFD; use `readBytes()` for binary data.

**`f.readBytes() -> Uint8Array`**

Returns the whole file as a `Uint8Array`, read directly so no byte is corrupted.

**`f.writeText(data[, options]) -> number`**

- `data` *(string | bytes)* — the content to write.
- `options` *(object)* — optional.
  - `append` *(boolean, default `false`)* — when `true`, appends; otherwise truncates.

Writes string or bytes. Returns the byte count.

**`f.writeBytes(data[, options]) -> number`**

- `data` *(bytes)* — the content to write.
- `options` *(object)* — as for `writeText`.

Alias of `writeText`; the underlying write accepts strings and every byte view.

**`f.append(data)`**

- `data` *(string | bytes)* — the content to append.

`writeText` with `append` supplied.

**`f.stat()` / `f.lstat()` -> object**

Returns a stat object (see `stat`).

**`f.exists() -> boolean`**

Returns a boolean; never throws.

**`f.remove()`**

Unlinks the file or empty directory.

**`f.realPath() -> Path`**

Returns the resolved `Path`.

**`f.chmod(mode)`**

- `mode` *(number)* — the permission bits.

Changes permissions.

**`f.moveTo(dest) -> this`**

- `dest` *(Path)* — the destination path.

Renames; on success the handle names the new location and the call returns the handle itself. The return-`this` is deliberate: after a move there is exactly one file, and the handle you hold IS it.

**`f.copyTo(dest[, options]) -> File`**

- `dest` *(Path)* — the destination path.
- `options` *(object)* — optional.
  - `overwrite` *(boolean, default `false`)* — when `true`, an existing destination is truncated and replaced; otherwise the copy is refused with `EEXIST`. Same contract as the free `copyFile` — the handle form used to lack the flag for no reason.

Byte copy to a **new** `File`, not a hard link; the source survives untouched, so a distinct handle naming the destination is what comes back. Note the pairing: `moveTo` returns `this` (the file moved, the handle follows it), `copyTo` returns a new `File` for the new copy.

**`f.reader([options])` / `f.writer([options])`**

- `options` *(object)* — forwarded to the reader or writer constructor.

A `FileReader` / `FileWriter` over this path.

**`f.toString()` / `f.toJSON() -> string`**

The path string.

```js
import { Path, File, makeDir, removeAll } from "dyna:file";
const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const f = new File(new Path(dir, "data.txt"));
f.writeText("hello");
const bytes = f.readBytes();
f.append(" world");
print(f.readText(), f.stat().size, bytes instanceof Uint8Array);
const c = f.copyTo(new Path(dir, "copy.txt"));
print(String(c.path), f.exists(), f.path.isAbsolute);
const c2 = f.copyTo(new Path(dir, "copy.txt"), { overwrite: true });  /* refused without the flag */
print(c2 instanceof File, f.writeText("moved?") >= 0);
const m = f.moveTo(new Path(dir, "moved.txt"));   /* returns the handle itself */
print(String(f.path).endsWith("moved.txt"), m === f);
removeAll(dir);
```

### FileReader

Buffered sequential reader over a strictly-opened fd. A resource: closed explicitly or by the GC finalizer, both paths ASan-clean. Like `readFile`'s whole-file read, the accumulating reads are capped at 1 GiB (`DYN_MAX_INPUT`): a `read()` with `n` omitted, a `readAll()`, a `readBytes()` with `n` omitted, or a `readLine()` over a newline-less stream throws a `RangeError` past it instead of growing without bound.

Two read families, by contract: `read`/`readLine`/`readAll` are the **string** paths (UTF-8 decoded; invalid bytes become U+FFFD and cannot be recovered), `readInto`/`readBytes` are the **byte** paths (raw file bytes, nothing decoded).

**`new FileReader(path[, options])`**

- `path` *(Path | string)* — the file to read.
- `options` *(object)* — optional.
  - `bufferSize` *(number, default `128 * 1024`)* — clamped to 4 KiB..64 MiB.

**`r.read([n]) -> string`**

- `n` *(number)* — the maximum number of bytes to read; omitted reads all.

Returns up to `n` bytes as a UTF-8 string, `""` at EOF.

**`r.readLine() -> string | null`**

Returns the next line without its trailing newline (CRLF handled); `null` at a clean EOF.

**`r.readAll() -> string`**

Returns the rest of the file.

**`r.readInto(buf) -> number`**

- `buf` *(Uint8Array | TypedArray | DataView)* — the destination; the view's own byte offset and length bound the copy, not the underlying buffer.

Copies up to `buf.length` **raw bytes** straight out of the read buffer and returns how many landed; `0` means EOF (and is also what a zero-length buffer reports, by the same "nothing copied" contract). A buffer larger than the reader's `bufferSize` simply loops internal fills — no need to size `buf` to the reader. This is a bytes primitive: **no UTF-8 repair happens**, so a multi-byte sequence split at the edge of your buffer arrives split, exactly as it sits on disk; reassemble at the text layer if text is what you want.

**`r.readBytes([n]) -> Uint8Array`**

- `n` *(number)* — the maximum number of bytes; omitted reads all.

The byte twin of `read(n)`: a fresh `Uint8Array` of up to `n` raw bytes, the empty array at EOF. Nothing decoded, so what was written is what comes back.

**`r.close()` / `r.dispose()`**

Release the fd; both mark the handle closed.

**`r.closed -> boolean`**

`true` once released.

```js
import { Path, FileReader, writeFile, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const f = new Path(dir, "lines.txt");
writeFile(f, "alpha\nbeta\ngamma");
const r = new FileReader(f);
print(r.readLine(), r.readLine(), r.readLine(), r.readLine());
r.close();
const rb = new FileReader(f, { bufferSize: 4 });   /* tiny buffer: readInto loops fills */
const out = new Uint8Array(16);
const nread = rb.readInto(out);
rb.close();
print(nread, String.fromCharCode(...out.subarray(0, 5)));
removeAll(dir);
```

### FileWriter

Buffered sequential writer. `O_CREAT`; truncates by default, `{append: true}` appends. Flushes buffered bytes on teardown.

**`new FileWriter(path[, options])`**

- `path` *(Path | string)* — the file to write.
- `options` *(object)* — optional.
  - `bufferSize` *(number, default `128 * 1024`)* — clamped as for `FileReader`.
  - `preallocate` *(number)* — bytes to preallocate.
  - `append` *(boolean)* — append rather than truncate.

**`w.write(data) -> number`**

- `data` *(string | ArrayBuffer | TypedArray | DataView)* — the content to write.

Accepts a string, an `ArrayBuffer`, or any `TypedArray`/`DataView`; a plain `Uint8Array` is written as its bytes, never as decimal text. Returns the bytes accepted.

**`w.flush()`**

Pushes buffered bytes to the fd.

**`w.sync()`**

Flushes then durably syncs (fcntl `F_FULLFSYNC` on Darwin).

**`w.syncAsync() -> Promise`**

The same durability off the loop. The flush runs on the loop; only the durable sync is offloaded, and only when there is dirty data.

**`w.close()` / `w.dispose()`**

Flushes (best-effort) and closes.

**`w.closed -> boolean`**

`true` once released.

```js
import { Path, File, FileWriter, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const w = new FileWriter(new Path(dir, "out.bin"));
w.write(new Uint8Array([1, 2, 255, 0]));
w.write("tail");
w.sync();
w.close();
const rb = new File(new Path(dir, "out.bin")).readBytes();
print(rb.length, rb[2], w.closed);
removeAll(dir);
```

### FileLock

An advisory exclusive lock via `flock(2)` (or `LockFileEx`, or an `O_EXCL` lock-file fallback). Locks belong to the open file description, so two `FileLock`s on one path conflict even in one process; a stale `flock` dies with its fd. A stale fallback `.lock` file stays until a human removes it.

**`new FileLock(path, { retry, retryMs })`**

- `path` *(Path | string)* — a string path is accepted here.
- `retry` *(number, default `0`)* — retry attempts; must be a number, negatives refused.
- `retryMs` *(number, default `100`)* — retry attempts this many milliseconds apart; must be a number, negatives refused.

Throws `EWOULDBLOCK` when contended.

**`l.withLock(fn)`**

- `fn` *(function)* — the callback to run under the lock.

Calls `fn`, then releases the lock no matter what `fn` did; returns `fn`'s value (or its exception). The lock is consumed.

**`l.close()` / `l.dispose()`**

Release the lock.

**`l.closed -> boolean`**

`true` once released.

```js
import { Path, FileLock, writeFile, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const target = new Path(dir, "target.txt");
writeFile(target, "x");
const lock = new FileLock(target);
let contended = false;
try { new FileLock(target, { retry: 0 }); } catch (e) { contended = e.code === "EWOULDBLOCK"; }
print(lock.withLock(() => "critical"), contended);
print(lock.closed);
removeAll(dir);
```

### Watcher

Kernel-event file watching (kqueue on macOS, inotify on Linux), one classifier for both: events are derived by diffing a snapshot of the tree, so the two platforms emit identical events. Needs the shared reactor; refuses on a reactor with no vnode interest.

**`new Watcher(path, { recursive, debounceMs, ignore })`**

- `path` *(Path)* — the root to watch.
- `recursive` *(boolean, default `true`)* — watch subdirectories.
- `debounceMs` *(number)* — editor save bursts are coalesced by the debounce.
- `ignore` *(array of glob patterns)* — paths to skip.

`opts` must be an object when given (`null`/`undefined` mean no options); a string or number in the options position throws `TypeError` instead of being silently ignored.

The first walk is the baseline, so nothing already present is reported. Every later change scan is a full tree diff per debounced burst, linear in snapshot size — the 100000-entry cap is the bound, and huge trees pay proportionally per event burst.

**`w.start([cb])`**

- `cb` *(function)* — optional; receives the typed event `{ path, kind }` with `kind` one of `change`, `add`, `addDir`, `unlink`, `unlinkDir` and `path` a plain string relative to the root.

Arms the watch. Throws if already started or the root is not a directory. A `(re)`start reopens the event stream a `stop()` ended. Events go to BOTH surfaces: the callback and the async iteration below.

**`for await (const ev of w)`**

The async iteration over the same typed events — the house manual-iterator shape: `w.next()` resolves `{ value, done }` per pull, `w.return()` is what a `break` calls, and `w[Symbol.asyncIterator]()` is the watcher itself. Concurrent `next()` calls settle in arrival order; events arriving between pulls buffer (capped at 8192, past which the newest is dropped and `truncated` says so). A `break` runs the unconditional cleanup doctrine (`cf.` pg.queryIter): it ends the iteration AND stops the watch.

<!-- check:skip (drives live kernel events; the runnable form is tests/test_watch_iter.js) -->
```js
const w = new Watcher(dir, { debounceMs: 10 });
w.start();
(async () => {
    for await (const ev of w) {
        if (ev.kind === "add" && ev.path === "done.txt") break;   // stops the watch
    }
})();
```

**`w.stop()`**

Halts the watch (disarm, end the stream) but keeps the object alive, `closed === false`, and re-`start()`able. Idempotent, and a no-op after `close()`. The end-of-stream contract is the Channel close contract: events ALREADY buffered still drain through `next()` in arrival order and `{ done: true }` comes after the drain, so stop-then-iterate always terminates; a parked `next()` with an empty buffer resolves done immediately.

**`w.stats() -> object`**

Returns `{ entries, directories, events, truncated, debounceMs }`. Snapshot caps (100000 entries, depth 64, 4096 fds on kqueue, 8192 buffered events) set `truncated` rather than going silent. Works after `stop()`.

**`w.close()` / `w.dispose()`**

Disarm, release, and end the iteration immediately (buffered events are dropped -- an explicit close is an explicit end; use `stop()` to drain first).

**`w.closed -> boolean`**

`true` once released.

```js
import { Path, Watcher, writeFile, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
writeFile(new Path(dir, "seed.txt"), "x");
const w = new Watcher(dir, { debounceMs: 10 });
w.start((ev) => print("event:", ev.kind, ev.path));
writeFile(new Path(dir, "w.txt"), "data");
setTimeout(() => { const stats = w.stats(); w.close(); print(w.closed, stats.entries >= 1); removeAll(dir); }, 120);
```

### Glob

A compiled pattern over the same `*`, `**`, `?`, `[...]` matcher `glob()` walks with. The pattern is configuration, checked once at construction.

**`new Glob(pattern)`**

- `pattern` *(string)* — the glob pattern; requires a string.

`hasWildcard` is computed once.

**`Glob.match(path, pattern) -> boolean`**

- `path` *(Path | string)* — the path to test; a `Path` contributes its normalised bytes (`""` becomes `"."`) while a plain string is used verbatim.
- `pattern` *(string)* — the glob pattern.

The one-shot spelling of `new Glob(pattern).matches(path)`: same matcher, no instance to hold, purely lexical. The lexical grammar, runtime-verified against this matcher: `*` and `**` match any run of characters (INCLUDING `/`, and INCLUDING the empty run) and `?` matches exactly one character (also `/`); literals stay literal — `"a/**/c"` does **not** match `"a/c"` because the pattern's two slashes are two slashes, while `"x**y"` matches `"xy"`; `[abc]`/`[a-c]` are character classes with `[!...]`/`[^...]` negation; there is NO leading-dot rule (`"*idden"` matches `".hidden"`); backslash is a literal byte, not an escape; an unterminated `[` is literal text. Brace expansion `{a,b}` is NOT supported anywhere in this module.

**`g.matches(path) -> boolean`**

- `path` *(Path)* — the path to test.

Lexical match only, no filesystem access, so it works on paths that do not exist. The lexical matcher runs the pattern against the **whole path string** with `/` as an ordinary byte: `*` and `?` cross segment boundaries, a star run may match the empty string but its surrounding literals stay literal (so `**/b` does NOT match `b` — the run may vanish, the slash may not), and the walk's leading-dot rule does **not** apply (`*idden` matches `.hidden` here). The per-segment, dotfile-aware semantics you see from `glob()` come from the walk, not from this matcher — pin both behaviors, don't mix them.

**`g.expand([cwd]) -> Path[]`**

- `cwd` *(Path)* — optional; the directory to walk from.

The `glob()` walk; returns matching `Path`s.

**`g.filter(paths[]) -> Path[]`**

- `paths` *(Path[])* — the array to filter.

The subset of the array matching the pattern.

**`g.pattern` / `g.hasWildcard`**

Accessors.

```js
import { Path, Glob, writeFile, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
writeFile(new Path(dir, "a.bin"), "1");
writeFile(new Path(dir, "b.txt"), "2");
const g = new Glob("*.bin");
print(g.pattern, g.hasWildcard);
print(g.matches(new Path(dir, "a.bin")));
print(g.filter([new Path(dir, "x.bin"), new Path(dir, "y.txt")]).length);
print(g.expand(dir).length);
removeAll(dir);
```

### Content I/O

One-shot reads and writes over a path; the async pair is a two-strategy portfolio — payloads under 1 MiB run inline on the loop, larger ones offload to the pool — so the loop keeps serving while a big file transfers.

**`readFile(path[, { bytes }]) -> string | Uint8Array`**

- `path` *(Path)* — the file to read.
- `bytes` *(boolean, default `false`)* — when `true`, returns a `Uint8Array` of the raw file bytes instead of a decoded string.

Returns the whole file; strict open first. The string form decodes UTF-8, so it is lossy for binary data (invalid bytes become U+FFFD) — pass `{bytes: true}` for anything that is not text. Like `FileReader`'s accumulating reads, the whole-file read is capped at 1 GiB (`DYN_MAX_INPUT`): a larger file throws a `RangeError` instead of growing without bound.

**`readBytes(path) -> Uint8Array`**

- `path` *(Path)* — the file to read.

The bytes alias of `readFile(path, {bytes: true})`, named for what it returns; identical to `File.readBytes()`. Same strict open, same 1 GiB cap, same `RangeError` past it.

**`writeFile(path, data[, { append }]) -> number`**

- `path` *(Path)* — the file to write.
- `data` *(string | bytes)* — the content.
- `append` *(boolean, default `false`)* — append rather than truncate.

String or bytes; `O_CREAT`, truncate unless `append`. Returns the byte count.

**`readFileAsync(path[, { bytes }]) -> Promise`**

- `path` *(Path)* — the file to read.
- `bytes` *(boolean, default `false`)* — when `true`, resolves a `Uint8Array`; else a string.

A non-regular path offloads whatever its size.

**`writeFileAsync(path, data[, { append }]) -> Promise`**

- `path` *(Path)* — the file to write.
- `data` *(string | bytes)* — the content.
- `append` *(boolean)* — append rather than truncate.

Resolves the byte count; the payload is copied before the call returns.

**`copyFileAsync(from, to[, { overwrite }]) -> Promise`**

- `from` *(Path)* — the source.
- `to` *(Path)* — the destination.
- `overwrite` *(boolean, default `false`)* — when `true`, truncates an existing destination; otherwise the promise rejects with `EEXIST`.

The `copyFile` refusal semantics off the loop: the copy rides the same kernel path (`fcopyfile`/`copy_file_range`, read/write fallback) on the pool, the destination gets the source's permission bits, and the promise rejects with the same message the sync call throws — a missing source, a directory source, an unwritable destination. Sources from `copyMin` (1 MiB) up are offloaded; a non-regular source offloads whatever its size, because an open that can block never belongs to the loop thread. Resolves the source byte count.


**`asyncStats() -> object`**

Returns `{ inline, offloaded, readMin, writeMin }`, the per-process counters and the 1 MiB thresholds, so a test can assert which arm ran.

```js
import { Path, writeFile, readFile, readBytes, writeFileAsync, asyncStats, makeDir, removeAll } from "dyna:file";
const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const f = new Path(dir, "t.txt");
print(writeFile(f, "hello") === 5, readFile(f));
writeFile(f, new Uint8Array([1, 2, 255, 0, 3]));
const raw = readBytes(f);
print(raw.length, raw[2], raw[2] === readFile(f, { bytes: true })[2]);
writeFileAsync(f, "!", { append: true }).then((n) => {
    print(n, readBytes(f).length === 6, asyncStats().inline >= 1);
    removeAll(dir);
}).catch(() => { removeAll(dir); });
```

### Metadata

**`stat(path) -> object`**

- `path` *(Path)* — the path to stat.

Follows the final component; returns a plain object with `size`, `mode`, `isDir`, `isFile`, `isSymlink`, `mtimeMs`, `atimeMs`, `ctimeMs`, `uid`, `gid`, `ino`, `nlink`.

**`lstat(path) -> object`**

- `path` *(Path)* — the path to stat.

Does not follow the final component; intermediate components are still strict.

**`exists(path) -> boolean`**

- `path` *(Path)* — the path to test.

Returns a boolean; never throws, any error yields `false`. Uses `lstat`, so a dangling symlink reports `true`.

```js
import { Path, writeFile, stat, lstat, exists, makeDir, removeAll } from "dyna:file";
const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const f = new Path(dir, "t.txt");
writeFile(f, "data");
const st = stat(f);
print(st.size, st.isFile, st.mtimeMs > 0);
print(exists(f), exists(new Path(dir, "missing")), lstat(f).nlink >= 1);
removeAll(dir);
```

### Directories

**`readDir(path) -> { name, isDir, isFile, isSymlink }[]`**

- `path` *(Path)* — the directory to read.

Sorted entries; `.`/`..` excluded. Uses `d_type` with an `lstat` fallback.

**`makeDir(path[, { recursive, mode }])`**

- `path` *(Path)* — the directory to create.
- `recursive` *(boolean)* — create missing parents (an existing directory is success).
- `mode` *(number, default `0777`)* — the permission bits.

Creates a directory.

**`remove(path)`**

- `path` *(Path)* — the entry to remove.

Unlinks a file or an *empty* directory (libc `remove`).

**`removeAll(path)`**

- `path` *(Path)* — the entry to remove.

Recursive and symlink-safe (never descends through a symlink); a missing path is a no-op. Depth bounded at 512.

**`rename(from, to)`**

- `from` *(Path)* — the source.
- `to` *(Path)* — the destination.

`rename(2)`.

**`copyFile(from, to[, { overwrite }])`**

- `from` *(Path)* — the source.
- `to` *(Path)* — the destination.
- `overwrite` *(boolean, default `false`)* — when `true`, truncates an existing destination; otherwise it is refused with `EEXIST`.

Byte copy through the kernel (`fcopyfile`/`copy_file_range`) with a read/write fallback. The destination gets the source's permission bits, so a private file stays private.

**`move(from, to)`**

- `from` *(Path)* — the source.
- `to` *(Path)* — the destination.

`rename(2)`, atomic within one filesystem; across filesystems (`EXDEV`) it falls back to copy-then-unlink, which is not atomic and says so — the source is unlinked last, so a failure leaves both copies.

**`sniffType(pathOrBytes) -> string`**

- `pathOrBytes` *(Path | byte view)* — the bytes to sniff.

MIME type from magic bytes, not the extension; SQLite, tar, gzip, zip, PNG, JPEG, wasm and more.

```js
import { Path, makeDir, writeFile, readDir, exists, rename, copyFile, move, sniffType, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const sub = new Path(dir, "a", "b");
makeDir(sub, { recursive: true });
const f = new Path(sub, "f.txt");
writeFile(f, "hi");
print(readDir(dir).length, exists(f));
rename(f, new Path(sub, "g.txt"));
copyFile(new Path(sub, "g.txt"), new Path(dir, "h.txt"));
move(new Path(dir, "h.txt"), new Path(dir, "i.txt"));
print(exists(new Path(dir, "i.txt")), exists(new Path(dir, "h.txt")));
print(sniffType(new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])));
removeAll(dir);
print(!exists(dir));
```

### Links & permissions

**`symlink(target, linkpath)`**

- `target` *(string)* — the link target, stored verbatim.
- `linkpath` *(Path)* — the link location.

Creates the link. The target may be relative or point at nothing; normalising it would rewrite the link, so only the link location is a `Path`.

**`readLink(path) -> string`**

- `path` *(Path)* — the link to read.

The stored target verbatim, as a string (not resolved); grows its buffer up to 64 KiB.

**`realPath(path) -> Path`**

- `path` *(Path)* — the path to resolve.

The fully resolved path as a `Path`.

**`chmod(path, mode)`**

- `path` *(Path)* — the entry.
- `mode` *(number)* — the permission bits.

Changes permissions.

```js
import { Path, makeDir, writeFile, symlink, readLink, realPath, chmod, stat, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
writeFile(new Path(dir, "orig.txt"), "x");
symlink("orig.txt", new Path(dir, "ln.txt"));
print(readLink(new Path(dir, "ln.txt")), String(realPath(new Path(dir, "orig.txt"))).length > 0);
chmod(new Path(dir, "orig.txt"), 0o600);
print((stat(new Path(dir, "orig.txt")).mode & 0o777) === 0o600);
removeAll(dir);
```

### Globbing

**`glob(pattern[, { cwd }]) -> Path[]`**

- `pattern` *(string)* — the glob pattern.
- `cwd` *(Path, default `.`)* — relative patterns match against it.

Walks the filesystem matching `*`, `**`, `?`, `[...]` (with ranges and `!`/`^` negation). `**` spans zero or more directories and never follows a symlink (a symlinked directory is still emitted as a leaf, but not descended — cycle safety); wildcard and literal segments do follow symlinks to intermediate directories. Collapsed runs of `**` avoid a re-walk DoS. Wildcards never match a leading `.` (minimatch rule), and `**` skips dotfile entries outright — start the pattern segment with `.` to reach them. Brace expansion (`{a,b}`) is NOT supported — list the alternatives as separate patterns. `{cwd}` is the only option (there is no `followSymlinks`); as everywhere in this module, an unknown option key throws. Returns sorted, de-duplicated `Path`s (a symlink-cycle walk can yield one duplicate — a known quirk); an empty pattern matches nothing. Depth bounded at 512.

```js
import { Path, makeDir, writeFile, glob, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynafile-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
writeFile(new Path(dir, "one.txt"), "1");
writeFile(new Path(dir, "two.log"), "2");
makeDir(new Path(dir, "nested"), { recursive: true });
writeFile(new Path(dir, "nested", "three.txt"), "3");
const all = glob("**/*.txt", { cwd: dir });
const top = glob("*.txt", { cwd: dir });
print(all.length, top.length, String(top[0]).endsWith("one.txt"));
removeAll(dir);
```

Showcase — Path arithmetic, streaming reader/writer, locks and a compiled Glob:

```js
import { Path, File, FileReader, FileWriter, FileLock, Glob, makeTempDir, makeDir, writeFile, readFile, removeAll } from "dyna:file";

// Path: immutable, normalized, joined/resolved lazily
const base = new Path(Path.temp(), "dynafile-show");
print(base.basename, base.extname, base.isAbsolute); // dynafile-show "" true
print(base.join("a", "b").toString().endsWith("/a/b")); // true
makeDir(base, { recursive: true });

// File handle: read/write/append, chmod, stat
const f = new File(base.join("data.txt"));
print(f.writeText("line1\nline2\n"), f.append("line3\n")); // 12 6
print(f.readText().split("\n").length);                    // 4
print(f.stat().size, f.exists());                          // 18 true
print(f.readBytes().length);                               // 18

// buffered streaming reader with readLine
const r = new FileReader(base.join("data.txt"));
const lines = [];
let ln;
while ((ln = r.readLine()) !== null) lines.push(ln);
print(lines.join("|"));                                    // line1|line2|line3
r.close();

// buffered writer with flush/sync (F_FULLFSYNC on Darwin)
const w = new FileWriter(base.join("out.bin"), { bufferSize: 16 });
w.write(new Uint8Array([1, 2, 3]));
w.write("tail");
w.sync();
print(readFile(base.join("out.bin")).length);              // 7
w.close();

// advisory exclusive lock: withLock releases no matter what fn does
const lock = new FileLock(base.join("lock"));
const guarded = lock.withLock(() => readFile(base.join("data.txt")).length);
print(guarded);                                            // 18
lock.close();

// compiled glob: lexical match without touching the FS, then the walk
const g = new Glob("**/*.txt");
print(g.matches(new Path(base, "nested", "x.txt")), g.hasWildcard); // true true
print(g.expand(base).length);                              // 1 (data.txt)

removeAll(base);
```

### Temp files & platform directories

**`tempDir() -> Path`**

The system temp directory (`$TMPDIR`, else `/tmp`) as a `Path`.

**`makeTempDir([prefix]) -> Path`**

- `prefix` *(string, default `"tmp"`)* — the name prefix.

`mkdtemp` under the temp dir, resolved once so the returned `Path` is usable under strict resolution.

**`makeTempFile([prefix]) -> Path`**

- `prefix` *(string)* — the name prefix.

`mkstemp`, closed immediately; returns the path of the empty file.

**`dataDir([app])` / `configDir([app])` / `cacheDir([app]) -> Path`**

- `app` *(string)* — optional; must be a single path segment — no `/`, `\` or NUL, not `.` or `..` — and is appended.

Per-user platform dirs (XDG on Linux, `~/Library` on macOS, `%LOCALAPPDATA%` on Windows). Lookup never creates the directory (XDG spec); an env override wins, a relative one is ignored.

**`dataDirSite([app])` / `configDirSite([app])` / `cacheDirSite([app]) -> Path`**

- `app` *(string)* — optional; as for the per-user variants.

The system-wide variants (`/Library/...` on macOS, first entry of `XDG_DATA_DIRS`/`XDG_CONFIG_DIRS` or `/var/cache` on Linux).

```js
import { Path, tempDir, makeTempDir, makeTempFile, exists, dataDir, cacheDir, remove, removeAll } from "dyna:file";

const td = makeTempDir("dynaprobe-");
print(exists(td));
const tf = makeTempFile("dynaprobe-");
print(exists(tf), String(tempDir()).length > 0);
print(String(dataDir("myapp")).includes("myapp"), String(cacheDir()).length > 0);
removeAll(td);
remove(tf);
```

---

# dyna:stream

Pull-based, buffer-oriented byte streams: caller-owned buffers, no per-chunk allocation, no queuing-strategy machinery. Deliberately **not** WHATWG ReadableStream (of the API plan) — the shape is two small interfaces plus composable adapters, sized for the throughput personas.

`import { pipe, fromBytes, fromFile, toFile } from "dyna:stream";`

The contract, end to end:

- a **ByteSource** `read(buf)` fills *up to* `buf.length` bytes **into the caller's buffer** and resolves with the count actually read; `0` means EOF. Nothing is allocated per chunk.
- a **ByteSink** `write(buf)` accepts bytes and resolves with the count accepted (buffered sinks accept the whole view); `flush()` pushes buffered bytes to the underlying target; `close()` releases (a buffered sink flushes best-effort, so call `flush()` when a write error must be observed).
- both carry the shared native-resource surface — `close()`, `dispose()`, `[Symbol.dispose]`, `closed` — so `using` works.
- **duck typing is the integration contract**: `pipe` only calls `read`/`write`, so any object with compatible methods is a source or sink. Bodies from other modules (fetch responses, spawn output) plug in without this module knowing them.

### ByteSource

Pull-based byte producer. The native implementations come from `fromBytes`/`fromFile`; there is no public constructor.

**`s.read(buf) -> Promise<number>`**

- `buf` *(Uint8Array)* — the caller-owned destination. A zero-length buffer resolves `0` without touching anything.

**`s.close()` / `s.dispose()` / `s[Symbol.dispose]()`** — deterministic release; `s.closed` is `true` afterwards. Any later `read` throws `TypeError`.

### ByteSink

Buffered byte consumer (128 KiB by default). The native implementation comes from `toFile`.

**`w.write(buf) -> Promise<number>`**

- `buf` *(ByteView)* — bytes to accept. Buffered sinks accept the whole view unless the underlying target refuses; a refused write makes the sink **sticky** — every later `write`/`flush` rejects with the same errno until `close()`.

**`w.flush() -> Promise<void>`** — pushes buffered bytes to the target; this is where a buffered write error surfaces.

**`w.close()`** — flushes best-effort (errors unreportable), then releases. `close` alone persists, but `flush()` before `close()` when the error matters.

### pipe

**`pipe(src, dst[, {onChunk}]) -> Promise<number>`**

Reads `src` into a private 128 KiB chunk and writes the filled prefix to `dst` — retrying short writes — until EOF; resolves with the **total** bytes transferred. `onChunk(bytes)` fires after each accepted chunk; a throw rejects the pipe and closes both sides. On **failure** the promise rejects with the cause and both sides are closed (best effort, their close errors swallowed). On **success** neither side is closed — the caller owns both; close the destination sink to persist its buffer.

```js
import { pipe, fromBytes, fromFile, toFile } from "dyna:stream";
import { Path, makeDir, writeFile, readFileAsync, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynastream-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });

// in-memory source -> file sink; the sink close persists the buffered tail
const dst = toFile(new Path(dir, "hello.txt"));
const total = await pipe(fromBytes("hello stream world\n".repeat(100)), dst);
dst.close();
const back = await readFileAsync(new Path(dir, "hello.txt"), { bytes: true });
print(total === 1900, back.length === 1900);

// file -> file, byte-identical through a multi-MB source (bigger than any buffer)
const big = new Path(dir, "big.bin");
const bigBytes = new Uint8Array(3 * 1024 * 1024 + 777);
for (let i = 0; i < bigBytes.length; i++) bigBytes[i] = (i * 7 + (i >> 8)) & 0xff;
writeFile(big, bigBytes);
const copied = await pipe(fromFile(big), toFile(new Path(dir, "big-copy.bin")));
const copy = await readFileAsync(new Path(dir, "big-copy.bin"), { bytes: true });
print(copied === bigBytes.length, copy.length === bigBytes.length);

removeAll(dir);
```

### fromBytes

**`fromBytes(b) -> ByteSource`**

- `b` *(Uint8Array | string)* — the bytes; a string is encoded UTF-8.

The bytes are **copied**, so later mutation of `b` never shows through the source.

### fromFile

**`fromFile(path) -> ByteSource`**

- `path` *(Path | string)* — unlike `dyna:file`, a plain string is accepted alongside `Path`.

Opened **lazily**: nothing touches the disk until the first `read`, so a bad path *rejects the read* (`ENOENT` with a `code`) instead of throwing here, and a source that is only closed never opens anything. Reads are sequential `read(2)`, so non-seekable files (FIFOs) stream as well as regular files.

```js
import { fromFile } from "dyna:stream";
import { Path } from "dyna:file";

const missing = fromFile(new Path(Path.temp(), "definitely-not-here-" + (Date.now() % 100000)));
try { await missing.read(new Uint8Array(16)); }
catch (e) { print(e.code); }  // ENOENT, rejected on the promise
missing.close();              // backstop; nothing was opened
```

### toFile

**`toFile(path[, {append, bufferSize}]) -> ByteSink`**

- `path` *(Path | string)*.
- `append` *(boolean, default `false`)* — append instead of truncate.
- `bufferSize` *(number, default `128 * 1024`)* — clamped to 4 KiB..64 MiB.

Created **lazily**: the file appears at the first `write` (or the first `flush` that has data). `write` resolves once bytes are accepted into the buffer; the fd-level error surfaces on the write that triggers a flush or on `flush()`. `close()` flushes best-effort and persists.

```js
import { toFile } from "dyna:stream";
import { Path, readFile, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynastream2-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });
const log = toFile(new Path(dir, "app.log"));
await log.write(new TextEncoder().encode("one\n"));
await log.write(new TextEncoder().encode("two\n"));
log.close();                                   // best-effort flush + close
print(readFile(new Path(dir, "app.log")));     // one\ntwo\n
removeAll(dir);
```

### lines

**`lines(src[, {encoding}]) -> AsyncIterable<string>`**

Splits a ByteSource into lines (`for await (const line of lines(src))`). Chunk-safe by construction: bytes accumulate until a `"\n"` byte appears, so a line split across **any** read boundary — including a source that yields one byte at a time — is carried intact; a partial multi-byte UTF-8 sequence waits at the buffer end for its continuation (continuation bytes are `>= 0x80`, so the 0x0a split byte can never occur inside one). `"\r\n"` is stripped; empty lines are preserved; a final line without a trailing newline is still delivered. Invalid UTF-8 decodes with U+FFFD replacement. `encoding` accepts only `"utf-8"` (the default) — the one encoding whose carry this splitter can promise — and refuses anything else. `break`ing the loop closes the source.

```js
import { lines, fromBytes } from "dyna:stream";

const out = [];
for await (const line of lines(fromBytes("alpha\nbeta\r\ngamma"))) out.push(line);
print(JSON.stringify(out));  // ["alpha","beta","gamma"]
```

### ndjson

**`ndjson(src) -> AsyncIterable<unknown>`**

Newline-delimited JSON: each line is `JSON.parse`d and yielded. Blank or whitespace-only lines are skipped; a malformed line rejects with a `SyntaxError` naming the 1-based **line number** (`ndjson: line 7: ...`), and the iterator stays usable — the next `next()` resumes after the bad line.

```js
import { ndjson, fromBytes } from "dyna:stream";

const vals = [];
for await (const v of ndjson(fromBytes('{"a":1}\n\n{"b":[2,3]}\n'))) vals.push(v);
print(vals.length, JSON.stringify(vals[1]));  // 2 {"b":[2,3]}
```

### inflate / deflate

**`inflate(src, {codec}) -> ByteSource`** — a decompressing ByteSource over `src`: `read(buf)` fills the caller's buffer with decompressed bytes; `0` = end of stream, reported only after the framing trailers and checksums verify. Closing the wrapper releases it; close the underlying source separately if you own it.

**`deflate(sink, {codec, level}) -> ByteSink & {finish()}** — a compressing ByteSink over `sink`: `write(raw)` accepts raw bytes, compressed bytes flow into `sink` as the codec emits them. TEARDOWN: the stream tail needs an async write, so end a compressing sink with **`await sink.finish()`** — it finalizes the stream, flushes, closes the wrapped sink, and resolves with the total raw bytes compressed. The inherited `close()`/`dispose()` (and `using`) only release the wrapper: an unfinished stream's tail would be lost, so never `using` a compressing sink.

- `codec` — `"gzip"`, `"zstd"`, `"brotli"`, `"lz4"` or `"deflate"` (raw RFC 1951).
- `level` — zstd 1..22 (default 3, clamped), lz4 1..12 (default 1); gzip/deflate/brotli are fixed-quality (accepted and ignored).
- Platform honesty: zstd needs libzstd and brotli needs libcompression/libbrotli — where a codec's library is absent the factory throws a named "not compiled in" error. gzip/deflate stream through libcompression's raw-deflate and are macOS-only in this build. lz4 writes and parses the LZ4 frame format directly (spec checksums; cross-decodes with `dyna:compress` lz4Frame and the `lz4` CLI).

```js
import { deflate, inflate, fromBytes, fromFile, toFile } from "dyna:stream";
import { Path, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "dynastream3-" + (Date.now() % 1000000));
makeDir(dir, { recursive: true });

const raw = new TextEncoder().encode("hello hello hello world. ".repeat(1000));
const ds = deflate(toFile(new Path(dir, "raw.zst")), { codec: "zstd", level: 5 });
await ds.write(raw);
await ds.finish();                      // finalize + flush + close the sink

const src = inflate(fromFile(new Path(dir, "raw.zst")), { codec: "zstd" });
const buf = new Uint8Array(64 * 1024);
let total = 0, n;
while ((n = await src.read(buf)) > 0) total += n;
print(total === raw.length);
removeAll(dir);
```

---

# dyna:sys

Process and environment interface: environment, arguments, working directory, machine facts, and subprocesses.

`import { Exec, Which, env, getEnv, setEnv, args, cwd, chDir, platform, arch, uname, getuid, getgid, setUid, setGid, cpuUsage, rusage, pid, hostName, homeDir, cpuInfo, memInfo, loadAvg, uptime, diskUsage, memoryUsage, setNativeMemoryLimit } from "dyna:sys";`

The filesystem surface lives in `dyna:file`; this module owns only the process/environment surface. Everything here is synchronous. Failed syscalls throw an `Error` with `.errno` and a `.code` name (`ENOENT`, `EACCES`, ...).

### Process & environment

**`env() -> object`**

A snapshot object of the current environment.

**`getEnv(name) -> string | undefined`**

- `name` *(string)* — the variable name.

The value, or `undefined` when unset.

**`setEnv(name, value)`**

- `name` *(string)* — the variable name.
- `value` *(string)* — the new value.

Overwrites an entry. Refuses an empty name, a name containing `=`, or a NUL anywhere: `setenv` stops at the first NUL, so a NUL-bearing name would silently set a truncated key.

**`args() -> string[]`**

The process argument vector (argv[0] first).

**`cwd() -> string`**

Current working directory; grows past `PATH_MAX` rather than truncating.

**`chDir(path)`**

- `path` *(string)* — the directory to change to.

Changes the process working directory.

**`platform() -> string`**

`"darwin"`, `"linux"`, or the lowercased `uname(2)` sysname for the other platforms a build can run on (`"freebsd"`, `"openbsd"`, `"netbsd"`, `"dragonfly"`, `"sunos"`). `"unknown"` only when `uname()` itself fails.

**`arch() -> string`**

The machine hardware name from `uname(2)`, with the two common spellings normalised: `aarch64` → `"arm64"`, `amd64` → `"x86_64"`. Any other machine string (`i386`/`i686`/`riscv64`/...) passes through lowercased.

**`uname() -> object`**

The five standard utsname fields: `{ sysname, nodename, release, version, machine }` — all strings; `nodename` agrees with `hostName()`.

```js
import { platform, arch, uname } from "dyna:sys";

const un = uname();
print(platform() === un.sysname.toLowerCase());
print(arch() === (un.machine === "aarch64" ? "arm64" : un.machine === "amd64" ? "x86_64" : un.machine.toLowerCase()));
print(typeof un.release === "string" && un.release.length > 0);
print(un.nodename.length > 0);
```

### Process identity & CPU time

**`getuid() -> number`**

The real user id.

**`getgid() -> number`**

The real group id.

**`setUid(id)`**

`setuid(2)`. Takes effect only with privilege; an unprivileged change is refused by the kernel and surfaces as a clean `Error` with `.code === "EPERM"` (`.errno === 1`) — never a silent `-1`. `id` must be a non-negative integer (`TypeError` for non-numbers, `RangeError` for negatives).

**`setGid(id)`**

`setgid(2)`, same discipline as `setUid`.

**`cpuUsage() -> object`**

CPU time consumed by this process: `{ user, system }` in fractional seconds, from `getrusage(RUSAGE_SELF)`. Monotonic non-decreasing over the process lifetime.

**`rusage() -> object`**

The full `getrusage(RUSAGE_SELF)` struct as a flat object. `user`/`system` are fractional seconds — exactly the values `cpuUsage()` returns — and `maxrss` is normalised to bytes (it is the same number `memoryUsage().peakRss` reports). The remaining fields: `idrss`/`isrss` (historical shared-memory counters; Linux reports 0), `minflt`/`majflt` (page faults without/with I/O), `nswap`, `inblock`/`oublock` (block I/O), `msgsnd`/`msgrcv` (IPC messages), `nsigs` (signals delivered), `nvcsw`/`nivcsw` (voluntary/involuntary context switches). Counter semantics beyond `user`/`system`/`maxrss` are kernel-specific; portable code should treat them as advisory.

```js
import { cpuUsage, rusage, getuid, getgid, setUid, setGid } from "dyna:sys";

const c = cpuUsage();
print(c.user >= 0, c.system >= 0);
const r = rusage();
print(Math.abs(r.user - c.user) < 0.01, r.maxrss > 0, r.nvcsw >= 0);
print(getuid() >= 0, getgid() >= 0);
try {
    setUid(getuid() === 0 ? 0 : getuid() + 1);
} catch (e) {
    print(e.code === "EPERM");   // unprivileged change refused
}
```

**`pid() -> number`**

The process id.

**`hostName() -> string`**

The host name.

**`homeDir() -> string`**

`$HOME`, else the passwd entry.

```js
import { platform, pid, hostName, cwd, homeDir, getEnv, args, setEnv, env, chDir } from "dyna:sys";

print(platform(), pid() > 0, hostName().length > 0);
print(cwd().startsWith("/"), homeDir().startsWith("/"));
print(getEnv("HOME") === homeDir(), args()[0].length > 0);
setEnv("DYN_SYS_PROBE", "42");
print(getEnv("DYN_SYS_PROBE"), "DYN_SYS_PROBE" in env());
const old = cwd();
chDir("/tmp");
print(cwd());
chDir(old);
```

### Machine facts

**`cpuInfo() -> object`**

Returns `{ model, cores?, threads, mhz?, features }`; `features` lists the SIMD capabilities the dispatcher actually selected (`sse42`, `avx2`, `avx512f`, `neon`, ...). A fact the OS does not publish is absent, never a fabricated zero.

**`memInfo() -> object`**

Returns `{ total, free, available }` in bytes; `available` is what the OS would reclaim under pressure.

**`loadAvg() -> number[]`**

The `[1, 5, 15]`-minute load averages.

**`uptime() -> number`**

Seconds since boot.

**`diskUsage(path) -> object`**

- `path` *(string)* — the path whose filesystem is queried.

Returns `{ total, free, available }` for the filesystem holding `path`; `free` is what exists, `available` is what a non-root caller may take.

**`memoryUsage() -> object`**

The engine's own live allocation counters (`mallocCount`, `mallocSize`, `objCount`, ...) plus the OS `peakRss` in bytes (normalised across platforms), plus the module-native ledger:

- `nativeSize` — libc memory the `dyna:*` modules hold OUTSIDE the JS heap, where the engine's counters cannot follow. This is what `dyna:dataframe` columns, `dyna:structures` contents and `dyna:ml` buffers are made of; the engine's memory limit (`JS_SetMemoryLimit` from C) does not see them. The ledger counts every native resource's box (the `DynResource` header each `dyna:*` resource carries — that box also stays visible to the engine's `mallocSize`, a deliberate ~40-byte double entry) and every allocation modules route through the counted native allocator (`dyna:structures` today). Payload buffers inside modules not yet converted (`dyna:dataframe`, `dyna:ml`, `dyna:http`, ...) are NOT in this number yet — size them against `peakRss`, not `nativeSize`.
- `nativeLimit` — the cap `setNativeMemoryLimit()` installed; `0` means uncapped.

```js
import { cpuInfo, memInfo, loadAvg, uptime, diskUsage, memoryUsage, cwd } from "dyna:sys";

const ci = cpuInfo();
print(ci.model, ci.threads, ci.features.length >= 0);
const mi = memInfo();
print(mi.total > 0, mi.available > 0, mi.free > 0);
print(loadAvg().length === 3, uptime() > 0);
const du = diskUsage(cwd());
print(du.total > 0, du.available > 0, du.free > 0);
print(memoryUsage().mallocCount >= 0, memoryUsage().peakRss > 0);
print(memoryUsage().nativeSize >= 0, memoryUsage().nativeLimit === 0);
```

**`setNativeMemoryLimit(bytes)`**

Caps module-native memory at `bytes`. The engine limit cannot see libc memory a module allocates outside the JS heap, so an embedder who wants one ceiling on the process needs this lever on the other half. An allocation past the cap is REFUSED, not served: the call throws out-of-memory, so a script sees an exception instead of the process growing without bound.

- `bytes` *(number)* — the cap in bytes; `0` removes the cap.

The default is `0` — unlimited, deliberately. Native allocations were never limited before, and a second, independent ceiling switched on by default would start failing scripts that never asked for it (an engine limit is not a native limit; it is a different ledger). Set it explicitly at startup, before running untrusted code. Setting a limit never frees anything — it only gates new allocations — and the cap is absolute: once live module bytes are over the limit, every new native allocation is refused until something is freed, exactly like the engine's limit.

```js
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import { BitSet } from "dyna:structures";

setNativeMemoryLimit(64 * 1024 * 1024);   // 64 MiB of native memory
try {
    const big = new BitSet(1 << 30);      // ~128 MiB of words
} catch (e) {
    print("refused:", e instanceof Error); // refused: true
}
setNativeMemoryLimit(0);                  // uncapped again
```


### Subprocesses

**`Exec(command, args[, options]) -> { code, signal, stdout, stderr, timedOut }`**

- `command` *(string)* — the program to run.
- `args` *(string[])* — the argument vector.
- `cwd` *(string)* — must be a directory.
- `env` *(object)* — an object replacing the environment.
- `input` *(string | byte view)* — fed to stdin.
- `timeoutMs` *(number)* — SIGTERM, then SIGKILL after a 2 s grace.
- `maxBuffer` *(number, default `8 MiB`)* — exceeding it throws rather than truncating.
- `encoding` *(string, default `"utf8"`)* — or `"bytes"` for `Uint8Array` output.

Runs a program with argv, no shell anywhere: a command line is not a string here, so command injection is unrepresentable (a caller wanting a shell writes `Exec("/bin/sh", ["-c", s])`). The parent resolves the program against `PATH` (the child's `PATH` when its environment is replaced), so a missing command is a clear error rather than exit 127. `code` is `null` when a signal killed the child, and `signal` names it. This is the canonical spawn door; `os.exec` (the `--std` QuickJS compat module) is the pid/errno-shaped legacy equivalent.

**`Which(name) -> string | null`**

- `name` *(string)* — the program name.

Resolves a program name against `PATH`; returns the absolute path, or `null` when not found. No shell involved.

**`new Spawn(command, args[, options]) -> Spawn`**

- `command` *(string)* — the program to run.
- `args` *(string[])* — the argument vector.
- `cwd` *(string)* — must be a directory.
- `env` *(object)* — an object replacing the environment.
- `input` *(string | byte view)* — written to the child's stdin, then EOF (implies `stdin: "pipe"`).
- `stdin` *(string, default `"ignore"`)* — `"ignore"` gives the child /dev/null (a child that reads stdin gets EOF and never hangs); `"pipe"` makes `p.stdin` writable; `"inherit"` passes the engine's stdin through.
- `timeoutMs` *(number)* — SIGTERM at the deadline, SIGKILL after the 2 s grace; `wait()` reports `timedOut: true`.
- `maxPipe` *(number, default `8 MiB`)* — the per-pipe drain-ahead bound where backpressure kicks in.
- `uid` / `gid` *(number)* — dropped after the exec-side chdir, gid before uid.
- `allowPathCwd` *(boolean)* — `false` skips empty `PATH` elements (the execvp cwd search).

The ASYNC child process next to the sync `Exec`: the same argv-only, no-shell, NUL-refusing execve discipline (the parent resolves `PATH`), but nothing blocks the JS thread. `stdout` and `stderr` are dyna:stream-compatible ByteSources — `read(buf)` fills up to `buf.length` and resolves with the byte count, `0` at EOF — drained continuously into buffers bounded by `maxPipe`; a full buffer turns into REAL kernel backpressure (the READ watch comes off, the child blocks on write until a `read()` drains room) instead of a kill. `stdin` is a write side: `write(buf)` accepts the whole view into a queue and resolves with its length, `flush()` resolves when the queue reaches the child, `close()` sends EOF after the queue drains; an EPIPE rejects `flush()` and makes later writes refuse. `wait()` resolves `{code, signal, timedOut}` with Exec's conventions (`code` null when a signal killed the child); `kill(sig)` signals the child GROUP (`true` when the child was still running); `close()`/`dispose()` is the deterministic teardown — SIGKILL the group, reap, drop everything — so a Spawn never leaves a zombie, and a script that spawns and never awaits still runs the child to completion before the process exits. One `read()` per pipe and one `wait()` at a time; a stdio view pulled into a local, an awaited `read()` and a pending `wait()` all outlive a dropped Spawn, because each view and each operation parked in C holds a reference until it settles.

```js
import { Exec, Which, Spawn } from "dyna:sys";

const r = Exec("/bin/sh", ["-c", "echo out; echo err >&2"]);
print(r.code, r.stdout.trim(), r.stderr.trim(), r.timedOut);
const rb = Exec("printf", ["%s", "hi"], { encoding: "bytes" });
print(rb.stdout instanceof Uint8Array, rb.stdout.length);
const w = Which("sh");
print(w !== null, w.length > 0);

const p = new Spawn("sh", ["-c", "echo out; echo err >&2"]);
async function readAll(src) {
    const b = new Uint8Array(65536); let out = "";
    for (;;) { const n = await src.read(b); if (n === 0) break; out += new TextDecoder().decode(b.subarray(0, n)); }
    return out;
}
const o = await readAll(p.stdout);
const e = await readAll(p.stderr);
print(o.trim(), e.trim(), (await p.wait()).code);

const f = new Spawn("cat", [], { input: "fed through stdin" });
const buf = new Uint8Array(4096);
let total = 0;
for (;;) { const n = await f.stdout.read(buf); if (n === 0) break; total += n; }
print(total, (await f.wait()).code);

const t = new Spawn("sleep", ["30"], { timeoutMs: 100 });
print((await t.wait()).timedOut, t.exited);
t.close();
```

---

# dyna:bench

The performance persona's first import: native timing via the monotonic clock around repeated calls. Strict options; timeless docs.

`import { bench } from "dyna:bench";`

### bench & table

**`bench(name, fn, { timeMs, warmupMs }?) -> object`**

- `name` *(string)* — the benchmark name (recorded for `table()`).
- `fn` *(function)* — the synchronous function to time; a throw propagates.
- `timeMs` *(number, default `500`, 1..60000)* — measurement budget.
- `warmupMs`/`warmup` *(number, default `100`, 0..60000)* — warmup budget (aliases).

Returns `{ name, iters, elapsedMs, opsPerSec, rsd, p50Ms, p99Ms, meanMs }`: ops/sec from the mean, RSD (std/mean), p50/p99 over per-iteration latencies.

**`table() -> string`**

Tab-separated table of all `bench()` results this process (header + one row per run).

```js
import { bench, table } from "dyna:bench";
const r = bench("noop", () => {}, { timeMs: 20, warmupMs: 5 });
print(r.iters > 0, r.opsPerSec > 0);
print(table().includes("noop"));
```

---

# dyna:async

Concurrency utilities — with the parallelism question answered HONESTLY: every function runs on the event-loop thread. The engine's shared native offload pool executes C functions, not JS, and `os.Worker` takes a module filename whose messages cannot carry functions, so a user fn cannot cross to a worker thread. What the module delivers is CONCURRENCY-limited interleaving (`pmap`/`Pool` never run more than the given number of fns at once; async fns interleave at their awaits), bounded handoff (`Channel`, `Queue`, `Semaphore`), and clock-driven combinators (`withTimeout`, `retry`, `debounce`, `throttle`). Argument refusals throw synchronously, before anything is scheduled.

`import { pmap, Pool, Channel, Queue, Semaphore, sleep, withTimeout, retry, debounce, throttle } from "dyna:async";`

### sleep / withTimeout / retry

**`sleep(ms) -> Promise<void>`** — resolves after `ms` ms.

**`withTimeout(p, ms[, reason]) -> Promise`** — rejects if `p` does not settle within `ms`, otherwise adopts `p`'s outcome. The underlying promise is handled even when the timer wins, so a losing promise's late rejection never surfaces as unhandled, and the timer is cleared on settle. `reason` replaces the error message.

**`retry(fn[, opts]) -> Promise`** — calls `fn` until it settles or the retries run out: 1 initial attempt plus `retries` (default 3), waiting `backoffMs * factor^attempt` (defaults 0 ms and 2) between tries; `onRetry(error, attempt)` observes each retry. The LAST error surfaces.

### Channel / Queue / Semaphore

**`new Channel([cap])`** — MPSC: many senders, one consumer. `send(v)` resolves when accepted (immediately under `cap`, else when room appears — backpressure, never a silent drop); `recv()` resolves `{value, done: false}`, then — after `close()` AND a full drain — `{done: true}`. `close()` resolves a parked `recv()` with done, and REJECTS a send that was still blocked on capacity (its value was never accepted). A second concurrent `recv()` is refused (one consumer). Works with `for await` (which binds the yielded value; a closed channel ends the loop after the drain).

**`new Queue([cap])`** — the synchronous bounded FIFO: `push` throws when closed or full, `tryPush` returns false, `shift` returns `undefined` when empty, `close()` freezes it.

**`new Semaphore(n)`** — counting semaphore with DIRECT token hand-off: a release passes the token straight to the next waiter, so a woken waiter cannot be jumped by a newcomer. `acquire()` resolves with a release function; `run(fn)` wraps acquire/try/release.

### pmap / Pool

**`pmap(items, fn[, {concurrency}]) -> Promise<R[]>`** — maps `fn` over `items` with at most `concurrency` (default 8) invocations in flight, results in ITEM order regardless of completion order. The FIRST rejection rejects the whole pmap; in-flight items still run to completion and settle, their late results dropped and their late rejections handled — nothing surfaces twice and nothing is left dangling.

**`new Pool(n)`** — the same honest interleave as `pmap`, shaped as a long-lived scheduler: `submit(fn)` runs `fn` when a slot frees; a throwing job rejects its submit, frees its slot, and the pool keeps pumping. `size`/`active`/`pending` report the state.

### debounce / throttle

**`debounce(fn, ms)`** — trailing: the LAST call inside the window runs once at its edge. **`throttle(fn, ms)`** — leading with a trailing call: the first call in the window runs now, the last runs at the edge. Both return `fn`-wrappers with `cancel()` and `pending()`.

```js
import { pmap, Pool, Channel, Semaphore, withTimeout, retry, sleep } from "dyna:async";

const urls = ["a", "b", "c", "d", "e"];
const pages = await pmap(urls, async (u) => "page:" + u, { concurrency: 2 });
print(JSON.stringify(pages));                       // ["page:a","page:b","page:c","page:d","page:e"]

const sem = new Semaphore(2);
const loads = await Promise.all(urls.map((u) => sem.run(async () => u.toUpperCase())));
print(loads.length, sem.available);                 // 5 2

const ch = new Channel(1);
await ch.send(1);
const next = ch.send(2);                            // blocked: capacity
print(JSON.stringify(await ch.recv()));             // {"value":1,"done":false}
await next;
ch.close();
print(JSON.stringify(await ch.recv()));             // {"done":true}

const r = await retry(async () => Math.random() < 0.5 ? Promise.reject(new Error("flip")) : "heads",
                      { retries: 5, backoffMs: 1 });
print(r === "heads");
print(await withTimeout(sleep(5).then(() => "made it"), 1000));

const pool = new Pool(2);
print(JSON.stringify(await Promise.all([pool.submit(() => 1), pool.submit(async () => 2)])));
```

---

# dyna:uring

Linux-only disk I/O through io_uring, loadable only in builds made with `CONFIG_IO_URING` on Linux. The reader submits many block reads (256 KiB blocks at queue depth 64) and reaps their completions, so a large file is fetched in one submit/complete cycle per batch instead of one blocking `pread` per block. Correctness is proven against a pread reference: identical bytes, identical checksum. Four plain functions, no classes — `readFile`, `readFileSync`, `readFileBytes` and `checksum`.

Every entry point takes a `Path`, never a string. The `Path` class is owned by `dyna:file` and borrowed here through the single shared class id, so any `Path` built by `dyna:file` is accepted. A string (or any non-`Path`) argument is refused with a `TypeError` naming the parameter.

`import { readFile, readFileSync, readFileBytes, checksum } from "dyna:uring";`

### readFile

**`readFile(path) -> string`**

- `path` *(Path)* — the file to read.

Reads the entire file through the io_uring bulk reader and returns its contents as a string; an empty file returns `""`. The whole file is materialised in memory at once, so the practical bound is addressable memory. The path must name a regular file; a directory, device, FIFO or other non-regular path is refused. The string is the raw bytes decoded as UTF-8 with invalid sequences replaced, so binary files come back mangled — use `dyna:file`'s byte-returning reader (`readFileAsync` with `{ bytes: true }`) for binary content.

### readFileSync

**`readFileSync(path) -> string`**

- `path` *(Path)* — the file to read.

The blocking `pread(2)` reference reader: the same whole-file string with the same bounds and refusals. This is the correctness oracle and baseline the io_uring path is measured against, and it is the only reader that works on a host where io_uring itself is unavailable.

### readFileBytes

**`readFileBytes(path) -> Uint8Array`**

- `path` *(Path)* — the file to read.

The byte variant of the bulk reader. The read is identical to `readFile` — same io_uring path, same bounds, same refusals, an empty file gives a zero-length `Uint8Array` — but the handoff to JS is a fresh `Uint8Array` over the exact bytes: no UTF-8 decode, so binary files, files with a cut multi-byte tail and files whose contents are not text at all come back byte-exact, which the string form cannot promise. The array owns a copy; a later write to it never re-enters the file. Like `readFile` it takes the io_uring path, so on a host where io_uring is unavailable it refuses (use `dyna:file`'s byte readers there).

```js
/* self-guarding: dyna:uring only loads on Linux CONFIG_IO_URING builds */
import { Path } from "dyna:file";
const m = await import("dyna:uring").catch(() => null);
if (!m) {
    print("dyna:uring not built here");
} else {
    const raw = m.readFileBytes(new Path("/bin/sh"));
    print(raw instanceof Uint8Array, raw[0] === 0x7f, raw[1] === 0x45); // true true true
}
```

### checksum

**`checksum(path[, useUring]) -> object`**

- `path` *(Path)* — the file to read.
- `useUring` *(boolean, default `true`)* — `true` reads through the io_uring bulk reader, `false` through the pread reference. Any value is coerced with the usual truthiness rules.

Reads the whole file and returns `{ bytes, sum }`: `bytes` is the file length, `sum` is a 32-bit FNV-1a rolling checksum over the raw bytes (an unsigned integer in `[0, 2^32 - 1]`). The function exists to force every byte to be touched without materialising a giant JS string, and the two backends are the differential: identical `sum` for both proves the io_uring reader byte-correct.

All four functions refuse in the same two ways: a non-`Path` argument throws a `TypeError` (`"path must be a Path -- wrap it with new Path(...) from dyna:file"`, or `"…but dyna:file is not built in"` in builds without the file module); and a read that cannot complete throws `InternalError("dyna:uring: read failed")` — this covers an unopenable path, a non-regular file, an out-of-memory read, and, for the io_uring reader specifically, a kernel or emulation that does not implement io_uring (for example `io_uring_queue_init` returning `ENOSYS` under qemu emulation, or a seccomp profile filtering the setup syscall). `readFileSync` and `checksum(path, false)` never touch io_uring and are unaffected by that last case.

---

# dyna:time

Durations, clocks, RFC 3339 and layout-based formatting, recurrence rules, and civil-calendar value types.

`import { Duration, PlainDate, PlainDateTime, PlainTime, RRule, Format, DateParser, parseDuration, parseDurationMs, parseDurationSecs, durationMs, durationSecs, durationString, now, nowSec, nowMillis, nowUnixNano, nowNanos, monotonicNano, formatRFC3339, formatUnix, parseRFC3339, date, fromUnix, parseDate, dateFromEpochDay, parseTime, toPlainDateTime } from "dyna:time";`

A `Duration` is an integer nanosecond count. `parseDuration` returns a `Number` when the magnitude is a safe integer and a `BigInt` otherwise, so sub-day values still compare with `===`; `monotonicNano()` differences exceed `2^53` at ~104 days, so `BigInt` is the normal case. The unit-named twins (`parseDurationMs`, `parseDurationSecs`) and the scalar builders (`durationMs`, `durationSecs`) say their unit in the name.

Which clock when — the engine's time sources:

| Clock | Kind | Unit/shape | Use for |
|---|---|---|---|
| `time.now()` | wall | `{sec, nsec}` split | second-precision timestamps, RFC 3339 formatting |
| `time.nowSec()` | wall | number whole seconds | unix seconds without the sub-second half (`now().sec`) |
| `time.nowUnixNano()` | wall | BigInt ns since the Unix epoch | exact elapsed math, cross-process timestamps |
| `time.nowNanos()` | wall | BigInt ns since the Unix epoch | alias of `nowUnixNano` (the duration-side spelling) |
| `time.nowMillis()` | wall | number ms | the `Date.now()` equivalent off the same clock |
| `time.monotonicNano()` | monotonic | BigInt ns, arbitrary origin | measuring durations; never compared across processes |
| `Date.now()` / `performance.now()` | wall / monotonic | number ms | web-compat shims (same truths, less precision) |

(`os.now` under `--std` is an ms clock with an arbitrary origin — see the compatibility table at the top of this reference.)

### Duration

**`parseDuration(str) -> number | BigInt`**

- `str` *(string)* — the duration text.

`"300ms"`, `"-1.5h"`, `"2h45m"`, `"0"`; units `ns us µs μs ms s m h` (NANOSECONDS are the result unit — there is no `d`). The result is nanoseconds as a `Number` while the magnitude stays below 2^53, and a `BigInt` from 2^53 ns up (`parseDuration("200000h")` is `720000000000000000n`); a value that overflows int64 nanoseconds — like any malformed input — throws `SyntaxError` (`"dyna:time: invalid duration"`).

**`parseDurationMs(str) -> number`**

The same parse, delivered in **milliseconds** and always a `Number`. The ns parse is exact (int64 end to end); the conversion is one IEEE division, so the result is the correctly-rounded double of `ns/1e6` — exact whenever that quotient is a safe integer, which holds far past the point where `parseDuration` itself had to switch to `BigInt` (`parseDurationMs("200000h")` is `7.2e11` exactly). Past 2^53 **in the result unit** the usual double rounding applies; `parseDuration` stays the exact path. Same grammar, same `SyntaxError`.

**`parseDurationSecs(str) -> number`**

The same precision rule with `ns/1e9`.

**`durationString(ns) -> string`**

- `ns` *(number | BigInt)* — the nanosecond count.

The inverse of `parseDuration`: largest unit first, fractions trimmed of trailing zeros. `0` is `"0s"`; below 1 s a single unit is chosen by magnitude.

**`Nanosecond` / `Microsecond` / `Millisecond` / `Second` / `Minute` / `Hour` -> number**

Module constants in nanoseconds.

```js
import { parseDuration, parseDurationMs, parseDurationSecs, durationString, Nanosecond, Microsecond, Millisecond, Second, Minute, Hour } from "dyna:time";

print(parseDuration("1.5h") === 5400000000000);
print(parseDuration("300ms"), typeof parseDuration("300ms"));
print(parseDurationMs("300ms"), parseDurationSecs("1.5h"));
print(parseDurationMs("200000h") === 7.2e11, parseDuration("200000h"));
print(durationString(5400000000000), durationString(1500000000), durationString(0));
print(Nanosecond, Millisecond, Second, Minute, Hour);
```

### Duration (class)

Constructed from an options object; years fold into months and weeks into days (both exact), while days never fold into hours — a month and a day are deliberately not collapsed into a fixed number of hours.

**`new Duration({ years, months, weeks, days, hours, minutes, seconds, milliseconds })`**

- `options` *(object)* — required; all fields optional.

**`durationMs(ms) -> Duration`** / **`durationSecs(s) -> Duration`**

- `ms` *(number)* — milliseconds.
- `s` *(number)* — seconds.

The scalar builders: `durationMs(ms)` is exactly `new Duration({ milliseconds: ms })`, `durationSecs(s)` exactly `new Duration({ seconds: s })` — the constructor's field semantics, so values are integral (`NaN`/`Infinity` coerce to 0, non-integers truncate toward zero) and negative is allowed. `durationSecs` folds seconds into the millisecond field the way the constructor does, so fractional input truncates to whole seconds FIRST: `durationSecs(1.9)` is 1s, not 1.9s — pass `durationMs` for sub-second precision. A value whose millisecond fold would leave int64 throws `RangeError`.

**`d.years` / `d.months -> number`**

The month count split as `months / 12` and the rest.

**`d.days -> number`**

The day count.

**`d.sign -> number`**

`1`, `-1` or `0`.

**`d.blank -> boolean`**

`true` when every component is zero.

**`d.toString() -> string`**

ISO 8601 (`"PT1H30M"`, `"P1M2DT5S"`); a mixed-sign value has no ISO representation and throws rather than emitting a parseable wrong answer.

```js
import { Duration, durationMs, durationSecs } from "dyna:time";

const d = new Duration({ hours: 1, minutes: 30 });
print(String(d), d.sign, d.blank, d.years, d.months, d.days);
const m = new Duration({ months: 14, days: 3 });
print(String(m), m.years, m.months, m.days);
print(String(durationMs(1500)), String(durationSecs(90)), String(durationSecs(-1.9)));
print(durationMs(1500).blank, durationSecs(0).blank);
```

### Clocks

**`now() -> { sec, nsec }`**

`{ sec, nsec }` from `CLOCK_REALTIME`.

**`nowSec() -> number`**

Whole seconds since the epoch — the `sec` half of `now()` as one number, for callers whose resolution is a second.

**`nowUnixNano() -> BigInt`**

Nanoseconds since the Unix epoch.

**`nowNanos() -> BigInt`**

Alias of `nowUnixNano` — the same clock under the duration-side name.

**`nowMillis() -> number`**

Milliseconds since the epoch.

**`monotonicNano() -> BigInt`**

Nanoseconds from `CLOCK_MONOTONIC`; differences are the right measure for intervals.

```js
import { monotonicNano, now, nowSec, nowUnixNano, nowNanos, nowMillis } from "dyna:time";

const t0 = monotonicNano();
print(now().sec > 0, nowSec() > 0, nowUnixNano() > 0, nowMillis() > 0);
print(typeof nowNanos() === "bigint");
print(monotonicNano() - t0 >= 0n);
```

### Formatting & parsing

**`formatRFC3339(sec[, nsec[, utc]]) -> string`** / **`formatRFC3339(sec, { nsec, offsetMinutes }) -> string`**

- `sec` *(number)* — unix seconds; a whole number.
- `nsec` *(number)* — optional; must be a whole number in `[0, 999999999]`, emitted only when non-zero (trailing fraction zeros are trimmed).
- `utc` *(boolean, default `true`)* — when `false`, the local offset is appended.
- `offsetMinutes` *(number)* — render at a FIXED UTC offset in `-1439..1439` (RFC 3339's `+/-23:59` bound, the same range `parseRFC3339` accepts): `"Z"` at zero, `"+HH:MM"`/`"-HH:MM"` otherwise — exactly the shapes `parseRFC3339` reads back.

Every one of those slots is typed as declared and never coerced: a non-number (`"5"`, `true`, `null`, an object, a BigInt) throws `TypeError`, a non-integer number (`2.5`, `NaN`, `Infinity`) throws `RangeError`, and a whole number with no int64 representation (`1e300`) throws `RangeError` "out of range" — `sec`, `nsec` and `offsetMinutes` alike; `utc` takes a boolean or throws `TypeError`. The second argument is the one overloaded slot: an **object** there is the options bag (that is how the bag form is spelled), so `formatRFC3339(sec, {})` is the empty bag and an object is never read as a legacy `nsec`; every other value is the legacy `nsec` and refuses exactly what the bag's `nsec` refuses.

The bag form is STRICT: the valid keys are `nsec` and `offsetMinutes`, and an unknown key throws a `TypeError` naming the key and the valid set. The bag form is EXACTLY two arguments — a third positional argument throws `TypeError` instead of being silently ignored (the `(nsec, utc)` tail belongs to the legacy overload only).

`"2026-08-17T10:30:00Z"`; `formatRFC3339(0, { offsetMinutes: 330 })` is `"1970-01-01T05:30:00+05:30"`.

**`formatUnix(sec, layout[, { offsetMinutes }]) -> string`**

- `sec` *(number)* — unix seconds.
- `layout` *(string)* — Go-style layout tokens.
- `offsetMinutes` *(number)* — the strict bag `{ offsetMinutes }`: the fixed UTC offset the fields render at (default 0). Typed as declared: a non-number throws `TypeError`, a fraction or NaN `RangeError` "must be an integer", a whole number outside `-1439..1439` `RangeError` — the same reader `formatRFC3339` uses.

Tokens: `2006` (year), `Jan`, `Mon`, `01` (month), `02` (day), `15` (hour), `04` (minute), `05` (second), and 's `%z` (the UTC offset: `"Z"` at zero, `"+HH:MM"`/`"-HH:MM"` otherwise); anything else is literal — a `%` that is not `%z` is a literal `%`. The options bag must be an object when given (`null`/`undefined` mean no options); a string or number in the options position throws `TypeError` instead of being silently ignored.

**`parseRFC3339(str) -> { sec, nsec }`**

- `str` *(string)* — the timestamp text.

Strict; accepts `"YYYY-MM-DDTHH:MM:SS[.fraction](Z|±HH:MM)"` with valid calendar fields (`se == 60` tolerated as a leap-second literal).

**`date(y, mo, d[, h[, mi[, s]]]) -> number`**

- `y` *(number)* — the year.
- `mo` *(number)* — the month; an out-of-range month carries into the year (`month 13` is next January).
- `d` *(number)* — the day.
- `h` *(number)* — optional hour.
- `mi` *(number)* — optional minute.
- `s` *(number)* — optional second.

Unix seconds (UTC).

**`fromUnix(sec) -> object`**

- `sec` *(number)* — unix seconds.

Returns `{ year, month, day, hour, min, sec, weekday, yday }` (weekday 0 = Sunday).

```js
import { formatRFC3339, formatUnix, parseRFC3339, date, fromUnix } from "dyna:time";

print(formatRFC3339(0), formatRFC3339(0, 500000000));
print(formatUnix(0, "2006-01-02 15:04:05 Mon Jan"));
print(JSON.stringify(parseRFC3339("2026-08-17T10:30:00Z")));
print(date(2026, 8, 17, 10, 30, 0) === parseRFC3339("2026-08-17T10:30:00Z").sec);
print(fromUnix(0).year, fromUnix(0).month, fromUnix(0).weekday);
```

### Format

A compiled layout: the constructor tokenises the layout once, and one instance formats any number of times without re-scanning. Read-only and freely reusable.

**`new Format(layout[, { offsetMinutes }])`**

- `layout` *(string)* — the layout; requires a string. Tokens as `formatUnix`, `%z` included.
- `offsetMinutes` *(number)* — the strict bag `{ offsetMinutes }`: the fixed UTC offset the fields and `%z` render at (default 0).

The options bag must be an object when given (`null`/`undefined` mean no options); a string or number in the options position throws `TypeError` instead of being silently ignored.

**`f.format(sec) -> string`**

- `sec` *(number)* — unix seconds.

Formats unix seconds at the instance's offset.

**`f.parse(str) -> number`**

- `str` *(string)* — the text to parse.

The inverse, strict: every literal must match byte-for-byte and every field must be exactly as wide as the format emits, so `format`/`parse` round-trip. A field the layout omits takes its value from `1970-01-01T00:00:00Z`. Throws `SyntaxError` on any mismatch.: fields are read AT the instance's `offsetMinutes` unless the input supplies one through `%z` (`"Z"`, or `"+HH:MM"`/`"-HH:MM"` within RFC 3339's bound), which wins — and two `%z` tokens that disagree throw. `%z` at format time emits the instance's offset, so a `%z` layout round-trips at every offset.

**`f.layout -> string`**

The original layout.

```js
import { Format, date } from "dyna:time";

const f = new Format("2006-01-02");
const ts = date(2026, 8, 17);
print(f.format(ts), f.layout, f.parse("2026-08-17") === ts);
```

### PlainDate

An immutable calendar date, proleptic Gregorian, exact integer arithmetic with no time zone. Compared by content.

**`new PlainDate(year, month, day)`**

- `year` *(number)* — in `-271821..275760`.
- `month` *(number)* — 1-based.
- `day` *(number)* — 1-based.

All three are required. An impossible date is refused, never rolled over: `31 February` throws.

**`d.year` / `d.month` / `d.day -> number`**

Fields.

**`d.dayOfWeek -> number`**

ISO 8601, Monday is `1`.

**`d.dayOfYear -> number`**

1-based.

**`d.daysInMonth` / `d.daysInYear` / `d.inLeapYear`**

Calendar facts.

**`d.epochDay -> number`**

Days since `1970-01-01`.

**`d.add(duration)` / `d.subtract(duration) -> PlainDate`**

- `duration` *(Duration)* — the amount to move.

Months move first and clamp to the end of the target month (`31 Jan + 1 month` is `28 Feb`), then days are added exactly. Order is part of the contract.

**`d.until(other) -> Duration`**

- `other` *(PlainDate)* — the later date.

A `Duration` of whole months plus the remaining days.

**`d.compare(other) -> number`**

- `other` *(PlainDate)* — the date to compare against.

`-1`, `0` or `1`.

**`d.toString() -> string`**

ISO 8601; years outside `0..9999` print signed with six digits.

**`parseDate("YYYY-MM-DD") -> PlainDate`**

- `str` *(string)* — the date text.

Strict ISO parse, no locale permissiveness.

**`dateFromEpochDay(n) -> PlainDate`**

- `n` *(number)* — the day count.

A `PlainDate` from a day count.

```js
import { PlainDate, Duration, parseDate, dateFromEpochDay } from "dyna:time";

const pd = new PlainDate(2026, 8, 17);
print(String(pd), pd.dayOfWeek, pd.dayOfYear, pd.daysInMonth, pd.epochDay);
print(String(pd.add(new Duration({ days: 30 }))));
print(String(pd.subtract(new Duration({ months: 1 }))));
print(String(pd.until(new PlainDate(2027, 1, 1))));
print(pd.compare(new PlainDate(2026, 1, 1)), pd.compare(pd));
print(String(parseDate("2026-08-17")), String(dateFromEpochDay(0)));
```

### PlainDateTime

A date and time of day with no zone. Adding time **carries into the date** — `23:30 + 2h` is `01:30` the next day — which is the whole difference from `PlainTime`.

**`new PlainDateTime(year, month, day[, hour[, minute[, second[, millisecond]]]])`**

- `year` *(number)* — required.
- `month` *(number)* — required.
- `day` *(number)* — required.
- `hour` *(number)* — `0..23`; out of range throws.
- `minute` *(number)* — out of range throws.
- `second` *(number)* — out of range throws.
- `millisecond` *(number)* — out of range throws.

The date is required.

**`dt.year` / `dt.month` / `dt.day` / `dt.hour` / `dt.minute` / `dt.second` / `dt.millisecond -> number`**

Fields.

**`dt.epochDay` / `dt.dayOfWeek`**

Date facts.

**`dt.add(duration)` / `dt.subtract(duration) -> PlainDateTime`**

- `duration` *(Duration)* — the amount to move.

Months and days move the date (clamped exactly as `PlainDate`), then the time is added and its overflow carries into the day.

**`dt.until(other) -> Duration`**

- `other` *(PlainDateTime)* — the later date-time.

The `PlainDate.until` fold on the date parts — whole months first, the day-of-month clamp applied (`31 Jan 23:30` to `1 Mar 00:30` in a leap year is 1 month + 1 hour, not 0 months + 28.5 days), then the remaining days — with the time-of-day remainder folded into the Duration's millisecond component. Days count whole 24-hour periods **from the shifted, day-clamped point** (exactly the point `add` resumes from), not calendar dates: `b`'s earlier time of day costs a day. That makes `a.add(a.until(b))` equal `b` exactly, for every pair. Two documented consequences:

- A result may be **mixed-sign** (months > 0 with a negative ms remainder) when `other` falls on the very day the month shift lands on but earlier in it (`10 Jan 15:00` → `10 Feb 09:00` is `1 month, −6h`). The value is correct under `add`/`subtract`, but `toString()` refuses it, as it refuses every mixed-sign `Duration`.
- Exact component negation between `a.until(b)` and `b.until(a)` is guaranteed only while both stay inside one calendar month; across a month boundary the two folds clamp different days (`PlainDate.until` has the same property).

**`dt.toPlainDate()` / `dt.toPlainTime()`**

The two halves.

**`toPlainDateTime(date, time) -> PlainDateTime`**

- `date` *(PlainDate)* — the date half.
- `time` *(PlainTime)* — the time half.

The joiner for the two splitters: a pure field copy (the date and the ms-since-midnight land in a `PlainDateTime` unchanged), so `toPlainDateTime(dt.toPlainDate(), dt.toPlainTime())` equals `dt` for every `dt`. Both arguments must be the right class — a non-`PlainDate`/non-`PlainTime` argument throws `TypeError`.

**`dt.compare(other) -> number`**

- `other` *(PlainDateTime)* — the value to compare against.

`-1`, `0` or `1`.

**`dt.toString() -> string`**

ISO 8601 with `T`, milliseconds omitted when zero.

```js
import { PlainDateTime, PlainDate, PlainTime, Duration, toPlainDateTime } from "dyna:time";

const dt = new PlainDateTime(2026, 8, 17, 23, 30);
print(String(dt), dt.hour, dt.minute, dt.epochDay);
print(String(dt.add(new Duration({ hours: 2 }))));
print(String(dt.toPlainDate()), String(dt.toPlainTime()));
print(dt.compare(new PlainDateTime(2026, 8, 17, 0, 0)));
print(String(toPlainDateTime(dt.toPlainDate(), dt.toPlainTime())) === String(dt));
print(String(toPlainDateTime(new PlainDate(2026, 8, 17), new PlainTime(9, 5))));
print(String(new PlainDateTime(2026, 8, 17, 23, 30).until(new PlainDateTime(2026, 8, 18, 1, 30))));
```

### PlainTime

A wall-clock time of day, no date, no zone — one integer millisecond count since midnight, so comparison is an integer compare.

**`new PlainTime([hour[, minute[, second[, millisecond]]]])`**

- `hour` *(number)* — optional.
- `minute` *(number)* — optional.
- `second` *(number)* — optional.
- `millisecond` *(number)* — optional.

`24:00` is refused (it names the same wall clock as `00:00`).

**`t.hour` / `t.minute` / `t.second` / `t.millisecond -> number`**

Fields.

**`t.msSinceMidnight -> number`**

The whole value.

**`t.add(duration)` / `t.subtract(duration) -> PlainTime`**

- `duration` *(Duration)* — the amount to move.

Wrap at midnight; a duration in months is refused (a month has no meaning for a time of day).

**`t.compare(other) -> number`**

- `other` *(PlainTime)* — the value to compare against.

`-1`, `0` or `1`.

**`t.toString() -> string`**

`"HH:MM:SS"`, milliseconds appended when non-zero.

**`parseTime("HH:MM[:SS[.mmm]]") -> PlainTime`**

- `str` *(string)* — the time text.

Strict parse; out-of-range fields throw `RangeError`, malformed input `SyntaxError`.

```js
import { PlainTime, Duration, parseTime } from "dyna:time";

const t = new PlainTime(13, 45, 30, 250);
print(String(t), t.hour, t.msSinceMidnight);
print(String(t.add(new Duration({ minutes: 30 }))));
print(String(parseTime("13:45:30.250")));
```

### RRule

**STRICT options:** `fromString`'s bag is `{dtstart}` only; unknown keys throw (a typo'd `dtstart` used to anchor the rule to its own default start).

RFC 5545 recurrence rules, semantics ported field-for-field from python-dateutil (validated against pinned dateutil). Everything is UTC whole-second unix time; `dtstart` fixes the time of day of every occurrence.

**`new RRule({ freq, interval?, count?, until?, dtstart?, wkst?, bymonth?, bymonthday?, byyearday?, byweekno?, bysetpos?, byweekday? })`**

- `freq` *(string)* — required: `YEARLY|MONTHLY|WEEKLY|DAILY|HOURLY|MINUTELY|SECONDLY`.
- `interval` *(number)* — optional.
- `count` *(number)* — optional.
- `until` *(Date | ISO string | unix seconds)* — optional; NaN and out-of-range values are rejected, never cast.
- `dtstart` *(Date | ISO string | unix seconds)* — optional; fixes the time of day of every occurrence.
- `wkst` — optional.
- `bymonth` — optional.
- `bymonthday` — optional.
- `byyearday` — optional.
- `byweekno` — optional.
- `bysetpos` — optional.
- `byweekday` *(string | number)* — items are strings (`"MO"`, `"1MO"`, `"-1FR"`) or numbers `0..6` (`0=MO`).

`BYHOUR`/`BYMINUTE`/`BYSECOND`/`BYEASTER` are refused rather than silently ignored. Years are validated into `0001..9999`.

**`RRule.fromString(str[, { dtstart }]) -> RRule`**

- `str` *(string)* — the rule text.
- `dtstart` *(Date | ISO string | unix seconds)* — optional; the rule start.

Parses `"RRULE:FREQ=..."` parts plus optional `DTSTART:` lines; parts are order-independent and empty `by*` arrays count as absent.

**`r.all([limit]) -> Date[]`**

- `limit` *(number)* — optional cap on the result count.

Every occurrence as `Date`s. Without `COUNT`, `UNTIL` or an explicit `limit` an infinite rule refuses with `RangeError`. Bounds: at most 1,000,000 periods scanned per call and 1,000,000 results — a rule that never matches returns `[]` instead of spinning, and one producing more throws rather than growing an unbounded array.

**`r.between(start, end[, inc]) -> Date[]`**

- `start` *(Date)* — window start.
- `end` *(Date)* — window end.
- `inc` *(boolean)* — makes both ends inclusive.

Occurrences in the window. An uncounted rule jumps its cursor to the window instead of walking every prior period.

**`r.next([fromDate]) -> Date | null`**

- `fromDate` *(Date)* — optional; defaults to `dtstart`.

The first occurrence strictly after `fromDate`, or `null`.

**`r.prev([fromDate]) -> Date | null`**

- `fromDate` *(Date)* — optional.

The last occurrence strictly before `fromDate`; with no argument it is the rule's last occurrence, `null` for an infinite rule.

**`r.toString() -> string`**

The rule back in RFC 5545 text form, round-trippable.

```js
import { RRule } from "dyna:time";

const rrule = new RRule({
    freq: "DAILY",
    count: 5,
    dtstart: new Date(2026, 7, 17, 0, 0, 0)
});
const occurrences = rrule.all();
print(occurrences.length, String(rrule));
const weekly = RRule.fromString("FREQ=WEEKLY;BYDAY=MO,WE;COUNT=3", {
    dtstart: new Date(2026, 0, 5, 9, 0, 0)
});
print(weekly.all().length, String(weekly));
print(rrule.between(new Date(0), new Date(2026, 8, 1)).length);
print(rrule.next(new Date(2026, 7, 18)) !== null, rrule.prev(new Date(2026, 9, 1)) !== null);
```

### DateParser

Natural-language date parsing per locale. The locale decides the numeric order — `"03/04/2026"` is 4 March in `en-GB` and 3 April in `en-US` — and supplies the month/weekday name tables, built in the constructor.

**`new DateParser([locale[, { now }]])`**

- `locale` *(string, default `"en-US"`)* — one of `en-US en-GB en fr de es`.
- `now` *(number)* — unix seconds; makes relative words deterministic. **STRICT:** `{now}` is the whole bag; unknown keys throw a `TypeError` naming the key (a typo used to silently race the real clock).

**`p.parse(text) -> number | null`**

- `text` *(string)* — the text to parse.

Unix seconds, or `null` when nothing matches (a human-typed string failing to parse is an ordinary outcome, not an error). Accepts ISO dates, `"3 April 2026"`, `"28/07/2026"` in the locale's order, times with `am`/`pm`, and relative forms `"now"`, `"today"`, `"tomorrow"`, `"yesterday"`, `"in 3 days"`, `"2 weeks ago"`, `"next monday"`, `"last friday"`. Two-digit years use the POSIX window (69 and below are 2000s).

**`p.locale` / `p.dayFirst`**

The resolved locale name and its numeric-day-first flag.

```js
import { DateParser, date, fromUnix } from "dyna:time";

const dp = new DateParser("en-GB", { now: date(2026, 8, 17) });
print(dp.locale, dp.dayFirst);
print(fromUnix(dp.parse("3 April 2026")).month);
print(fromUnix(dp.parse("tomorrow")).day);
print(fromUnix(dp.parse("in 3 days")).day);
print(dp.parse("not a date"));
const us = new DateParser("en-US");
print(us.dayFirst);
```

Showcase — scheduling: a recurring meeting, its occurrences, and calendar math on top:

```js
import { RRule, PlainDate, PlainDateTime, PlainTime, Duration, Format, parseDuration, durationString, parseRFC3339, formatRFC3339 } from "dyna:time";

// RFC 5545: weekly Tuesdays at 10:00, four occurrences
const rule = new RRule({
    freq: "WEEKLY",
    byday: ["TU"],
    count: 4,
    dtstart: new Date(Date.UTC(2026, 7, 18, 10, 0, 0))
});
const dates = rule.all();
print(dates.length, dates[0].toISOString().slice(0, 10), dates[1].toISOString().slice(0, 10));
// 4 2026-08-18 2026-08-25
print(rule.between(new Date(Date.UTC(2026, 7, 18)), new Date(Date.UTC(2026, 8, 8))).length); // 3
print(rule.next(new Date(Date.UTC(2026, 7, 18))).toISOString().slice(0, 10)); // 2026-08-18 (next after midnight is the 10:00 occurrence)
print(String(rule));                                        // RRULE:FREQ=WEEKLY;COUNT=4

// civil calendar: exact integer arithmetic, clamp not rollover
const pd = new PlainDate(2026, 8, 18);
print(String(pd), pd.dayOfWeek, pd.epochDay);               // 2026-08-18 2 20683
print(String(pd.add(new Duration({ months: 1, days: 5 })))); // 2026-09-23
print(String(new PlainDate(2026, 1, 31).add(new Duration({ months: 1 })))); // 2026-02-28 (clamped)
print(String(pd.until(new PlainDate(2027, 2, 1))));         // P5M14D

// durations parse and re-print losslessly
print(durationString(parseDuration("2h45m")), parseDuration("1.5h") === 5400000000000); // 2h45m0s true

// wall clocks carry and wrap
print(String(new PlainDateTime(2026, 8, 17, 23, 30).add(new Duration({ hours: 2 })))); // 2026-08-18T01:30:00
print(String(new PlainTime(23, 59).add(new Duration({ minutes: 2 })))); // 00:01:00 (wraps at midnight)

// a compiled layout round-trips strict both ways
const f = new Format("2006-01-02 15:04");
print(f.format(parseRFC3339("2026-08-17T10:30:00Z").sec));  // 2026-08-17 10:30
print(formatRFC3339(parseRFC3339("2026-08-17T10:30:00Z").sec)); // 2026-08-17T10:30:00Z
```

# ext:builtins

Prototype and static extensions the engine installs on the built-in classes — a Ramda/lodash/date-fns-flavoured toolkit of 385 names inventoried by `tools/api-inventory.js` (Array 98, Date 91, Number 48, Object 73, String 71, RegExp 1; ext:Map adds `getOrInsert`/`getOrInsertComputed`, ext:Set adds `Set.groupBy`, ext:Promise and ext:Math add none). Methods are non-enumerable and live on the prototype unless marked **static**; matcher/mapper arguments follow one convention across classes — a function, a RegExp (tested against `String(el)`), or a value (SameValueZero). All methods coerce their own arguments to C locals before running user code, so a `{valueOf}`-armed argument cannot desync state.

**Opting out (`--no-prototypes`).** The whole tier described in this section is optional: booted with `--no-prototypes`, or with `DYNAJS_NO_PROTOTYPES=1` in the environment, the engine installs none of it — no Array/String/Number/Date/Function/Iterator prototype extensions and no `Array.repeat`/`Object.*`/`Function.*` static extensions. Standard ES builtins are never touched (standard Iterator helpers such as `map`/`filter`/`take`/`drop` stay), and `std`/`os` plus every `dyna:*` module keep working, because those are native code that does not call the JS extensions. Either mechanism opts out; the flag wins over an explicitly disabling `DYNAJS_NO_PROTOTYPES=0`. A script that does use the extensions fails with "not a function" under the opt-out — that is the contract, and it exists for embedders who need a strict-baseline context:

```js
import { Exec, cwd } from "dyna:sys";
const r = Exec(cwd() + "/dynajs",
               ["--no-prototypes", "-e", "print(typeof [].sum, [3,1,2].sort().join(''))"]);
print(r.stdout.trim()); // undefined 1,2,3
```

### Array

**`Array.prototype.isEmpty() -> Boolean`**

`length === 0`.

**`Array.prototype.first([n]) -> any | Array`, `head() -> any`, `last([n]) -> any | Array`, `init() -> Array`, `tail() -> Array`, `nth(i) -> any`**

- `n` *(number)* — how many elements; `first`/`last` return a new array of the first/last n, or the single first/last element without an argument.
- `i` *(number)* — an index; negative counts from the end.

`first()` is the first element (undefined if empty), with `head()` as an alias. `last()` is the last element, or the last n in original order. `init()` is all but the last; `tail()` is all but the first. `nth(i)` is the element at index i; out of range is undefined.

```js
import {} from "dyna:uuid";
const a = [1, 2, 3, 4, 5];
a.first();            // 1
a.last(2);            // [4, 5]
a.init();             // [1, 2, 3, 4]
a.tail();             // [2, 3, 4, 5]
a.nth(-1);            // 5
a.nth(9);             // undefined
```

**`Array.prototype.sum() -> number`, `average() -> number`, `mean() -> number`, `median() -> number`, `product() -> number`, `min([map]) -> any`, `max([map]) -> any`, `count(matcher) -> number`, `none(matcher) -> boolean`, `any(matcher) -> boolean`, `all(matcher) -> boolean`**

- `matcher` — a function, a RegExp, or a value (SameValueZero), per the module convention.
- `map` *(function | string, optional)* — `min`/`max` pick the element whose mapped value (a function, a property name, or identity) is smallest/largest; ties keep the first, and an empty array returns `undefined`.

`sum()` is the total of the elements coerced to doubles. `average()`/`mean()` is the sum over n, 0 for empty (`average` is the canonical spelling; `mean` is the shipped legacy alias). `median()` is the middle value, or the mean of the two middles on an even length (a NaN element yields null). `product()` is the product, 1 for empty. `count(matcher)` is how many elements the matcher accepts; `none`/`any`/`all` are the quantified forms.

```js
import {} from "dyna:uuid";
[1, 2, 3, 4].sum();                    // 10
[1, 2, 3].average();                   // 2
[1, 2, 3, 4].median();                 // 2.5
[1, 2, 3, 4].count(x => x % 2 === 0);  // 2
[1, 2, 3].none(x => x > 10);           // true
[1, 2, 3].any(x => x > 2);             // true
[1, 2, 3].all(x => x > 0);             // true
```

**`Array.prototype.take(n) -> Array`, `drop(n)`, `takeLast(n)`, `dropLast(n)`, `takeWhile(matcher)`, `dropWhile(matcher)`, `takeLastWhile(matcher)`, `dropLastWhile(matcher)`, `dropRepeats()`, `dropRepeatsWith(pred)`, `dropRepeatsBy(fn)`, `startsWith(prefix) -> boolean`, `endsWith(suffix) -> boolean`**

- `n` *(number)* — how many to keep or remove; a negative n yields empty (take/takeLast) or an unchanged copy (drop/dropLast), and values beyond the length clamp to the whole array.
- `matcher` — accepts elements to keep or drop, from the front or the back.
- `pred(lastKept, current)` — the duplicate test for `dropRepeatsWith`.
- `fn` — maps each element; `dropRepeatsBy` deep-compares the mapped values.

`take`/`drop` keep or remove the first/last n. `dropRepeats()` removes adjacent duplicates (deep equality). All keep the first element of every run. `startsWith(prefix)`/`endsWith(suffix)` test the array's leading/trailing slice by deep equality.

```js
import {} from "dyna:uuid";
[1, 2, 3, 4, 5].take(2);               // [1, 2]
[1, 2, 3, 4, 5].drop(2);               // [3, 4, 5]
[1, 2, 3, 4, 5].takeLast(2);           // [4, 5]
[1, 2, 3, 4, 5].dropLast(2);           // [1, 2, 3]
[1, 2, 3, 4].takeWhile(x => x < 3);    // [1, 2]
[1, 2, 3, 4].dropWhile(x => x < 3);    // [3, 4]
[1, 1, 2, 2, 3].dropRepeats();         // [1, 2, 3]
[{k:1},{k:1},{k:2}].dropRepeatsBy(x => x.k);  // [{k:1},{k:2}]
```

**`Array.prototype.mapFromIndex(i, fn) -> Array`, `forEachFromIndex(i, fn)`, `filterFromIndex(i, fn)`, `findFromIndex(i, fn)`, `findIndexFromIndex(i, fn)`, `someFromIndex(i, fn) -> boolean`, `everyFromIndex(i, fn) -> boolean`, `reduceFromIndex(i, fn, seed)`, `reduceRightFromIndex(i, fn, seed)`**

- `i` *(number)* — the index at which iteration starts.
- `fn` *(function)* — the callback; receives the original element and the original (de-shifted) index.
- `seed` — the reducer seed; a falsy `reduce` initialValue is dropped (seed quirk), so pass a truthy seed when one is needed.

The nine *FromIndex* methods start iteration at a given index.

```js
import {} from "dyna:uuid";
[1, 2, 3, 4].mapFromIndex(1, (x, i) => x + i);       // [3, 5, 7]
[1, 2, 3, 4, 5].filterFromIndex(2, x => x > 3);      // [4, 5]
[1, 2, 3, 4, 5].findIndexFromIndex(2, x => x > 3);   // 3
[1, 2, 3, 4, 5].reduceFromIndex(2, (a, x) => a + x, 100);  // 112
```

**`Array.prototype.partition(matcher) -> [Array, Array]`, `splitAt(i) -> [Array, Array]`, `splitEvery(n) -> Array[]`, `splitWhen(matcher) -> [Array, Array]`**

- `matcher` — `partition` splits by it; `splitWhen` starts the second half at the first accepted element (no match gives `[all, []]`).
- `i` *(number)* — the split index; negative counts from the end.
- `n` *(number)* — the chunk size; `splitEvery` refuses `n <= 0` with `RangeError`.

`partition` returns two arrays split by the matcher; `splitAt` returns `[take(i), drop(i)]`.

```js
import {} from "dyna:uuid";
[1, 2, 3, 4].partition(x => x % 2 === 0);   // [[2, 4], [1, 3]]
[1, 2, 3, 4, 5].splitAt(2);                 // [[1, 2], [3, 4, 5]]
[1, 2, 3, 4].splitEvery(2);                 // [[1, 2], [3, 4]]
[1, 2, 9, 3].splitWhen(9);                  // [[1, 2], [9, 3]]
```

**`Array.prototype.unique([map]) -> Array`, `uniq([map])`, `uniqBy(fn)`, `intersect(other)`, `intersection(other)`, `difference(other)`, `without(other)`, `union(other)`, `unionWith(pred, other)`, `differenceWith(pred, other)`, `symmetricDifference(other)`, `symmetricDifferenceWith(pred, other)`**

- `map` *(function | string)* — maps each element (identity, function, or property name) before comparing.
- `other` *(Array)* — the other set.
- `pred(x, y)` — the equality test for the `With` forms.

Set operations build a SameValueZero set from the argument. `unique`/`uniq` removes duplicates, first occurrence kept, comparing the mapped value (`unique` is canonical, `uniq` the shipped legacy alias; `uniqBy` takes the map as a function). `intersect(other)` keeps the elements of this in other, deduplicated (`intersection` is the shipped legacy alias of `intersect`); `difference(other)` keeps elements not in other, deduplicated; `without(other)` is the same but keeps this's duplicates; `union(other)` is the deduplicated concat; `unionWith(pred, other)` is uniq under `pred`; `differenceWith(pred, other)` keeps elements for which no element of other satisfies `pred(x, y)`; `symmetricDifference` and its `With` form keep elements present in exactly one side, deduplicated.

```js
import {} from "dyna:uuid";
[1, 1, 2].unique();                        // [1, 2]
[1, 1, 2, 3].without([1]);                 // [2, 3]
[1, 2].union([2, 3]);                      // [1, 2, 3]
[1, 2, 3].differenceWith((a, b) => a === b, [2, 3]);  // [1]
[1, 2, 3].symmetricDifference([2, 4]);     // [1, 3, 4]
[{k:1},{k:1},{k:2}].uniqBy(x => x.k);      // [{k:1},{k:2}]
```

**`Array.prototype.adjust(i, fn) -> Array`, `update(i, v)`, `move(from, to)`, `swap(i, j)`, `insert(i, elt)`, `insertAll(i, elts)`, `removeAt(i)`, `remove(matcher)`, `reject(matcher)`, `exclude(matcher)`, `removeRange(start, count)`, `append(x)`, `prepend(x)`**

- `i` *(number)* — an index; negative counts from the end; out-of-range indices return an unchanged copy.
- `fn` *(function)* — applied at index `i`.
- `v`, `elt`, `elts`, `x` — the values involved.
- `from`, `to` *(number)* — the relocation pair for `move`.
- `j` *(number)* — the second index for `swap` (`i == j` is a no-op).
- `matcher` — every element it accepts is removed by `remove`/`reject`/`exclude`.
- `start`, `count` *(number)* — `removeRange` removes `count` elements from `start` (negative from the end).

All non-mutating. `adjust` is a copy with `fn` applied at `i`; `update` is the same with a value; `insert`/`insertAll` append when the index is outside `[0, len]`; `append`/`prepend` push onto either end.

```js
import {} from "dyna:uuid";
[1, 2, 3].adjust(1, x => x * 10);          // [1, 20, 3]
[1, 2, 3].update(1, 9);                    // [1, 9, 3]
[1, 2, 3].move(0, 2);                      // [2, 3, 1]
[1, 2, 3].swap(0, 2);                      // [3, 2, 1]
[1, 2, 3].insert(1, 9);                    // [1, 9, 2, 3]
[1, 2, 3].remove(x => x > 1);              // [1]
[1, 2, 3].removeRange(1, 2);               // [1]
[1, 2, 3].prepend(0);                      // [0, 1, 2, 3]
```

**`Array.prototype.zip(other) -> Array`, `zipWith(fn, other)`, `zipObj(values)`, `fromPairs()`, `intersperse(sep)`, `flatten()`, `unnest()`, `transpose()`, `xprod(other)`, `aperture(n)`**

- `other` *(Array)* — the second array.
- `fn` *(function)* — combines `fn(this[i], other[i])`.
- `values` *(Array)* — `zipObj` maps `this[i]` to `values[i]`.
- `sep` — placed between every pair of elements by `intersperse`.
- `n` *(number)* — the window length; `aperture` refuses negative n with `RangeError` (`n <= 0` yields `len + 1` empty windows).

`zip` pairs are truncated to the shorter length. `fromPairs()` makes an object from `[k, v]` pairs, later pairs winning. `flatten()` is recursive, guarded to a C-stack depth of 512 (deeper nesting emitted as-is); `unnest()` flattens exactly one level. `transpose()` transposes an array of arrays, skipping missing cells in ragged input. `xprod(other)` is the cross product; `aperture(n)` is all length-n sliding windows.

```js
import {} from "dyna:uuid";
[1, 2, 3].zip(["a", "b"]);                 // [[1,"a"],[2,"b"]]
[1, 2, 3].intersperse(0);                  // [1, 0, 2, 0, 3]
[1, 2, 3, 4].aperture(2);                  // [[1,2],[2,3],[3,4]]
[[1, 2], [3, 4]].transpose();              // [[1,3],[2,4]]
["a", "b"].xprod([1, 2]);                  // [["a",1],["a",2],["b",1],["b",2]]
[["a", 1], ["b", 2]].fromPairs();          // {a:1, b:2}
["a", "b"].zipObj([1, 2]);                 // {a:1, b:2}
```

**`Array.prototype.countBy(fn) -> object`, `indexBy(fn)`, `groupBy(fn)`, `pluck(key) -> Array`, `innerJoin(pred, other) -> Array`**

- `fn` *(function | string)* — a function, a property name, or identity.
- `key` — read as `element[key]`.
- `pred(element, y)` — the join condition.
- `other` *(Array)* — the joined array.

`countBy(fn)` maps each key to the number of elements with it; `indexBy(fn)` maps each key to its **last** element; `groupBy(fn)` maps each key to the array of its elements. `innerJoin(pred, other)` keeps the elements for which `pred(element, y)` holds for some `y` in other.

```js
import {} from "dyna:uuid";
["a", "b", "a"].countBy(x => x);           // {a:2, b:1}
[1, 2, 3, 4].groupBy(x => x % 2);          // {0:[2,4], 1:[1,3]}
[{a: 1}, {a: 2}].pluck("a");               // [1, 2]
[1, 2, 3].innerJoin((a, b) => a === b, [2, 3, 4]);  // [2, 3]
```

**`Array.prototype.sortBy(map) -> Array`, `sortWith(comparators)`, `sortedIndexOf(value[, cmp]) -> number`**

- `map` *(function | string)* — the numeric or string key each element maps to.
- `comparators` *(array)* — a list of comparators applied in order.
- `value` — the value to locate.
- `cmp` *(function)* — an optional comparator.

`sortBy` is a stable decorate/sort/undecorate. `sortedIndexOf` is a binary search over an already-sorted array, returning the index or -1 (undefined behaviour on unsorted input).

```js
import {} from "dyna:uuid";
[3, 1, 2].sortBy(x => x);                  // [1, 2, 3]
[{a:2},{a:1},{a:2}].sortWith([(x,y) => x.a - y.a]);  // [{a:1},{a:2},{a:2}]
[3, 1, 2].sortedIndexOf(2);                // 1
```

**`Array.prototype.scan(fn, acc) -> Array`, `reduceBy(valueFn, acc, keyFn) -> object`, `transduce(xf, fn, acc)`, `into(acc, xf)`, `sequence(F)`, `traverse(F, fn)`**

- `fn` *(function)* — the reducer.
- `acc` — the accumulator.
- `valueFn` *(function)* — the group reducer; `reduceBy` groups by `keyFn(el)` and reduces each group from a shallow clone of `acc`.
- `xf` *(transducer)* — the transducer.
- `F` *(applicative)* — a type with `of` and `ap` methods; a type with neither is refused with `TypeError`.

`scan(fn, acc)` is every intermediate accumulator: `[acc, fn(acc, x0), ...]`, length `len + 1`. `transduce(xf, fn, acc)` reduces `this` through the transducer with a 2-arity reducer; `into(acc, xf)` does the same into a fresh container of `acc`'s type (array, string, or object) unless `acc` is a transformer. `sequence(F)` is an array of applicatives into an applicative of arrays (the list path is the cross product); `traverse(F, fn)` is `sequence(F, map(fn, this))`.

```js
import {} from "dyna:uuid";
[1, 2, 3].scan((a, b) => a + b, 0);        // [0, 1, 3, 6]
[1, 2, 3, 4].reduceBy((a, x) => a + x, 0, x => x % 2);  // {0:6, 1:4}
[1, 2, 3, 4].transduce(x => x, (a, x) => a.concat([x * 10]), []);  // [10,20,30,40]
[1, 2, 3].into([], x => x);                // [1, 2, 3]
[[1, 2], [3]].sequence(Array);             // all cross-product orderings
[1, 2].traverse(Array, x => [x, x * 10]);  // [[1,2],[1,20],[10,2],[10,20]]
```

**`Array.prototype.shuffle() -> Array`, `sample([n]) -> any | Array`**

- `n` *(number)* — how many distinct elements to sample; `n > len` returns everything shuffled.

`shuffle()` is a Fisher–Yates permutation of a copy. `sample()` is one uniformly random element, or n distinct ones. Both draw from the engine's GLOBAL RNG — for a seeded/reproducible shuffle use a `dyna:random` `Random` instance (see the which-random-when note in that section).

```js
import {} from "dyna:uuid";
[1, 2, 3].sample(2).length === 2;          // true
[1, 2, 3, 4].shuffle().sort((a, b) => a - b);  // [1, 2, 3, 4]
```

**`Array.prototype.lazy() -> Iterator`, `compact() -> Array`**

`lazy()` is a fresh, single-use iterator over the array's elements; arguments are ignored, and the Iterator-helper methods compose on the result. `compact()` is a copy without `null`/`undefined` elements (falsy values `0`, `false`, `""` survive).

```js
import {} from "dyna:uuid";
const it = [1, 2, 3].lazy();
it.next().value;                           // 1
[...it];                                   // [2, 3]
[null, 0, false, "", undefined, 3].compact();  // [0, false, "", 3]
```

**`Array.repeat(value, n) -> Array` (static)**

- `value` — repeated `n` times.
- `n` *(number)* — the count; negative is refused, a fractional count truncates down.

`Array.repeat(3, 2)` is `[3, 3]`; `n == 0` is `[]`.

### String

**`String.prototype.isEmpty() -> Boolean`, `isBlank() -> Boolean`**

`isEmpty()` is `length === 0`; `isBlank()` is empty or all-whitespace (the `trim()` set).

**`String.prototype.first([n]) -> string`, `last([n])`, `from(i)`, `to(i)`, `chars() -> string[]`, `codes() -> number[]`, `substr(start, len) -> string`**

- `n` *(number)* — code units, default 1.
- `i` *(number)* — an index; negative counts from the end; both `from` and `to` clamp.
- `start`, `len` *(number)* — the legacy substring.

`first`/`last` are the first/last n code units. `from(i)` is `[i, end)`; `to(i)` is `[0, i)`. `chars()` is an array of single-code-unit strings; `codes()` is the code units as numbers.

```js
import {} from "dyna:uuid";
"hello".first(2);            // "he"
"hello".last(2);             // "lo"
"hello".from(-3);            // "llo"
"hello".to(2);               // "he"
"abc".chars();               // ["a", "b", "c"]
"abc".codes();               // [97, 98, 99]
"hello".substr(1, 2);        // "el"
```

**`String.prototype.containsAny(set) -> boolean`, `indexOfAny(set) -> number`, `indexOfAll(sub) -> number[]`, `count(substr) -> number`, `splitN(sep, n) -> string[]`, `equalsIgnoreCase(other) -> boolean`, `compareBytes(other) -> -1 | 0 | 1`**

- `set` *(string)* — the code units to test for; a surrogate pair in `set` matches either half, like `indexOf`.
- `sub`, `substr` *(string)* — the needle.
- `sep` *(string)* — the separator.
- `n` *(number)* — `splitN` makes at most n parts, the last holding the unsplit remainder (`"a:b:c".splitN(":", 2)` is `["a", "b:c"]`); `n === 0` gives `[]`, negative n means no limit.
- `other` *(string)* — the other string.

`containsAny(set)` is true when any code unit of `set` occurs; `indexOfAny(set)` is the first such position, -1 if none. `indexOfAll(sub)` is every match position in ascending order, counting overlaps: `"aaaa".indexOfAll("aa")` is `[0, 1, 2]`; an empty needle returns `[]`. `count(substr)` is the number of non-overlapping occurrences. `equalsIgnoreCase(other)` uses ASCII case folding only (no locale). `compareBytes(other)` is -1 / 0 / 1 in **UTF-8 byte order**, which orders non-BMP characters above U+E000, unlike `<` (which compares UTF-16 code units).

```js
import {} from "dyna:uuid";
"hello".containsAny("ae");            // true
"hello".indexOfAny("lz");             // 2
"aaaa".indexOfAll("aa");              // [0, 1, 2]
"hello world".count("o");             // 2
"a:b:c".splitN(":", 2);               // ["a", "b:c"]
"HELLO".equalsIgnoreCase("hello");    // true
"caf\u00e9".compareBytes("cafe");     // 1
```

**`String.prototype.insert(str, i = end) -> string`, `remove(str)`, `removeAll(str)`, `compact()`, `shift(n)`, `pad(num, padding = " ")`, `trimPrefix(p)`, `trimSuffix(p)`, `trimChars(chars)`, `trimLeft()`, `trimRight()`**

- `str` *(string)* — inserted at the code-unit index (negative from the end, out of range clamps); `remove` removes the first occurrence; `removeAll` removes every non-overlapping occurrence (an empty needle is a no-op).
- `i` *(number)* — the insertion index, default `end`.
- `n` *(number)* — every code unit shifted by n (mod 2^16), `fromCharCode`/`charCodeAt` semantics.
- `num` *(number)* — the width in code units; `pad` leaves the string unchanged when already at least `num` or the padding coerces empty.
- `padding` *(string)* — default `" "`; **center** padding, floor to the front and ceil to the back.
- `p` *(string)* — the exact prefix/suffix removed by `trimPrefix`/`trimSuffix`.
- `chars` *(string)* — the code units trimmed from both ends by `trimChars`.

All return new strings. `compact()` trims the ends and collapses internal whitespace runs to single spaces. `trimLeft()`/`trimRight()` are aliases of `trimStart`/`trimEnd`.

```js
import {} from "dyna:uuid";
"hello".insert("!", 5);               // "hello!"
"hello world".remove(" ");            // "helloworld"
"hello world".removeAll("l");         // "heo word"
"a  b   c".compact();                 // "a b c"
"abc".shift(1);                       // "bcd"
"ABC".pad(7, "*");                    // "**ABC**"
"xxabc".trimPrefix("xx");             // "abc"
"....x....".trimChars(".");           // "x"
```

**`String.prototype.camelize(upper = true) -> string`, `underscore()`, `dasherize()`, `spacify()`, `capitalize(lower = false, all = false)`, `humanize()`, `titleize()`, `parameterize()`, `pluralize()`, `singularize()`**

- `upper` *(boolean)* — Upper- or lower-camel.
- `lower` *(boolean)* — lowercase the rest.
- `all` *(boolean)* — capitalize every word.

ASCII-oriented inflections with English rules. `underscore()` turns CamelCase to `snake_case`; `dasherize()` to `kebab-case`; `spacify()` to words separated by spaces. `humanize()` turns `"user_name_id"` into `"User name"` (strips a trailing `_id`). `titleize()` capitalizes each word, lowercasing small stop words unless first. `parameterize()` lowercases, collapses non-`[a-z0-9]` runs to one `-`, trims edges (non-ASCII acts as a separator). `pluralize()`/`singularize()` apply English suffix rules with irregular and uncountable tables (`"child"` → `"children"`, `"fish"` stays).

```js
import {} from "dyna:uuid";
"user_name".camelize();                // "UserName"
"userName".underscore();               // "user_name"
"Hello World".parameterize();          // "hello-world"
"hELLO".capitalize(true);              // "Hello"
"user_name_id".humanize();             // "User name"
"hello world".titleize();              // "Hello World"
"cat".pluralize();                     // "cats"
"children".singularize();              // "child"
```

**`String.prototype.escapeHTML() -> string`, `unescapeHTML()`, `stripTags()`, `removeTags([name])`, `anchor(name)`, `big()`, `blink()`, `bold()`, `fixed()`, `fontcolor(c)`, `fontsize(s)`, `italics()`, `link(href)`, `small()`, `strike()`, `sub()`, `sup()`**

- `name` *(string)* — the tag whose whole elements `removeTags` removes.
- `anchor(name)`, `fontcolor(c)`, `fontsize(s)`, `link(href)` — the tag arguments.

`escapeHTML()` turns `& < >` into entities; `unescapeHTML()` turns the named and numeric entities back. `stripTags()` removes tags but keeps content; `removeTags([name])` removes the whole element, content included (non-nesting; a matched open tag with no close drops to the end of the string). The wrappers `anchor`, `big`, `blink`, `bold`, `fixed`, `fontcolor`, `fontsize`, `italics`, `link`, `small`, `strike`, `sub`, `sup` produce their HTML tags.

```js
import {} from "dyna:uuid";
"<b>x</b> &".escapeHTML();             // "&lt;b&gt;x&lt;/b&gt; &amp;"
"&lt;b&gt;".unescapeHTML();            // "<b>"
"<p>hi</p>".stripTags();               // "hi"
"a<b>c</b>".removeTags("b");           // "a"
"x".bold();                            // "<b>x</b>"
"x".link("u");                         // '<a href="u">x</a>'
```

**`String.prototype.encodeBase64() -> string`, `decodeBase64()`, `escapeURL(param = false)`, `unescapeURL(param = false)`**

- `param` *(boolean)* — `escapeURL` uses `encodeURI(this)`, or `encodeURIComponent(this)` when truthy; `unescapeURL` uses `decodeURIComponent(this)`, or `decodeURI(this)` when truthy (deliberately asymmetric to escapeURL).

`encodeBase64()` is RFC 4648 base64 of the UTF-8 bytes; `decodeBase64()` is the reverse, **throwing** on an invalid character or length.

```js
import {} from "dyna:uuid";
"hi".encodeBase64();                   // "aGk="
"aGk=".decodeBase64();                 // "hi"
"a b".escapeURL();                     // "a%20b"
"a%20b".unescapeURL();                 // "a b"
"a/b".escapeURL(true);                 // "a%2Fb"
```

**`String.prototype.format(...args) -> string`, `toNumber(base = 10) -> number`, `truncate(length, from = "right", ellipsis = "...") -> string`, `truncateOnWord(length, ...) -> string`**

- `args` — `format` substitutes `{0}`-style tokens: a numeric token reads `args[n]`, a named token reads `args[0][name]`; `{{` and `}}` emit literal braces.
- `base` *(number)* — default 10; a lenient parse (`parseFloat` for base 10, else `parseInt`), `NaN` on no digits.
- `length` *(number)* — the kept length; the ellipsis is added outside it.
- `from` *(string)* — `'left'`, `'middle'`, or anything else for right.
- `ellipsis` *(string)* — default `"..."`.

`truncate` clips with the ellipsis added outside the kept length. `truncateOnWord` breaks at a word boundary; with no boundary it returns just the ellipsis.

```js
import {} from "dyna:uuid";
"hello {0}".format("world");           // "hello world"
"{{lit}}".format();                    // "{lit}"
" 42 ".toNumber();                     // 42
"abcdef".truncate(4);                  // "abcd..."
"abcdef".truncate(4, "middle");        // "ab...ef"
"one two three".truncateOnWord(7);     // "one two..."
```

**`String.prototype.words() -> string[]`, `lines()`, `forEach(fn)`, `graphemes()`, `displayWidth({ ambiguousAsWide = false }) -> number`, `wrapAnsi(columns, { hard = false, trim = true }) -> string`, `stripAnsi() -> string`, `lazy() -> Iterator`**

- `fn` *(function)* — `forEach` calls `fn(ch, i)` for each character.

- `ambiguousAsWide` *(boolean)* — count ambiguous-width characters as wide.
- `columns` *(number)* — the width in display cells.
- `hard` *(boolean)* — default false.
- `trim` *(boolean)* — default true.

`words()` is an array of whitespace-separated tokens (empties dropped). `lines()` splits on newlines, trims ends, drops the trailing blank line. `graphemes()` is an array of extended grapheme clusters (splits text, does not interpret terminal state). `displayWidth()` counts terminal cells: escapes, controls, combining marks and format characters count 0; East Asian W/F and emoji count 2. `wrapAnsi(columns)` wraps to `columns` display cells at whitespace, re-emitting active SGR state after each break. `stripAnsi()` removes CSI/OSC/escape sequences (a string with none is returned unchanged). `lazy()` is a fresh single-use iterator over the **code points** (runtime-verified: an astral character is one entry); arguments are ignored (there is no lazy-map form), and the result is a full `Iterator` — the helper methods (`map`/`filter`/`take`/`toArray`, ...) compose on it.

```js
import {} from "dyna:uuid";
"a b  c".words();                      // ["a", "b", "c"]
"a\nb".lines();                        // ["a", "b"]
"a\u{1F642}b".graphemes();             // ["a", "😂", "b"]
"e\u0301".displayWidth();              // 1
"hello world".wrapAnsi(5);             // "hello\nworld"
"\x1b[31mred\x1b[0m".stripAnsi();      // "red"
[...("a\u{1F642}b".lazy())];           // ["a", "😂", "b"]
```

### Number

**`Number.range(start, end, step = 1) -> number[]`**

- `start` *(number)* — inclusive start.
- `end` *(number)* — exclusive.
- `step` *(number)* — default 1; a step pointing the wrong way yields `[]`; a non-finite value or a zero step throws `RangeError`, as does a count past 100,000,000.

An array `[start, start + step, ...]` with `end` exclusive. `Number.EPSILON`, `Number.MAX_SAFE_INTEGER`, `Number.MIN_SAFE_INTEGER`, and `Number.NaN` are present as constants.

```js
import {} from "dyna:uuid";
Number.range(1, 5);                    // [1, 2, 3, 4]
Number.range(1, 10, 3);                // [1, 4, 7]
Number.range(10, 1, -3);               // [10, 7, 4]
Number.MAX_SAFE_INTEGER;               // 9007199254740991
```

**`Number.prototype.negate() -> number`, `inc()`, `dec()`, `add(n)`, `subtract(n)`, `multiply(n)`, `divide(n)`, `modulo(n)`, `mathMod(n)`, `abs() -> number`, `sqrt()`, `exp()`, `sin()`, `cos()`, `tan()`, `asin()`, `acos()`, `atan()`, `pow(n)`, `clamp(min, max)`, `gt(n) -> boolean`, `gte(n)`, `lt(n)`, `lte(n)`, `isOdd() -> boolean`, `isEven()`, `isMultipleOf(n)`, `log(base = e) -> number`, `round(p = 0) -> number`, `ceil(p)`, `floor(p)`**

- `n` *(number)* — the operand; `modulo` is `fmod`, matching JS `%`; `mathMod` is `NaN` unless both are integers and `n >= 1`, else the non-negative modulus `((m % n) + n) % n`; `isMultipleOf(n)` is false for `n == 0` or NaN; `pow(n)` is `Math.pow(this, n)`.
- `min`, `max` *(number)* — the clamp bounds.
- `base` *(number)* — default `e`; a change-of-base logarithm.
- `p` *(number)* — decimal places; negative rounds to tens/hundreds.

Each coerces `this` to a double, so they work on wrapper objects too. `abs`/`sqrt`/`exp`/`sin`/`cos`/`tan`/`asin`/`acos`/`atan` mirror the same-named `Math` functions on `this`. The relationals compare `this` (NaN compares false, as everywhere). `isOdd`/`isEven` are both false on non-integers.

```js
import {} from "dyna:uuid";
(5).negate();               // -5
(5).inc();                  // 6
(5).divide(2);              // 2.5
(-3).mathMod(4);            // 1
(7).clamp(1, 5);            // 5
(5).gte(5);                 // true
(5).isMultipleOf(3);        // false
(8).log(2);                 // 3
(3.14159).round(2);         // 3.14
(1234).ceil(-2);            // 1300
```

**`Number.prototype.abbr(p = 0) -> string`, `metric(p = 0)`, `bytes(p = 0)`, `format(place = 0, thousands = ",", decimal = ".")`, `pad(place = 0, sign = false, base = 10)`, `hex(place = 1)`, `ordinalize()`, `duration()`, `chr()`**

- `p` *(number)* — the decimal places.
- `place` *(number)* — `format` groups the integer part in 3s (place capped at 20); `pad` zero-pads the integer form to `place` digits (width capped at 1 MiB, base must be 2..36); `hex` is the base-16 form.
- `thousands`, `decimal` *(string)* — the group and decimal separators for `format`.
- `sign` *(boolean)* — include a sign in the padded form.
- `base` *(number)* — the radix for `pad`.

`abbr(p)` scales with a `k/m/b/t` suffix (base 1000); `metric(p)` uses SI prefixes `k M G T P E`; `bytes(p)` is base-1024 `KB/MB/...`. `ordinalize()` turns `1` → `"1st"`, `11`–`13` → `"th"`. `duration()` renders milliseconds as the largest English unit (`90061000` → `"1 day"`). `chr()` is `fromCharCode(this)`.

```js
import {} from "dyna:uuid";
(1500).abbr();               // "2k" (1.5 scaled, zero decimals)
```

**`Number.prototype.times(fn?) -> number[]`, `upto(end, step = 1, fn?)`, `downto(end, step = 1, fn?)`**

- `fn(i, i)` *(function)* — applied to each value; `times` without a fn yields `[0..n-1]`.
- `end` *(number)* — the inclusive end of `upto`/`downto`.
- `step` *(number)* — default 1; a step pointing the wrong way yields `[]`.

`times(fn)` is `[fn(0, 0), ..., fn(n-1, n-1)]`; counts past 100,000,000 throw `RangeError` before allocating. `upto`/`downto` build an inclusive range from `this` to `end`, with the same cap and refusal rules as `range`.

```js
import {} from "dyna:uuid";
(3).times(x => x * x);        // [0, 1, 4]
(5).upto(7);                  // [5, 6, 7]
(5).upto(10, 2, x => x * 2);  // [10, 14, 18]
(5).downto(3);                // [5, 4, 3]
(1).times(x => x * 2);        // [0]
```

### Object

**`Object.isObject(v) -> boolean`, `isArray(v)`, `isBoolean(v)`, `isNumber(v)`, `isString(v)`, `isFunction(v)`, `isDate(v)`, `isRegExp(v)`, `isError(v)`, `isSet(v)`, `isMap(v)`, `isArguments(v)`, `isNil(v)`, `isNotNil(v)`, `type(v) -> string`**

- `v` — the value to test.

`isObject(v)` is a plain object (not array, not function, not null). `isArray(v)` is the real Array check, so a Proxy of an Array reports true; the others test by class. `isNil(v)` is null or undefined; `isNotNil(v)` is the complement. `type(v)` is `"Number"`, `"String"`, `"Null"`, `"Undefined"`, `"Array"`, `"Function"`, `"Date"`, `"RegExp"`, `"Map"`, `"Set"`, `"Error"`, `"Boolean"`, `"BigInt"`, `"Symbol"`, or `"Object"`.

```js
import {} from "dyna:uuid";
Object.isNil(null);              // true
Object.isObject({ a: 1 });       // true
Object.isObject([]);             // false
Object.isArray([1]);             // true
Object.type(42);                 // "Number"
Object.type(null);               // "Null"
Object.type([]);                 // "Array"
```

**`Object.defaultTo(d, v)`, `objOf(k, v)`, `invert(o)`, `invertObj(o)`, `size(o) -> number`, `isEmpty(o) -> boolean`**

- `d` — the default; `defaultTo` returns `v` unless it is null, undefined, or NaN, then `d`.
- `v` — the value.
- `k` *(string)* — the key; `objOf` builds `{ [k]: v }`.
- `o` — the object.

`invert`/`invertObj` swap keys and values, values become keys via `ToPropertyKey`, last value wins (both spellings are the same implementation). `size(o)` is the count of own enumerable string keys (works on strings and arrays); `isEmpty(o)` is that count being 0.

```js
import {} from "dyna:uuid";
Object.defaultTo(5, undefined);  // 5
Object.defaultTo(5, 0);          // 0
Object.objOf("k", 1);            // {k: 1}
Object.invert({ a: 1 });         // {1: "a"}
Object.size("abc");              // 3
Object.isEmpty({});              // true
```

**`Object.pick(keys, o)`, `pickAll(keys, o)`, `omit(keys, o)`, `pickBy(pred, o)`, `toPairs(o)`, `fromPairs(pairs)`, `assoc(k, v, o)`, `dissoc(k, o)`, `renameKeys(map, o)`**

- `keys` *(array)* — the keys.
- `o` — the object.
- `pred(value, key)` — the filter.
- `pairs` *(array)* — the `[k, v]` pairs.
- `k` *(string)* — the key.
- `v` — the value.

`pick` returns the listed keys that exist (via `in`, so inherited keys are included); `pickAll` is like pick but missing keys become `undefined`; `omit` returns own enumerable props not in keys; `pickBy` keeps props where `pred(value, key)` is truthy. `toPairs` is `[[k, v], ...]`; `fromPairs` is the reverse. `assoc(k, v, o)` is a shallow copy with k set; `dissoc(k, o)` is a shallow copy without k; `renameKeys(map, o)` is a copy with keys renamed where present.

```js
import {} from "dyna:uuid";
Object.pick(["a"], { a: 1, b: 2 });        // {a: 1}
Object.pickAll(["a", "c"], { a: 1 });      // {a: 1, c: undefined}
Object.omit(["a"], { a: 1, b: 2 });        // {b: 2}
Object.assoc("c", 3, { a: 1 });            // {a: 1, c: 3}
Object.renameKeys({ a: "z" }, { a: 1 });   // {z: 1}
Object.toPairs({ a: 1 });                  // [["a", 1]]
```

**`Object.path(p, o)`, `pathOr(d, p, o)`, `paths(plist, o)`, `hasPath(p, o)`, `assocPath(p, v, o)`, `dissocPath(p, o)`, `set(o, p, v)`, `get(o, p, d?)`, `prop(k, o)`, `propOr(d, k, o)`, `props(keys, o)`, `propEq(v, k, o)`, `eqProps(k, a, b)`, `pathEq(v, p, o)`, `propSatisfies(pred, k, o)`, `pathSatisfies(pred, p, o)`, `propIs(Ctor, k, o)`, `where(spec, o)`, `whereAny(spec, o)`, `whereEq(spec, o)`**

- `p` — a path, a dotted string or an array of keys.
- `o` — the object.
- `d` — the default value.
- `k` *(string)* — a key.
- `v` — a value.
- `pred` *(function)* — a predicate.

`path` is a deep read, `undefined` on a missing step; `pathOr` supplies a default; `paths` is an array of path reads; `hasPath` checks own properties along the whole path. `assocPath` is an immutable deep set, missing intermediates created as **plain objects, never arrays** even for integer keys; `dissocPath` is an immutable deep delete. `set(o, p, v)` is the mutating deep set (returns `o`); `get(o, p, d?)` is the non-mutating read. `prop`/`propOr`/`props` are the shallow equivalents. Predicates: `propEq` is `equals(o[k], v)`; `eqProps` is `equals(a[k], b[k])`; `pathEq` compares at a path; `propSatisfies`/`pathSatisfies` test a predicate; `propIs` is `is(Ctor, o[k])`. `where(spec, o)` requires every `spec[k]` to be a **predicate** `o[k]` must satisfy (a non-function spec value throws TypeError); `whereAny(spec, o)` requires any one; `whereEq(spec, o)` requires every `o[k]` to deep-equal `spec[k]`.

```js
import {} from "dyna:uuid";
Object.path(["a", "b"], { a: { b: 3 } });      // 3
Object.pathOr(9, ["x"], {});                   // 9
Object.assocPath(["a", "b"], 3, {});           // {a: {b: 3}}
Object.get({ a: { b: 1 } }, "a.b", 9);         // 1
Object.set({ a: { b: 1 } }, "a.c", 2);         // {a: {b: 1, c: 2}}
Object.pathEq(2, ["a", "b"], { a: { b: 2 } }); // true
Object.where({ a: x => x > 0 }, { a: 1 });     // true
Object.whereEq({ a: { b: 1 } }, { a: { b: 1 } });  // true
```

**`Object.merge(a, b)`, `mergeRight(a, b)`, `mergeLeft(a, b)`, `mergeDeepRight(a, b)`, `mergeDeepLeft(a, b)`, `mergeWith(fn, a, b)`, `mergeWithKey(fn, a, b)`, `defaults(obj, source)`**

- `a`, `b` — the objects.
- `fn` *(function)* — `mergeWith` resolves conflicts with `fn(a[k], b[k])`; `mergeWithKey` with `fn(k, a[k], b[k])`.
- `obj`, `source` — `defaults` fills only missing keys from `source`, non-mutating.

`merge`/`mergeRight` — b wins, left key order kept; `mergeLeft` — a wins, b's missing keys appended; the deep forms recurse.

```js
import {} from "dyna:uuid";
Object.mergeRight({ a: 1 }, { a: 2, b: 3 });   // {a: 2, b: 3}
Object.mergeLeft({ a: 1 }, { a: 2, b: 3 });    // {a: 1, b: 3}
Object.mergeDeepRight({ a: { x: 1 } }, { a: { y: 2 } });  // {a: {x: 1, y: 2}}
Object.mergeWith((a, b) => a + b, { a: 1 }, { a: 2 });   // {a: 3}
Object.defaults({ a: 1 }, { b: 2 });           // {a: 1, b: 2}
```

**`Object.evolve(transforms, o)`, `modify(k, fn, o)`, `modifyPath(p, fn, o)`, `mapObjIndexed(fn, o)`, `forEachObjIndexed(fn, o)`, `mapKeys(fn, o)`, `project(keys, arr)`, `tap(fn, x)`**

- `transforms[k]` — a fn, or nested transforms; `evolve` applies it to `o[k]`, untouched keys pass through.
- `k` *(string)* — a key.
- `p` — a path.
- `fn` *(function)* — `mapObjIndexed` calls `fn(value, key, obj)` over own enumerable keys; `mapKeys` transforms keys by `fn(key)`.
- `o` — the object.
- `arr` *(array)* — `project` returns `arr.map(o => pick(keys, o))`.

`modify` is a shallow copy with `fn` applied to `o[k]` if present; `modifyPath` is the deep form. `forEachObjIndexed` is the side-effect form, returns undefined. `tap(fn, x)` calls `fn(x)` for its side effect and returns `x`.

```js
import {} from "dyna:uuid";
Object.evolve({ a: x => x + 1 }, { a: 1 });           // {a: 2}
Object.modify("a", x => x + 1, { a: 1 });             // {a: 2}
Object.mapObjIndexed((v, k) => k + v, { a: 1 });      // {a: "a1"}
Object.mapKeys(k => k.toUpperCase(), { a: 1 });       // {A: 1}
Object.project(["a"], [{ a: 1, b: 2 }, { a: 3 }]);    // [{a:1}, {a:3}]
Object.tap(x => x + 1, 5);                            // 5
```

**`Object.equals(a, b) -> boolean`, `identical(a, b)`, `clone(o)`, `hasIn(k, o) -> boolean`, `has(k, o)`, `keysIn(o)`, `valuesIn(o)`**

- `a`, `b` — the operands.
- `o` — the object.
- `k` *(string)* — the key.

`equals` is deep equality (arrays, plain objects, RegExp by source and flags, Date by time; cycles by reference fast-path). `identical` is the SameValue test (`-0 !== 0`, `NaN === NaN`). `clone` is a deep clone (a cyclic structure overflows the C stack with `RangeError`). `hasIn` is the `in` operator; `has` is own property only; `keysIn`/`valuesIn` are own **and** inherited enumerable keys/values.

```js
import {} from "dyna:uuid";
Object.equals({ a: [1, 2] }, { a: [1, 2] });   // true
Object.equals(NaN, NaN);                        // true
Object.identical(-0, 0);                        // false
Object.hasIn("toString", {});                   // true
Object.keysIn(Object.create({ inh: 1 }));       // ["inh"]
```

### Date

**`Date.prototype.isValid() -> Boolean`**

A non-NaN time value.

**`Date.prototype.isToday()`, `isYesterday()`, `isTomorrow()`, `isFuture()`, `isPast()`, `isWeekday()`, `isWeekend()`, `isLeapYear()`, `isSunday()`, `isMonday()`, `isTuesday()`, `isWednesday()`, `isThursday()`, `isFriday()`, `isSaturday()`, `isJanuary()`, `isFebruary()`, `isMarch()`, `isApril()`, `isMay()`, `isJune()`, `isJuly()`, `isAugust()`, `isSeptember()`, `isOctober()`, `isNovember()`, `isDecember() -> boolean`**

Calendar predicates, false on an invalid date: `isToday`/`isYesterday`/`isTomorrow` use the local calendar day; `isFuture`/`isPast`/`isWeekday`/`isWeekend`/`isLeapYear` and the day-name predicates `isSunday`..`isSaturday` and month-name predicates `isJanuary`..`isDecember` follow their names.

**`Date.prototype.getWeekday() -> number`, `getISOWeek() -> number`, `daysInMonth() -> number`**

`getWeekday()` is 0 (Sunday) .. 6. `getISOWeek()` is ISO-8601 week 1..53 (a year boundary rolls into the neighbouring year's week). `daysInMonth()` is 28..31 for the local month/year.

```js
import {} from "dyna:uuid";
const d = new Date(2024, 0, 15, 10, 30);   // Mon Jan 15 2024
d.isValid();            // true
d.isMonday();           // true
d.isJanuary();          // true
d.isLeapYear();         // true
d.isWeekend();          // false
d.getWeekday();         // 1
d.getISOWeek();         // 3
d.daysInMonth();        // 31
```

**`Date.prototype.isBefore(other) -> boolean`, `isAfter(other)`, `isBetween(a, b)`**

- `other`, `a`, `b` — a Date or a timestamp.

`isBefore`/`isAfter` are strict ms ordering; `isBetween` is inclusive, arguments swapped freely.

**`Date.prototype.XSince(other)`, `XUntil(other)`, `XAgo()`, `XFromNow()` — for X in milliseconds, seconds, minutes, hours, days, weeks, months, years: `millisecondsSince`/`millisecondsUntil`/`millisecondsAgo`/`millisecondsFromNow`, `secondsSince`/`secondsUntil`/`secondsAgo`/`secondsFromNow`, `minutesSince`/`minutesUntil`/`minutesAgo`/`minutesFromNow`, `hoursSince`/`hoursUntil`/`hoursAgo`/`hoursFromNow`, `daysSince`/`daysUntil`/`daysAgo`/`daysFromNow`, `weeksSince`/`weeksUntil`/`weeksAgo`/`weeksFromNow`, `monthsSince`/`monthsUntil`/`monthsAgo`/`monthsFromNow`, `yearsSince`/`yearsUntil`/`yearsAgo`/`yearsFromNow`**

- `other` — a Date or a timestamp.

`XSince(other)` is `this - other`; `XUntil(other)` is `other - this`; `XAgo()` is `now - this`; `XFromNow()` is `this - now`. The fixed-size units truncate toward zero; months and years use calendar arithmetic (day-of-month adjust: mid-February to mid-March is 1 month). An invalid operand yields NaN.

```js
import {} from "dyna:uuid";
const d = new Date(2024, 0, 15, 10, 30);
d.isBefore(new Date(2025, 0, 1));             // true
d.isBetween(new Date(2023, 0, 1), new Date(2025, 0, 1));  // true
d.daysUntil(new Date(2024, 1, 15, 10, 30));   // 31
d.monthsUntil(new Date(2024, 6, 10));         // 5
d.hoursFromNow() < 0;                         // true
```

**`Date.prototype.addMilliseconds(n)`, `addSeconds(n)`, `addMinutes(n)`, `addHours(n)`, `addDays(n)`, `addWeeks(n)`, `addMonths(n)`, `addYears(n) -> Date`, `advance({ years, months, weeks, days, hours, minutes, seconds, milliseconds }) -> Date`, `rewind(spec) -> Date`, `clone() -> Date`**

- `n` *(number)* — the shift, negative allowed; months and years use calendar math, so `addMonths` on Jan 31 lands on Feb 28/29.
- `spec` *(object)* — the amounts for `advance`; `rewind` applies the same spec negated.

Each returns a new Date shifted by the given amount. `clone()` is a new Date with the same ms.

```js
import {} from "dyna:uuid";
const d = new Date(2024, 0, 15, 10, 30);
d.addDays(1).getDate();           // 16
d.addMonths(1).getMonth();        // 1
d.addYears(1).getFullYear();      // 2025
d.advance({ days: 2, hours: 1 }).getHours();   // 11
d.rewind({ days: 1 }).getDate();  // 14
d.clone().getTime() === d.getTime();           // true
```

**`Date.prototype.beginningOfDay() -> Date`, `endOfDay()`, `beginningOfWeek()`, `endOfWeek()`, `beginningOfMonth()`, `endOfMonth()`, `beginningOfYear()`, `endOfYear()`**

Boundaries in local time. `endOfDay()` is 23:59:59.999 and the week starts Sunday.

**`Date.prototype.format(mask) -> string`, `iso()`, `relative()`, `toGMTString()`, `getYear() -> number`, `setYear(y)`**

- `mask` *(string)* — `{token}` substitution with `yyyy yy MM M dd d HH H hh h mm m ss s SSS tt TT Mon Month dow Weekday`; no mask gives the `YYYY-MM-DD HH:MM:SS` default; an invalid date yields `"Invalid Date"`.
- `y` *(number)* — a two-digit value is treated as 1900 + y, a three-or-more-digit value as the absolute year.

`iso()` is `toISOString()`. `relative()` is English `"N units ago"` / `"in N units"` / `"just now"` against `Date.now()`. `toGMTString()` is an alias of `toUTCString()`. `getYear()` is the year minus 1900 (124 for 2024).

```js
import {} from "dyna:uuid";
const d = new Date(2024, 0, 15, 10, 30);
d.beginningOfMonth().getDate();       // 1
d.endOfMonth().getDate();             // 31
d.beginningOfWeek().getDay();         // 0 (Sunday)
d.endOfDay().getHours();              // 23
d.format("{yyyy}-{MM}-{dd}");         // "2024-01-15"
d.format("{Month} {d}, {yyyy}");      // "January 15, 2024"
d.format("{Weekday} {hh}:{mm} {tt}"); // "Monday 10:30 am"
d.iso() === d.toISOString();          // true
const y = new Date(2020, 0, 1); y.setYear(99);
y.getFullYear();                      // 1999
```

### RegExp

**`RegExp.prototype.compile(pattern, flags) -> this`**

- `pattern` *(RegExp | string)* — the pattern; may be another RegExp, in which case `flags` must be undefined (else `TypeError`).
- `flags` *(string)* — the flags.

Re-compiles the regex in place and returns `this`; `lastIndex` resets to 0.

```js
import {} from "dyna:uuid";
const re = /x/;
re.compile("a+", "g");
re.test("aaa");                       // true
```

### Map & Set

**`Map.prototype.getOrInsert(key, value?) -> any`, `getOrInsertComputed(key, fn) -> any`**

- `key` — the key.
- `value` *(optional)* — inserted when the key is absent; without it, `undefined` is inserted and the key becomes present.
- `fn` *(function)* — `getOrInsertComputed` calls `fn(key)` and inserts the result when the key is absent.

Both return the stored value for `key`, inserting first when absent. The entry, once present, is returned unchanged on later calls.

**`Set.groupBy(items, fn) -> Map` (static)**

- `items` *(iterable)* — the input values.
- `fn` *(function)* — the key function, called as `fn(value)`.

Groups the input into a `Map` whose keys are `fn`'s results and whose values are arrays in input order.

```js
import {} from "dyna:uuid";
const m = new Map();
m.getOrInsert("k", 1);                 // 1
m.getOrInsert("k", 2);                 // 1 (present, unchanged)
m.getOrInsertComputed("j", k => k.length);  // 1
Set.groupBy([1, 2, 3, 4], v => v % 2).get(0);  // [2, 4]
```

---

**Remaining exports:** none of the 385 names is left unaccounted for; every name above is covered by its inventory row.


# globals

Ambient globals installed at context creation, before any script runs — no import needed. The module-backed four — `URL`, `URLSearchParams``sleep` and `structuredClone`plus the engine globals that previously shipped undeclared: `queueMicrotask`, the latin-1 `atob`/`btoa`, the ES2025 `Iterator` helpers, `Float16Array`, the disposable-stack trio, `InternalError`, the `Lens` optics constructor and `__loadScript` (internal). Each is THE SAME object the corresponding module exports — `globalThis.URL === (await import("dyna:url")).URL` and `globalThis.structuredClone === (await import("dyna:serialize")).structuredClone` hold, so `instanceof`, identity checks and caching behave identically for both spellings. Each is a writable, enumerable, configurable data property of `globalThis`: a script may shadow it, and `delete globalThis.URL` removes it without touching the module surface (a later import still returns the constructor). Workers install the same four names on their own runtime. In a build compiled without a module, its globals are simply absent — importing the module fails the same way, and nothing else changes.

**`URL`, `URLSearchParams`**

The dyna:url WHATWG URL parser and query list, promoted to globals; behaviour, bounds and refusals are exactly the `dyna:url` `URL` section's. A bound `url.searchParams` still writes through to `url.search`.

```js
import {} from "dyna:url";
const u = new URL("https://example.com:8443/a/b?x=1#f");
u.host;                          // "example.com:8443"
u.searchParams.set("x", "9");
String(u);                       // "https://example.com:8443/a/b?x=9#f"
const sp = new URLSearchParams({ a: "1", b: "x y" });
sp.toString();                   // "a=1&b=x+y"
URL === (await import("dyna:url")).URL;   // true
```

**`sleep(ms) -> Promise<void>`**

Resolves after `ms` milliseconds. It IS `os.sleepAsync` — the same C entry point and the same timer machinery — so it needs no import and other ready work (timers, IO, worker messages) is serviced while it waits. Fractional milliseconds truncate; `NaN`, negative or absent count as 0; values above 2^31-1 cap there. It cannot be cancelled (there is no handle). A `sleep` that nothing awaits is still a real timer: an unawaited pending sleep keeps the process alive until it fires — measured: an unawaited `sleep(3000)` ends the process after ~3.02s, and `sleep(100000)` hangs until killed — identical to `os.sleepAsync`.

```js
import {} from "dyna:serialize";
const t0 = Date.now();
await sleep(30);
const waited = Date.now() - t0;   // >= 30
typeof await sleep(0);            // "undefined"
```

**`structuredClone(value) -> unknown`**

Cycle-aware deep copy — the `dyna:serialize` export. Plain objects, arrays, `Date`, `Map`, `Set`, `RegExp`, typed arrays and `ArrayBuffer` (fresh buffer, same bytes), `BigInt` and `Symbol` clone; nested objects are copied, not shared; cycles are preserved inside the copy. Functions are refused with a `TypeError` naming the reason, as is a call with no argument. Byte views keep their intrinsic kind through engine-internal reconstruction — `constructor`/prototype spoofs, swapped or deleted global intrinsics cannot re-route it — and the documented edge semantics hold: a Proxy-wrapped view clones as a plain object, a detached `ArrayBuffer` clones to `{}`, and views over one shared buffer clone to independent windows (see the `dyna:serialize` section).

```js
import {} from "dyna:serialize";
const src = { at: new Date(1234), tags: new Set(["a"]), next: null };
src.next = src;
const c = structuredClone(src);
c !== src && c.next === c && c.at.getTime() === 1234;   // true
structuredClone(new Uint8Array([1, 2])).byteLength;     // 2
```


`queueMicrotask(callback)` — microtask scheduling; the callback runs before the next macrotask.

`atob(data)` / `btoa(data)` — the WHATWG base64 codecs, and a deliberate trap: they are LATIN-1 STRING codecs, not UTF-8. `btoa("é")` is `6Q==` (one raw byte; characters above U+00FF throw), while the byte-level `Base64Encode` of the same string's UTF-8 bytes is `w6k=`. For JS strings use `encoding.Base64*` or `String.prototype.encodeBase64`/`decodeBase64`; `atob`/`btoa` exist for web-compat code that already holds latin-1 strings (see the base64 vocabulary table in the `dyna:encoding` section).

`Iterator` — the base iterator constructor. Every runtime iterator's prototype chain reaches `Iterator.prototype`, which carries the ES2025 iterator helpers (`map`, `filter`, `take`, `drop`, `flatMap`, `reduce`, `toArray`, `forEach`, `some`, `every`, `find`) plus the engine extras (`includes`, `takeWhile`, `dropWhile`, `scan`, `intersperse`, `compact`, `dropRepeats*`, `aperture`, `splitEvery`, `zipWith`, `pluck`, `init`, `tail`, `unique`/`uniq`/`uniqBy`, `tee`, the numeric folds, `first`/`last`/`nth`, `countBy`, `indexBy`, `groupBy`, `reduceBy`). So `[1,2,3].values().map(x => x * 2).toArray()` works, as do the prototype lazies (`"str".lazy()`, `arr.lazy()`). Statics: `Iterator.from(iterable)`, `Iterator.concat(...iterables)`, `Iterator.zip(iterables, {mode})` with mode `"shortest"` (default), `"longest"` or `"strict"`, and `Iterator.zipKeyed({key: iterable, ...})`; raw strings are refused as zip inputs.

`Float16Array` — the ES2025 half-precision (binary16) typed array; a standard `%TypedArray%` (`BYTES_PER_ELEMENT` is 2, full prototype surface).

`DisposableStack` / `AsyncDisposableStack` / `SuppressedError` — explicit resource management; the engine honors `using` and `await using`. A stack's `use(v)` registers a resource, `adopt(v, fn)` a value with a disposer, `defer(fn)` a bare callback, `move()` hands the callbacks to a fresh stack; `dispose()` (or `disposeAsync()`) runs them in reverse order. `SuppressedError` carries both `error` and `suppressed` when disposal itself throws.

`InternalError` — an `Error` subclass thrown by internal invariant failures (for example a failed `dyna:uring` read). Not for user code.

`WeakRef` / `FinalizationRegistry` — present at runtime, typed by the standard library's own ES2021 declarations.

`Lens` — a native optics value: a reusable getter/setter pair over one focus, with immutable updates. Constructors: `Lens.prop(name)`, `Lens.index(i)`, `Lens.path("a.b.c" or ["a","b"])`, `Lens.lens(get, set)` (also `new Lens(get, set)` — callable both ways). Instance: `lens.view(source)`, `lens.set(value, source)`, `lens.over(fn, source)` — never mutating `source`. Statics `Lens.view(lens, source)` / `Lens.set(lens, value, source)` / `Lens.over(lens, fn, source)` are the same operations in free-function form. Lens vs the `Object.path` family: Lens is for REUSABLE, composable optics and `over`-style transforms; `Object.path`/`get`/`set`/`assocPath` are one-off paths with the object in hand.

```js
const pa = Lens.prop("a");
pa.view({a: 1});                        // 1
pa.set(9, {a: 1, b: 2});                // {a: 9, b: 2} (source untouched)
pa.over(x => x * 10, {a: 5});           // {a: 50}
Lens.path("x.y").view({x: {y: 42}});    // 42
Iterator.from([1, 2, 3]).map(x => x * 2).take(2).toArray();  // [2, 4]
```

`__loadScript(filename)` — @internal: loads and evaluates a script file into global scope (engine bootstrap helper; use `import` or `std.loadScript` instead).

**`WebSocket`**

Global thin alias over `dyna:http` `WsClient` (the same constructor object): `globalThis.WebSocket === (await import("dyna:http")).WsClient`. Browser-compat door for WebSocket clients; behaviour, handler bag and bounds are exactly the `WsClient` section's.

```js
const Ws = (await import("dyna:http")).WsClient;
print(WebSocket === Ws); // true
```

### TextDecoder / TextEncoder

**`new TextDecoder([label], [options])`**

- `label` *(string)* — encoding label; only UTF-8 is supported (`"utf-8"`, `"utf8"`, `"unicode-1-1-utf-8"`, case-insensitive, whitespace-trimmed). Anything else throws `RangeError`.
- `options` *(object)* — strict: unknown keys throw `TypeError` (e.g. `unknown option "fatl" (valid: fatal, ignoreBOM)`), and a non-object options value throws.
  - `fatal` *(boolean, default false)* — throw `TypeError` on malformed UTF-8 instead of emitting U+FFFD.
  - `ignoreBOM` *(boolean, default false)* — keep a leading BOM as U+FEFF content instead of stripping it.

**`get encoding -> "utf-8"`, `get fatal -> boolean`, `get ignoreBOM -> boolean`**

**`decode([buffer], [options]) -> string`**

- `buffer` *(Uint8Array | ArrayBuffer | DataView, optional)* — bytes to decode; omitted/undefined decodes nothing.
- `options` *(object)* — strict: unknown keys throw `TypeError` naming the key and the valid one (`unknown option "strem" (valid: stream)`); `stream` must be a boolean.
  - `stream` *(boolean, default false)* — `true` continues the decoder's stream: an incomplete trailing multi-byte sequence is carried (no U+FFFD, no fatal error) and prepended to the next call's input. `false` (or omitted) is a FINAL call: the carry is processed, an incomplete tail becomes U+FFFD (or throws under `fatal`), and the decoder resets — the next `decode()` starts a fresh stream.

A stream's leading BOM is stripped once, even when its bytes complete in a later chunk than the call that opened the stream; BOMs later in the stream decode to U+FEFF like any other code point. Plain (non-streaming) calls are stateless: every call strips one leading BOM and an incomplete tail becomes U+FFFD.

```js
const bytes = new TextEncoder().encode("aé😀");
const head = bytes.slice(0, 2), tail = bytes.slice(2);
const dec = new TextDecoder();
print(dec.decode(head, { stream: true }) + dec.decode(tail, { stream: false }));
```

**`TextEncoder`** — `new TextEncoder()`, `get encoding -> "utf-8"`, `encode(input?) -> Uint8Array`, `encodeInto(input, dest) -> { read, written }`.
  tests/test_textdecoder_stream.js \
