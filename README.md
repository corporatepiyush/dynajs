# DynaJS

> **Status: BETA.** The engine is stable and the standard-library surface is complete; pre-1.0 releases may change names, options and defaults.

DynaJS is an embeddable JavaScript engine and application runtime written in C. It executes a single deployable artifact — one static binary containing the interpreter, the garbage collector, and a compiled-in standard library of 35 modules covering networking, cryptography, data processing, machine learning, and systems work. There is no package directory to vendor, no lockfile, no dependency-resolution step, and no runtime installation on the target machine.

The design goal is operational simplicity for services, data jobs, and edge deployments: a program and its entire dependency tree compile to one executable that starts in milliseconds, uses a predictable amount of memory, and has a supply chain of exactly the compiler and four system libraries.

---

## Table of contents

- [Why DynaJS](#why-dynajs)
- [Install](#install)
- [Quick start](#quick-start)
- [Command line](#command-line)
- [The standard library](#the-standard-library)
- [Core concepts](#core-concepts)
- [Recipes: complete programs](#recipes-complete-programs)
- [Embedding DynaJS](#embedding-dynajs)
- [Building from source](#building-from-source)
- [Performance](#performance)
- [How correctness is enforced](#how-correctness-is-enforced)
- [Project layout](#project-layout)
- [AI agent guide](#ai-agent-guide)
- [TypeScript and IDE support](#typescript-and-ide-support)
- [License and docs](#license-and-docs)

---

## Why DynaJS

**One artifact, zero runtime dependencies.** The canonical build links only the operating system's security-tracked libraries (OpenSSL, SQLite, zstd, brotli). Deploying a DynaJS service means copying one file to the target and running it. There is no package directory to vendor, no version manager to install, and no step at which a package registry participates in your build. Supply-chain surface is the four system libraries and the compiler.

**A complete runtime, not a wrapper.** The parser, bytecode compiler, interpreter, and garbage collector are original C with no upstream engine dependency. The language surface targets ES2023 — classes, generators, async/await, modules, destructuring, `BigInt`, typed arrays, `Symbol.dispose` — and conformance is pinned by a test262 gate that runs on every push.

**The standard library is compiled in, not installed.** `dyna:*` modules ship in the binary: an HTTP server and WHATWG `fetch`, TCP/UDP sockets, DNS, Redis/PostgreSQL/SQLite clients, TLS with certificate verification, AES-GCM and ChaCha20-Poly1305 encryption, RSA/ECDSA/Ed25519 signatures, JWT, Argon2id password hashing, columnar DataFrames, decision trees and k-means, graphs and heaps and tries, TOML/YAML/XML/CSV parsers, gzip/zstd/brotli compression, structured logging, and a runtime-dispatched SIMD portfolio. Nothing is downloaded at any point.

**Failures are loud and bounded.** Every parser that accepts untrusted input carries size caps and refusal lists; decompression bombs are rejected deterministically instead of exhausting memory; the bytecode reader validates every deserialized index against the writer's contract before execution. The cryptographic comparators are constant-time. This posture is enforced by the gate, not by convention — see [How correctness is enforced](#how-correctness-is-enforced).

**Predictable performance.** There is no JIT warm-up and no optimization-deoptimization cycle: a cold start is a REPL with history and completion, or a `-e` one-liner with top-level `await`. Heavy numeric work runs through runtime-selected SIMD kernels (NEON, SSE4.2, AVX2, AVX-512, SVE — observable via `simd.active()`), data work is columnar over TypedArrays, and the HTTP server is syscall-bound with per-peer work capped.

**Embedding is a first-class mode.** The engine builds as `libdynajs.a`, and `dynajsc` compiles JavaScript to bytecode linked statically into a native executable — a JavaScript program ships as one binary with no JS source and no interpreter to install.

---

## Install

One command on macOS, Linux or FreeBSD:

```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynajs/master/install.sh | bash
```

The installer clones the source into `~/.cache/dynajs-build`, builds from scratch with the full native standard library, and installs one static binary to `/usr/local/bin/dynajs` (falling back to `$HOME/.local/bin` when that needs root and sudo is unavailable). It asks no questions, so it is safe to pipe. There is exactly one install mechanism: the script always builds the latest commit on master. Re-running it is how you upgrade — it re-clones, rebuilds, and overwrites the old binary. The full build log is kept at `~/.cache/dynajs-build/install.log`.

Options:

```sh
install.sh --prefix "$HOME/.local"   # install to a different prefix
install.sh --with-deps               # also install missing build tools (brew/apt/dnf/...)
install.sh --dry-run                 # print the plan and preflight report, install nothing
install.sh --uninstall               # remove the installed binary
install.sh --verbose                 # stream the build output
```

A first install needs `git`, `make` and a C compiler (clang preferred). On macOS the Xcode Command Line Tools provide all three. The preflight report says exactly what is missing before anything is downloaded.

---

## Quick start

```js
// hello.js — the standard library is already there
import { zstd, unzstd } from "dyna:compress";
const data = new Uint8Array(Array.from({ length: 1000 }, (_, i) => i % 7));
console.log("round trip ok:", unzstd(zstd(data)).every((v, i) => v === data[i]));
```

```sh
dynajs hello.js            # run a file
dynajs -e 'print(1 + 1)'   # evaluate an expression
dynajs -i                   # REPL with history, tab-completion and \h help
```

Web-platform globals work without importing anything:

```js
const res = new Response(JSON.stringify({ hello: "world" }), {
    status: 200,
    headers: { "Content-Type": "application/json" }
});
console.log("Response ok:", res.ok);
res.json().then(data => console.log("JSON:", JSON.stringify(data)));
```

And a complete HTTP service is a file, not a project (this block binds a
port and runs until killed — recipe 1 below is the runnable form):

<!-- check:skip -->
```js
import { App } from "dyna:net";

const app = new App({ port: 8080 });
app.rpc("/api", { hello: (name) => "hello " + name });
app.start();
```

---

## Command line

```text
usage: dynajs [options] [file [args]]

-h  --help         list options
-e  --eval EXPR    evaluate EXPR
-i  --interactive  go to interactive mode
-m  --module       load as ES6 module (default=autodetect)
    --script       load as ES6 script (default=autodetect)
    --strict       force strict mode
-I  --include file include an additional file
    --std          make 'std' and 'os' available to the loaded script
-T  --trace        trace memory allocation
-d  --dump         dump the memory usage stats
    --no-unhandled-rejection  ignore unhandled promise rejections
-s                 strip all the debug info
    --strip-source strip the source code
    --io-threads N IO worker threads (default max(ncpu,4); 0 runs
                   every offload inline, the no-pool control)
-q  --quit         just instantiate the interpreter and quit
```

Top-level `await` works in `-e` and in scripts, so `dynajs -e 'import("dyna:uuid").then(u => print(u.v7()))'` is a valid one-liner.

---

## The standard library

Every module is documented function-by-function, with executable examples, in the [API reference](API.md).

### Data and files

| Module | What it provides |
|---|---|
| [`dyna:file`](API.md#dynafile) | Filesystem work: atomic writes, file locks, watchers, globs, temp files, platform directories |
| [`dyna:csv`](API.md#dynacsv) | CSV (RFC 4180): streaming reads, type inference, a file-backed table |
| [`dyna:dataframe`](API.md#dynadataframe) | Columnar frames in plain JS: filter, sort, group-by, join, window functions, quantiles — over TypedArrays |
| [`dyna:config`](API.md#dynaconfig) | Configuration files: TOML 1.0, INI, `.env`, front matter |
| [`dyna:structures`](API.md#dynastructures) | The containers JS never shipped: graphs (Dijkstra, A*, MST), heaps, segment trees, tries, LRU caches — every one persists via `serialize()`/`deserialize()` |

### Network and web

| Module | What it provides |
|---|---|
| [`dyna:http`](API.md#dynahttp) | The web platform: WHATWG `fetch`/`Request`/`Response`/`Headers`/`FormData`, HTTP servers, WebSockets, multipart and header codecs |
| [`dyna:net`](API.md#dynanet) | Everything under HTTP: TCP/UDP sockets, a DNS client and server, Redis/PostgreSQL/SQLite clients, rate limiting, metrics, an L4 proxy (import this or `dyna:http`, not both) |
| [`dyna:url`](API.md#dynaurl) | URLs to spec: WHATWG parsing (WPT-conformant), IDNA 2008/UTS #46, Punycode, form encoding |
| [`dyna:html`](API.md#dynahtml) | Web content, server-side: an HTML5 tokenizer, CSS-selector queries, a sanitizer, markdown, templating |
| [`dyna:scrape`](API.md#dynascrape) | Polite crawling: robots.txt with token matching, per-host pacing, jittered retries, conditional GETs, response caps |
| [`dyna:uring`](API.md#dynauring) | Linux-only batched disk I/O over io_uring — many block reads per submit/complete cycle (built with `CONFIG_IO_URING`) |

### Security

| Module | What it provides |
|---|---|
| [`dyna:crypto`](API.md#dynacrypto) | AEAD encryption (AES-GCM, ChaCha20-Poly1305), signatures (RSA, ECDSA, Ed25519), key exchange, Argon2id/scrypt/bcrypt, X.509 certificates, JWT — constant-time wherever secrets are compared |
| [`dyna:oauth2`](API.md#dynaoauth2) | OAuth 2.0 helpers: PKCE S256, state CSRF, bearer auth, `WWW-Authenticate`, scope and `redirect_uri` checks |
| [`dyna:hash`](API.md#dynahash) | Digests: SHA-1/2, SHA-3, BLAKE2/3, CRC32 (hardware-accelerated), xxHash — one-shot or streaming |
| [`dyna:schema`](API.md#dynaschema) | JSON Schema validation (Draft 2020-12); schemas compile once, validation is pure dispatch |

### Text and formats

| Module | What it provides |
|---|---|
| [`dyna:bytes`](API.md#dynabytes) | Raw binary: build, slice and search byte buffers, fixed-width reads/writes, UTF-8-safe `Text` strings |
| [`dyna:encoding`](API.md#dynaencoding) | Encodings: charset detection, base64/base32/hex, varints, JSON5, JSONPath, QR codes |
| [`dyna:json`](API.md#dynajson) | JSON editing: Pointer (RFC 6901) lookups, Patch (RFC 6902) with copy-on-write |
| [`dyna:xml`](API.md#dynaxml) | XML: streaming SAX, a tree, or a plain object — size caps on by default |
| [`dyna:yaml`](API.md#dynayaml) | YAML 1.2; anchors and aliases are refused by name, never silently dropped |
| [`dyna:compress`](API.md#dynacompress) | zstd, brotli, snappy, gzip (dynamic-Huffman from level 6), LZ4, tar, zip — with deterministic bomb rejection |
| [`dyna:matcher`](API.md#dynamatcher) | Text search: Levenshtein, tries, Bloom filters, Aho-Corasick multi-pattern search, diffs |
| [`dyna:serialize`](API.md#dynaserialize) | Binary interchange: protobuf wire codec, ASN.1 DER, MessagePack, CBOR, BSON |

### Math, ML and compute

| Module | What it provides |
|---|---|
| [`dyna:simd`](API.md#dynasimd) | Explicit vector math: reductions, dot products, distances, matrix ops — the fastest kernel your CPU supports, picked at startup |
| [`dyna:ml`](API.md#dynaml) | Models in-process: trees, forests, k-NN, k-means, DBSCAN, linear/logistic regression, SVMs, gradient boosting, PCA, scalers — C over contiguous doubles, with persistence |
| [`dyna:mathx`](API.md#dynamathx) | The math toolbox: statistics, combinatorics, number theory, special functions, linear algebra |
| [`dyna:decimal`](API.md#dynadecimal) | Exact decimal and money arithmetic (IEEE decimal128, 34 digits) — no binary-float rounding surprises |
| [`dyna:random`](API.md#dynarandom) | OS-entropy randomness plus a seedable PRNG whose full state checkpoints and replays |

### Systems and operations

| Module | What it provides |
|---|---|
| [`dyna:sys`](API.md#dynasys) | The OS boundary: subprocesses, environment, arguments, CPU/memory facts |
| [`dyna:log`](API.md#dynalog) | Structured logging: one JSON object per line, leveled, pipe-friendly |
| [`dyna:cli`](API.md#dynacli) | Command-line tools: argument parsing, ANSI styling, prompts, progress bars |
| [`dyna:time`](API.md#dynatime) | Dates that work: RFC 5545 recurrence (`RRule`), `Duration`, civil calendar types, time zones |

### Identity and validation

| Module | What it provides |
|---|---|
| [`dyna:uuid`](API.md#dynauuid) | UUID v1–v7 from the OS CSPRNG, plus NanoID and ULID |
| [`dyna:semver`](API.md#dynasemver) | Version numbers: semver 2.0.0 parsing, ranges, comparison, incrementing |
| [`dyna:validate`](API.md#dynavalidate) | One-line checks: email, URL, UUID, credit card, IBAN, JWT — boolean predicates |

Core engine extensions (also in the reference): `Array`, `String`, `Number`, `Object`, `Date`, `RegExp` extras.

---

## Core concepts

**Modules.** Standard-library functionality lives in `dyna:*` ES modules — `import { fetch } from "dyna:http"`, `import { DataFrame } from "dyna:dataframe"`. Import `dyna:net` or `dyna:http`, not both: `dyna:net` re-exports the web platform plus the socket/database family, and the two registrations share the reactor.

**Web-platform globals.** `fetch`, `Request`, `Response`, `Headers`, `FormData`, `AbortController`, `TextEncoder`, `TextDecoder`, `console`, and the timer family exist on `globalThis` without imports — scripts written against web APIs run unmodified.

**The binary is the unit of deployment.** A DynaJS program depends on exactly the binary that runs it; there is no runtime version to match, no extension ABI to satisfy, and no transitive dependency to audit. Upgrading a service is replacing one file.

**Failures are named, not silent.** Library calls that refuse input throw with the reason in the message (`"gzip: level must be a number"`, `"invalid function bytecode (var_ref_idx)"`); parsers bound untrusted input with caps that reject rather than truncate. The README and API reference examples are executed by the build's own documentation gate, so what the docs claim is what the binary does.

---

## Recipes: complete programs

Each recipe is one file, uses several modules together, and — except where an external service is required — is executed verbatim by the documentation gate against the current binary.

### 1. A JSON-RPC microservice with auth, rate limiting and metrics

One file: a routed service over the async server, JWT bearer authentication, per-client token-bucket rate limiting, Prometheus metrics, and structured logs. The client and server run in the same script, so the block is self-contained and runs in the gate.

```js
import { App, RateLimiter, Metrics } from "dyna:net";
import { JWTSign } from "dyna:crypto";

const SECRET = new TextEncoder().encode("rotate-me-every-90-days-32-bytes!!");
const limiter = new RateLimiter({ tokensPerSec: 5, burst: 10 });

const app = new App({ port: 0 });
app.rpc("/v1/orders", {
    // JSON-RPC method handlers run on the JS thread
    create: (sku, qty) => {
        Metrics.counter("orders_created_total", 1, { sku });
        return { sku, qty, status: "accepted" };
    },
});
app.start();

// Issue a short-lived access token, then call the service as a client
const now = Math.floor(Date.now() / 1000);
const token = JWTSign({ sub: "svc-ingest", exp: now + 300 },
                       SECRET, { alg: "HS256" });

const res = await fetch(`http://127.0.0.1:${app.port}/v1/orders`, {
    method: "POST",
    headers: { "Content-Type": "application/json",
               "Authorization": "Bearer " + token },
    body: JSON.stringify({ jsonrpc: "2.0", id: 1,
                           method: "create", params: ["SKU-42", 3] }),
});
const reply = await res.json();
console.log("rpc result:", JSON.stringify(reply.result));

// The rate limiter is the same object the middleware would consult
console.log("limiter admits burst:", limiter.allow("svc-ingest"));
Metrics.counter("rpc_requests_total", 1);
console.log("metrics series:", Metrics.scrape().includes("rpc_requests_total"));
app.close();
```

### 2. Access-log analysis with URL parsing and DataFrames

Parse a web-server access log, extract structured fields, and produce a report — the kind of one-off analysis that usually lives in a shell pipeline, here with typed columns and group-bys.

```js
import { DataFrame } from "dyna:dataframe";
import { URL } from "dyna:url";

const lines = [
    '2026-09-04T10:00:01Z GET /api/users 200 12ms',
    '2026-09-04T10:00:04Z GET /api/orders 200 38ms',
    '2026-09-04T10:00:09Z POST /api/orders 201 51ms',
    '2026-09-04T10:00:15Z GET /api/users 200 9ms',
    '2026-09-04T10:00:22Z GET /static/app.js 304 2ms',
    '2026-09-04T10:00:30Z GET /api/orders 500 120ms',
];

const rows = lines.map(l => {
    const [ts, method, rawPath, status, ms] = l.split(" ");
    const u = new URL("http://log.local" + rawPath);   // WHATWG parsing
    return { api: u.pathname.startsWith("/api/"),
             path: u.pathname, method, status: Number(status),
             ms: Number(ms.replace("ms", "")) };
});

const df = new DataFrame({
    api:   rows.map(r => r.api),
    path:  rows.map(r => r.path),
    method: rows.map(r => r.method),
    status: new Int32Array(rows.map(r => r.status)),
    ms:    new Float64Array(rows.map(r => r.ms)),
});

const api = df.FILTER(df.ISIN("api", [true]));
const byPath = api.GROUP_BY_MEAN("path", "ms");
console.log("api requests:", api.ROWS, "of", df.ROWS);
byPath.keys.forEach((k, i) =>
    console.log(`  ${k}: mean ${byPath.values[i].toFixed(1)}ms`));
console.log("slowest:", Math.max(...df.HEAD("ms", df.ROWS)), "ms");
```

### 3. ETL: CSV in, model out, compressed archive on disk

Read a sales CSV, aggregate into a frame, cluster customers with k-means, write the scored output as JSON, and leave a compressed archive — one process, no data ever leaves memory until the writes.

```js
import { writeFile, readFile, makeTempDir, Path, File, removeAll } from "dyna:file";
import { CSVFile } from "dyna:csv";
import { DataFrame } from "dyna:dataframe";
import { KMeans } from "dyna:ml";
import { gzip } from "dyna:compress";
import { SHA256Hex } from "dyna:hash";

const dir = makeTempDir("etl");
const src = new Path(String(dir) + "/sales.csv");
writeFile(src, "cust,units,revenue,days_since\n" +
               "c1,3,120,4\nc2,9,810,2\nc3,2,60,40\n" +
               "c4,11,990,1\nc5,4,150,35\nc6,8,720,6\n");

const table = new CSVFile(src).read();               // file-backed table
const frame = new DataFrame({
    cust:   table.rows.map(r => r[0]),
    units:  new Int32Array(table.rows.map(r => Number(r[1]))),
    revenue: new Float64Array(table.rows.map(r => Number(r[2]))),
    recency: new Int32Array(table.rows.map(r => Number(r[3]))),
});

// Behavioral segments: volume vs recency, standardized by construction here
const features = frame.ROWS > 0 ? table.rows.map(r => [Number(r[1]), Number(r[3])]) : [];
const model = new KMeans(2, 7);                      // nClusters, seed
model.fit(features);
const scored = table.rows.map((r, i) => ({
    cust: r[0], revenue: Number(r[2]), segment: model.predict([features[i]])[0],
}));

const outPath = new Path(String(dir) + "/segments.json");
const payload = JSON.stringify(scored);
writeFile(outPath, payload);
console.log("customers scored:", scored.length,
            "| digest:", SHA256Hex(payload).slice(0, 16));

// Ship the raw extract as a single compressed artifact with an checksum
const archive = new Path(String(dir) + "/sales.json.gz");
writeFile(archive, gzip(JSON.stringify(table.rows), 9));
console.log("archive bytes:", new File(archive).readBytes().length > 40);
removeAll(dir);
```

### 4. An encrypted document vault

Key derivation from a passphrase, authenticated encryption bound to document metadata (tampering with the context fails the decryption), atomic writes, and integrity digests — the skeleton of an at-rest encryption layer.

```js
import { AESGCM, Argon2id, RandomBytes } from "dyna:crypto";
import { SHA256Hex } from "dyna:hash";
import { writeFile, makeTempDir, Path, File, removeAll } from "dyna:file";

// Derive the vault key from the passphrase (memory-hard by construction)
const key = Argon2id.hash("correct horse battery staple", RandomBytes(16),
                           { memory: 65536, iterations: 3, parallelism: 1,
                             hashLen: 32 });
const aead = new AESGCM(key);

const doc = JSON.stringify({ patient: "x-4102", diagnosis: "stable",
                             recorded: "2026-09-04" });

// The AAD binds the ciphertext to its context: swapping document IDs
// between patients turns decryption into an authentication failure
const sealed = aead.sealRandom(doc, "doc:x-4102");
const dir = makeTempDir("vault");
const at = new Path(String(dir) + "/x-4102.bin");
writeFile(at, sealed.sealed);
console.log("stored", sealed.sealed.length, "bytes; nonce kept with the id:",
            sealed.nonce.length === 12);

const stored = new File(at).readBytes();
const readBack = aead.open(sealed.nonce, stored, "doc:x-4102");
console.log("round trip:", new TextDecoder().decode(readBack) === doc);

let tamperCaught = false;
try {
    aead.open(sealed.nonce, stored, "doc:x-9999");
} catch (e) { tamperCaught = true; }                 // wrong context = refused
console.log("context swap refused:", tamperCaught);
console.log("integrity digest:", SHA256Hex(doc).slice(0, 16));
removeAll(dir);
```

### 5. Polite scraping with extraction and dedup

A crawler skeleton: robots policy with token matching, per-host pacing and jittered retries from `dyna:scrape`; CSS-selector extraction from `dyna:html`; URL canonicalization from `dyna:url`; a set for seen-content dedup. Requires the network, so this block is skipped by the documentation gate.

<!-- check:skip -->
```js
import { Fetcher, Extractor } from "dyna:scrape";
import { HTMLParse, Selector, HTMLText } from "dyna:html";
import { URL } from "dyna:url";
import { SHA256Hex } from "dyna:hash";
import { Logger } from "dyna:log";

const log = new Logger({ name: "crawler" });
const fetcher = new Fetcher({ agent: "research-bot/1.0 (contact: ops@co)",
                               robots: true, paceMs: 1500, jitter: 0.3 });

const extract = new Extractor({
    title:   { sel: new Selector("h1") },
    links:   { sel: new Selector("a"), attr: "href", all: true },
}, { text: HTMLText });

const seen = new Set();
for (const start of ["https://example.com/docs", "https://example.com/blog"]) {
    const res = await fetcher.get(start);           // honors robots + pacing
    if (!res.ok) { log.warn("fetch_failed", { url: start, status: res.status }); continue; }
    const doc = HTMLParse(res.text);
    const out = extract.run(doc);
    const fingerprint = SHA256Hex(out.value.title ?? "");
    if (seen.has(fingerprint)) { log.info("duplicate", { url: start }); continue; }
    seen.add(fingerprint);
    log.info("indexed", { url: start, title: out.value.title,
                          links: out.value.links.length });
    // Canonicalize and follow same-host links only
    const next = out.value.links
        .map(h => new URL(h, start))
        .filter(u => u.host === new URL(start).host)
        .map(u => u.href);
    console.log("queueing:", next.length, "same-host links");
}
```

### 6. A SQLite-backed inventory CLI

A command-line tool over the embedded database: styled tables, argument parsing, validation — the shape of an internal ops tool, as a single binary.

```js
import { SQLite } from "dyna:net";
import { StyleText, Styles, IsTTY } from "dyna:cli";

const db = new SQLite(":memory:");
db.exec("CREATE TABLE inventory(sku TEXT, qty INTEGER, price REAL)");
for (const [sku, qty, price] of [["A1", 10, 1.5], ["B2", 3, 9.0],
                                  ["C3", 7, 2.25], ["D4", 0, 12.0]])
    db.exec("INSERT INTO inventory VALUES (?, ?, ?)", [sku, qty, price]);

const low = db.query("SELECT sku, qty FROM inventory WHERE qty < ?", [5]);
const value = db.query("SELECT SUM(qty * price) AS total FROM inventory")[0];
console.log(StyleText(IsTTY() ? "green" : "reset",
                      `inventory value: $${value.total.toFixed(2)}`));
for (const row of low)
    console.log(`  RESTOCK ${row.sku} (qty ${row.qty})`);
```

### 7. Static site with health metrics

A thread-pool server serving static assets with a metrics endpoint and structured access logs — the deployable half of recipe 1, verified here against the blocking client.

```js
import { HTTPServer, HTTPClient } from "dyna:net";
import { Logger } from "dyna:log";

const log = new Logger({ name: "edge" });
const server = new HTTPServer({
    port: 0, workers: 2,
    routes: {
        "/":        { status: 200, contentType: "text/html",
                     body: "<h1>status ok</h1>" },
        "/health":  { status: 200, contentType: "application/json",
                     body: '{"ok":true}' },
        "/missing": { status: 404, contentType: "text/plain", body: "no" },
    },
});
server.start();

const client = new HTTPClient();
const health = client.get(`http://127.0.0.1:${server.port}/health`);
log.info("request", { path: "/health", status: health.status });
console.log("health:", health.status, JSON.parse(health.body).ok);
const missing = client.get(`http://127.0.0.1:${server.port}/missing`);
console.log("unknown route:", missing.status);
client.close();
server.close();
```

### 8. Ship the program as one executable

Every recipe above is a JS file. `dynajsc` compiles it to bytecode and links the engine in statically — the target machine needs the binary and nothing else:

```sh
make examples/hello    # examples/hello.js -> ./examples/hello (native)
./examples/hello
```

The program keeps its interpreter, its standard library, and its garbage collector in that one file; the target needs nothing else on disk.

---

## Embedding DynaJS

DynaJS is a library first. The engine builds as `libdynajs.a` for direct embedding, and [recipe 8](#8-ship-the-program-as-one-executable) shows the `dynajsc` path that turns any of the programs above into a single self-contained native executable. The public headers are `src/dynajs.h` and `src/dyna-libc.h`; `examples/` holds three patterns — static JS compilation (`hello.js`), ES modules (`hello_module.js`), and an external native module (`fib.c` → `fib.so`).

---

## Building from source

```sh
# Full engine with the native standard library and TLS
make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y

# Minimal engine
make
```

Build-configuration flags (all are optional; each selects a feature or a strategy):

| Flag | Effect |
|---|---|
| `CONFIG_NATIVE_MODULES=y` | Compile the `dyna:*` standard library in |
| `CONFIG_TLS=y` | OpenSSL-backed TLS: HTTPS, RSA/ECDSA, X.509, AEAD ciphers |
| `CONFIG_SQLITE=y` | Force SQLite support (auto-detected via pkg-config otherwise) |
| `CONFIG_ZSTD=y` | Force zstd support (auto-detected) |
| `CONFIG_IO_URING=y` | Linux io_uring async backend |
| `CONFIG_CLANG=y` | Use clang instead of gcc on Linux |
| `CONFIG_OPENLIBM=y` | Link the vendored openlibm for reproducible numerics |
| `CONFIG_ASAN=y` / `CONFIG_UBSAN=y` / `CONFIG_TSAN=y` | Sanitizer builds |
| `CONFIG_ML_NO_SIMD=y` | Strictly sequential ML kernels (the oracle control) |

A flag change is not tracked by make; run `make clean` after changing flags, or a stale object will silently drop conditionally-compiled code. The SVE kernels are the compile-gated exception to the runtime SIMD dispatch: they require building with `-march=armv8-a+sve`, so a default arm64 build runs NEON even on SVE-capable hardware.

---

## Performance

- **SIMD kernels are a runtime-dispatched portfolio** — scalar, NEON, SSE4.2, AVX2, AVX-512, SVE — the fastest kernel the CPU can actually run is chosen at startup and is observable (`simd.active()`), so a deployment can audit which code path it executes.
- **Numeric work is columnar.** DataFrames store typed columns over TypedArrays; ML kernels reduce over contiguous doubles; string scanning dispatches through the same runtime-selected kernels.
- **The HTTP server is syscall-bound, not CPU-bound.** Per-request work is bounded, peer-demanded work is capped, and slow peers cannot block the reactor — blocking operations (file copies, large hashes, uploads) offload to the IO pool so the event loop stays live.
- **Compression selects by level.** gzip emits fixed-Huffman blocks below level 6 and builds dynamic-Huffman blocks from the input's own statistics from level 6 up, whichever is smaller per block; output never expands (stored-block fallback).
- **`bench/` holds the benchmark suites** (parser, regexp, stdio, numeric, the C cores); each covers the area it is named for. Run the one that covers what you touched.

---

## How correctness is enforced

The repository treats verification as part of the product, not a phase. Every push passes a ten-stage pre-push gate, installed as the `pre-push` git hook: source-shape analysis (struct padding, function complexity, hand-written link lines — regressions no compiler reports), a zero-warning clean build, fuzz-target link proofs, an import/orphan audit, the 39-suite core engine run, the 175-suite native-module run, the eight-layer API audit (every documented name exists in the binary and runs; every `README`/`API.md` example block executes against the current build), the adversarial security suites, a pty-driven REPL harness, and the TLS/AEAD vector checks.

A fuller local battery (`./dev.sh gate`) adds sanitizer legs on top: ASan and UBSan over the complete suite, TSan over the threaded HTTP paths, and test262 at a pinned baseline. WHATWG WPT-URL conformance sits at **zero recorded failures** (426 → 0 during the 2026 audit), regression-locked by the conformance suite.

Three libc/ISA legs (glibc, musl, emulated amd64) run in containers with cached toolchains, so a warm leg recompiles only what changed. The hostile-input posture is regression-locked: hand-crafted bytecode blobs that previously crashed the reader are a permanent test target, and the reader rejects them with named errors.

---

## Project layout

```text
src/            engine + stdlib sources (dyna-*.c per module; src/core/* pure C)
tests/          JS test suites (test_*.js) + shell harnesses (test_*.sh)
examples/       runnable JS examples, embedding demos, fib native module
tools/          code generators, doc/example checkers, api-inventory
bench/          benchmark suites and the code-graph analyzer (bench/codegraph.py)
docker/         container gates: Dockerfile (cached toolchains), build-and-test.sh, linux.sh
API.md          the API reference — every function, with executable examples
third_party/    vendored openlibm (linked only with CONFIG_OPENLIBM)
dev.sh          the developer entry point (build/asan/ubsan/t262/bench/gate)
install.sh      end-user installer (clone, build, install, verify, upgrade)
Makefile        single build system: binary, libdynajs.a, every gate target
```

---

## AI agent guide

This section is written for coding agents working in this repository. Read it before editing.

### Commands

```sh
make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y        # the canonical dev binary
./dev.sh gate                                    # full proof gate (build+ASan+UBSan+test+test262)
make prepush                                     # the 10-stage pre-push gate (what the git hook runs)
python3 bench/codegraph.py . N                   # source-shape analyzer; first arg is the ROOT
make ccheck FILE=src/foo.c                       # single-TU syntax check in seconds
./dev.sh mod csv,net                             # per-module slim-binary tests, no full build
./dev.sh ws create NAME                          # isolated concurrent workspace; ws diff|merge to converge
bash docker/build-and-test.sh                    # glibc/musl/amd64 legs, parallel, cached images
```

### The order that keeps you fast

1. Make ALL edits first, without compiling.
2. Static checks that need no binary: shellcheck for scripts, `python3 bench/codegraph.py` for source shape (padding, complexity, hand-written link lines).
3. Build once, then run the whole gate — never edit source while a gate runs.

### Verification is load-bearing

- The documentation gates execute every ```` ```js ```` block in `README.md` and `API.md` against the current binary (`make check-readme`, `make check-api`). A documented call must exist and run; a block that cannot run standalone must carry `<!-- check:skip -->` on the line before its fence. Skipped blocks are counted and printed.
- Doc prose must be present tense (`tools/doc-lint.sh`); internal links must resolve (`tools/check-anchors.py`).
- `tools/api-inventory.js` enumerates the binary's exported names — the API reference is generated against it, and a new export without a test shows up as a gap there.
- New code on the untrusted frontier (parsers, deserializers, anything a peer can feed) needs a refusal list, N-1/N/N+1 boundary cases, and a fuzz target from day one.

### What not to break

- Every value from a native API call is owned; free exactly once on every path including errors.
- `JSFunctionBytecode.debug` must stay the LAST member.
- Build flags do not trigger rebuilds: `make clean` after changing any `CONFIG_*` flag.
- A native module passes a `Uint8Array` to JS handlers, never a bare `ArrayBuffer`.
- Keep the Makefile's hand-written fuzz link lines in sync when a core source gains a dependency.

---

## TypeScript and IDE support

Ambient declarations for every `dyna:*` module, the WHATWG globals, and the core prototype extensions ship in [`dynajs.d.ts`](dynajs.d.ts). Reference it from your `tsconfig.json` / `jsconfig.json`:

```json
{
    "compilerOptions": {
        "types": ["dynajs"]
    }
}
```

That gives autocomplete, hover documentation and typechecking over the full standard library — the declarations are generated against the binary's own export inventory and the gate (`make check-types`) fails when they drift.

---

## License and docs

- Source of truth for every function: [API reference](API.md)
- Issues: https://github.com/corporatepiyush/dynajs/issues
