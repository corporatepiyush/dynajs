# DynaJS

**A JavaScript runtime with the standard library compiled in. One binary, nothing to install.**

DynaJS is an embeddable JavaScript engine and application runtime written in C, built on [QuickJS](https://bellard.org/quickjs/) and heavily modified from it. A single executable carries the interpreter, the garbage collector and 38 `dyna:*` modules: HTTP and `fetch`, TLS, authenticated encryption, SQLite, Redis and PostgreSQL clients, DataFrames, machine-learning models, streaming compression, and parsers for the formats real programs meet.

Write a script and run it. Or compile it into a native executable, copy that one file to a server, and run it there. No package manager takes part at any point, and nothing is downloaded at runtime.

> **DynaJS 1.0 is released.** The engine and standard library are stable and conformance-tested against test262, and the API described in [`dynajs.d.ts`](dynajs.d.ts) is the 1.0 contract.

## Contents

- [Why DynaJS](#why-dynajs)
- [Install](#install)
- [Quick start](#quick-start)
- [A taste of the library](#a-taste-of-the-library)
- [The standard library](#the-standard-library)
- [How DynaJS behaves](#how-dynajs-behaves)
- [Native first: let C run the loops](#native-first-let-c-run-the-loops)
- [Recipes](#recipes)
- [Example programs](#example-programs)
- [Command line](#command-line)
- [Shipping and embedding](#shipping-and-embedding)
- [Building from source](#building-from-source)
- [Reliability](#reliability)
- [TypeScript and editor support](#typescript-and-editor-support)
- [License and links](#license-and-links)

---

## Why DynaJS

**One artifact.** A deployed DynaJS program is a single file. It links only libraries the operating system already maintains (OpenSSL, SQLite, zstd, brotli), so there is no dependency directory to vendor, no runtime version to match, and no registry in your supply chain.

**The batteries are in the binary.** Servers, sockets, databases, cryptography, data frames, models, codecs and parsers are `import`s, not installs. A service that needs JWT, Postgres and gzip has three import lines and zero dependencies.

**A real engine.** DynaJS started from QuickJS, Fabrice Bellard and Charlie Gordon's compact interpreter, and has diverged a long way: the parser, bytecode compiler, interpreter and garbage collector have been reworked, and the runtime around them (event loop, reactor, the whole `dyna:*` library) is new. It does not track upstream. The language is ES2023: classes, generators, async/await, ES modules, destructuring, `BigInt`, typed arrays, and top-level `await`.

**It says no out loud.** A misspelled option throws and names the valid ones. A decompression bomb is refused at an exact, documented size. Half a cent is never rounded into a ledger. Input from the network is bounded before it is believed.

**Predictable.** There is no JIT, so there is no warm-up phase and no deoptimization cliff: the first request runs the same code as the millionth. Numeric work runs on vector kernels chosen for the CPU at startup.

**Embeddable.** The engine is a static library, and a compiler turns a script into a self-contained native executable with no JavaScript source inside.

---

## Install

One command on macOS, Linux or FreeBSD:

```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynajs/master/install.sh | bash
```

The installer builds from source with the full standard library and puts one binary at `/usr/local/bin/dynajs` (or `$HOME/.local/bin` when that needs root and sudo is unavailable). It asks no questions, so it is safe to pipe. Running it again upgrades.

```sh
install.sh --prefix "$HOME/.local"   # install to a different prefix
install.sh --with-deps               # also install missing build tools (brew/apt/dnf/...)
install.sh --dry-run                 # print the plan and preflight report, install nothing
install.sh --uninstall               # remove the installed binary
install.sh --verbose                 # stream the build output
```

A first install needs `git`, `make` and a C compiler (clang preferred); on macOS the Xcode Command Line Tools provide all three. The preflight report says exactly what is missing before anything is downloaded.

---

## Quick start

```js
// hello.js: the standard library is already there
import { zstd, unzstd } from "dyna:compress";
const data = new Uint8Array(Array.from({ length: 1000 }, (_, i) => i % 7));
console.log("round trip ok:", unzstd(zstd(data)).every((v, i) => v === data[i]));
```

```sh
dynajs hello.js            # run a file
dynajs -e 'print(1 + 1)'   # evaluate an expression
dynajs -i                  # interactive REPL with tab-completion
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

And an HTTP service is a file, not a project. This one binds a port and runs until stopped; [recipe 1](#1-a-json-rpc-service-with-auth-rate-limiting-and-metrics) is the self-contained version.

<!-- check:skip -->
```js
import { App } from "dyna:net";

const app = new App({ port: 8080 });
app.rpc("/api", { hello: (name) => "hello " + name });
app.get("/users/:id", (req) => ({ id: req.params.id }));
app.start();
```

---

## A taste of the library

Six modules, one file, no setup:

```js
import { pmap } from "dyna:async";
import { Money } from "dyna:decimal";
import { LRU } from "dyna:structures";
import { parseDurationMs } from "dyna:time";
import { Levenshtein } from "dyna:matcher";
import { dot } from "dyna:simd";

// Bounded concurrency, results in input order
const lengths = await pmap(["oslo", "lima", "kyoto"], async (city) => {
    await sleep(5);
    return city.length;
}, { concurrency: 2 });
console.log("pmap:", lengths.join(","));

// Money is integer minor units: no binary-float drift, and no silent rounding
const price = Money.fromString("19.99", "USD");
let refused = false;
try { Money.fromString("19.999", "USD"); } catch (e) { refused = true; }
console.log("money:", String(price), "| a fraction of a cent is refused:", refused);

// Containers, durations, fuzzy matching, vector math
const cache = new LRU(2);
cache.set("a", 1).set("b", 2).set("c", 3);
console.log("lru evicted the oldest:", !cache.has("a"));
console.log("1h30m =", parseDurationMs("1h30m"), "ms");
console.log("edit distance:", Levenshtein("kitten", "sitting"));
console.log("dot:", dot(new Float32Array([1, 2, 3]), new Float32Array([4, 5, 6])));
```

---

## The standard library

[`dynajs.d.ts`](dynajs.d.ts) is the reference: every function with a one-line contract. [`examples/`](examples) holds complete, self-testing programs to copy from.

### Data and files

| Module | What it provides |
|---|---|
| `dyna:stream` | Pull-based byte streaming: `pipe()`, file sources and sinks, chunk-safe `lines()`/`ndjson()` iterators, streaming zstd/brotli/lz4/gzip codecs |
| `dyna:file` | Filesystem work: atomic writes, file locks, watchers, globs, temp files, platform directories |
| `dyna:csv` | CSV (RFC 4180): streaming reads, type inference, a file-backed table, in-memory `parse`/`stringify` with custom delimiters |
| `dyna:dataframe` | Columnar frames in plain JS: filter, sort, group-by, join, window functions, quantiles — over TypedArrays |
| `dyna:config` | Configuration files: TOML 1.0, INI, `.env`, front matter |
| `dyna:structures` | The containers JS never shipped: graphs (Dijkstra, A*, MST), heaps and indexed heaps, B-trees, sorted string maps, Redis-style sorted sets, roaring bitmaps, k-d trees, a BM25 full-text index, consistent-hash rings, t-digest/top-k/cuckoo/HyperLogLog sketches, tries, LRU caches — every one persists via `serialize()`/`deserialize()` |

### Network and web

| Module | What it provides |
|---|---|
| `dyna:http` | The web platform: WHATWG `fetch`/`Request`/`Response`/`Headers`/`FormData`, HTTP servers, WebSockets, multipart and header codecs |
| `dyna:net` | Everything under HTTP: TCP/UDP sockets, a DNS client and server, Redis/PostgreSQL/SQLite clients, rate limiting, metrics, an L4 proxy (import this or `dyna:http`, not both) |
| `dyna:url` | URLs to spec: WHATWG parsing (WPT-conformant), IDNA 2008/UTS #46, Punycode, form encoding |
| `dyna:html` | Web content, server-side: an HTML5 tokenizer, CSS-selector queries, a sanitizer, markdown, templating |
| `dyna:scrape` | Polite crawling: robots.txt with token matching, per-host pacing, jittered retries, conditional GETs, response caps |
| `dyna:uring` | Linux-only batched disk I/O over io_uring — many block reads per submit/complete cycle (built with `CONFIG_IO_URING`) |

### Security

| Module | What it provides |
|---|---|
| `dyna:crypto` | AEAD encryption (AES-GCM, ChaCha20-Poly1305), signatures (RSA, RSA-PSS, ECDSA, Ed25519), RSA-OAEP sealing, key exchange, Argon2id/scrypt/bcrypt, HOTP/TOTP verification, X.509 certificates, JWT — constant-time wherever secrets are compared |
| `dyna:oauth2` | OAuth 2.0 helpers: PKCE S256, state CSRF, bearer auth, `WWW-Authenticate`, scope and `redirect_uri` checks |
| `dyna:hash` | Digests: SHA-1/2, SHA-3, BLAKE2/3, CRC32 (hardware-accelerated), xxHash — one-shot or streaming |
| `dyna:schema` | JSON Schema validation (Draft 2020-12); schemas compile once, validation is pure dispatch |

### Text and formats

| Module | What it provides |
|---|---|
| `dyna:bytes` | Raw binary: build, slice and search byte buffers, fixed-width reads/writes, UTF-8-safe `Text` strings |
| `dyna:encoding` | Encodings: charset detection, base64/base32/hex, varints, JSON5, JSONPath, QR codes |
| `dyna:json` | JSON editing: Pointer (RFC 6901) lookups, Patch (RFC 6902) with copy-on-write |
| `dyna:xml` | XML: streaming SAX, a tree, or a plain object — size caps on by default |
| `dyna:yaml` | YAML 1.2; anchors and aliases are refused by name, never silently dropped |
| `dyna:compress` | zstd, brotli, snappy, gzip (dynamic-Huffman from level 6), LZ4, tar, zip — with deterministic bomb rejection |
| `dyna:matcher` | Text search: Levenshtein, tries, Bloom filters, Aho-Corasick multi-pattern search, diffs |
| `dyna:serialize` | Binary interchange: protobuf wire codec, ASN.1 DER, MessagePack, CBOR, BSON |

### Math, ML and compute

| Module | What it provides |
|---|---|
| `dyna:simd` | Explicit vector math: reductions, dot products, distances, matrix ops — the fastest kernel your CPU supports, picked at startup |
| `dyna:ml` | Models in-process: trees, forests, k-NN, k-means, DBSCAN, linear/logistic regression, SVMs, gradient boosting, PCA, scalers — C over contiguous doubles, with persistence. Every fit, predict and metric returns a promise and computes on a thread of its own, so timers and sockets keep being served while a model trains |
| `dyna:mathx` | The math toolbox: statistics, combinatorics, number theory, special functions, linear algebra |
| `dyna:decimal` | Exact decimal and money arithmetic (IEEE decimal128, 34 digits) — no binary-float rounding surprises |
| `dyna:random` | OS-entropy randomness plus a seedable PRNG whose full state checkpoints and replays: normal/exponential/Poisson draws, shuffle/sample/choice, byte fills, `jump()`/`longJump()` |

### Systems and operations

| Module | What it provides |
|---|---|
| `dyna:sys` | The OS boundary: subprocesses, environment, arguments, CPU/memory facts |
| `dyna:log` | Structured logging: one JSON object per line, leveled, pipe-friendly |
| `dyna:cli` | Command-line tools: argument parsing, ANSI styling, prompts, progress bars |
| `dyna:time` | Dates that work: RFC 5545 recurrence (`RRule`), `Duration`, civil calendar types (`PlainDate`, `PlainTime`, `PlainDateTime`), time zones, duration parsing (`"1h30m"` → milliseconds) |

### Identity and validation

| Module | What it provides |
|---|---|
| `dyna:uuid` | UUID v1–v8 from the OS CSPRNG, plus NanoID and ULID with a monotonic mode |
| `dyna:semver` | Version numbers: semver 2.0.0 parsing, ranges, comparison, incrementing |
| `dyna:validate` | One-line checks: email, URL, UUID, credit card, IBAN, JWT, IPv4/IPv6, ports, base64, hex, hex colors, JSON, MIME types, RFC 3339 timestamps, strong passwords — boolean predicates |

### Concurrency and measurement

| Module | What it provides |
|---|---|
| `dyna:async` | Promise tools: `pmap` with a concurrency limit, `retry` with backoff, `withTimeout`, `Semaphore`, `Channel`, `Pool`, `debounce`/`throttle` |
| `dyna:bench` | A timing harness for your own code: adaptive batching on the monotonic clock, percentiles, a results table |

The engine also extends the standard prototypes (`Array`, `String`, `Number`, `Object`, `Date`, `RegExp`, lazy `Iterator` helpers); see [Prototype extensions](#prototype-extensions).

---

## How DynaJS behaves

A handful of rules hold across the whole library. Learn them once and every module reads the same way.

**Modules and globals.** Library code lives in `dyna:*` ES modules. The web platform is global: `fetch`, `Request`, `Response`, `Headers`, `FormData`, `WebSocket`, `AbortController`, `URL`, `URLSearchParams`, `TextEncoder`, `TextDecoder`, `structuredClone`, `atob`/`btoa`, `queueMicrotask`, `console`, the timers, and a promise-based `sleep`. Code written against web APIs runs unmodified. Import `dyna:net` or `dyna:http`, not both: `dyna:net` re-exports the web surface next to sockets and database clients.

**Option bags are strict.** An unknown key is a `TypeError` that names the key and the valid set, such as `unknown option "levle" (valid: level)`. A typo can never quietly change behavior.

**Refuse, never guess.** Errors carry their reason in the message (`"gzip: level must be a number"`). Parsers reject malformed input instead of repairing it. Limits reject rather than truncate: a zip that would expand past the cap, a reply longer than the client allows and a YAML document with aliases are all refused by name.

**Untrusted input is bounded first.** Anything a peer or a file can feed (HTTP requests, Redis and PostgreSQL replies, archives, XML, serialized structures) has size, depth and count caps, and the caps are part of the documented contract. Secret comparisons run in constant time.

**You can own the buffers.** Hot paths have `*Into` forms (`digestInto`, `sealInto`, `predictInto`, `hexEncodeInto`) that write into a buffer you supply and allocate nothing. Streams follow the same idea: `read(buf)` fills your buffer and returns a count.

**Streams pull.** A `ByteSource` is any object with `read(buf)`, a `ByteSink` any object with `write(view)`, and `pipe()` connects the two. Because both are duck-typed, a fetch body, a subprocess pipe and a compressed file all plug into `lines()`, `ndjson()`, `inflate()` and `deflate()` without adapters:

```js
import { lines, ndjson, fromBytes } from "dyna:stream";

const cities = [];
for await (const line of lines(fromBytes("oslo\nlima\nkyoto\n"))) cities.push(line);
console.log("lines:", cities.join(" "));

const codes = [];
for await (const ev of ndjson(fromBytes('{"code":200}\n{"code":500}\n'))) codes.push(ev.code);
console.log("codes:", codes.join(","));
```

**Native resources close.** Servers, clients, file handles, models and hashers hold native memory. Each has `close()` (and `Symbol.dispose`); the garbage collector is the backstop, not the plan.

**One event loop, and blocking is labelled.** `fetch`, sockets, database clients, streams and subprocesses (`Spawn`) are asynchronous. The calls that block say so in their documentation: `HTTPClient`, `Exec`, and the password hashes. `dyna:async` gives interleaving on that one loop, not parallelism.

**Let native code run the loop.** DynaJS interprets bytecode and has no JIT, so a loop body of a few lines costs more than one native call that does the same work in C. The library is shaped for that: bulk forms (`addMany`, `pushMany`, `internMany`, `knnMany`, `quantiles`), whole-array kernels in `dyna:simd`, and containers that keep the per-element bookkeeping native (`ZSet`, `OrderedMap`, `RoaringBitmap`, `InvertedIndex`). Prefer them to hand-written loops over large inputs.

**Exact where money or reproducibility is involved.** `Decimal` and `Money` never pass through a binary float. `mathx.stats.sum` is compensated. A seeded `Random` replays its stream exactly and checkpoints its full state.

---

## Native first: let C run the loops

DynaJS interprets bytecode and has no JIT. Every operation inside a JavaScript loop body is dispatched one at a time, while one call into the library does the same work in compiled C, often on vector instructions selected for the CPU at startup. The fastest DynaJS program is therefore the one that writes the fewest loops itself. Before writing a loop over a large input, reach for one of three layers.

**`dyna:simd` for numbers in typed arrays.** Reductions (`sum`, `mean`, `variance`, `min`, `max`, `argmax`, `f64Sum`, `f64Dot`), element-wise arithmetic (`add`, `mul`, `scale`, `axpy`, `fma`, `clamp`), transcendental kernels (`vexp`, `vlog`, `vsqrt`, `sigmoid`, `softmax`), distances (`distL2`, `distCos`), `topkIndices`, `cumsum`, `gemv`/`gemm`, and indexed `gather`/`scatterAdd`. Keep numeric columns in `Float64Array` or `Float32Array` from the start so they can be handed to a kernel without conversion.

**`dyna:structures` for bookkeeping.** Heaps, ordered maps, sorted sets, `ZSet`, `Multiset`, `TopK`, `TDigest`, `HyperLogLog`, `BloomFilter`, `RoaringBitmap`, `InvertedIndex`, `Graph` (shortest paths, components, flows), `UnionFind`, `KDTree`, `IntervalTree`, `LRU`, `RingBuffer` and more. Each one keeps its per-element work native, and most have bulk forms (`addMany`, `pushMany`, `internMany`, `quantiles`) so a whole batch crosses into C in one call.

**Prototype extensions for the small loops.** About three hundred helpers on the standard prototypes, implemented in C, replace the hand-written `for` loops that every program accumulates: `Array.prototype.sum/mean/median/unique/groupBy/countBy/pluck/partition/splitEvery/zip`, `String.prototype.words/lines/capitalize/truncate/escapeHTML`, `Number.prototype.clamp/format/bytes`, `Object.pick/omit/path/mergeDeepRight`, and lazy `Iterator` helpers (`take`, `scan`, `uniq`, `groupBy`). They are non-enumerable, so `for..in` and `JSON.stringify` never see them.

```js
import { f64Dot, f64Mean, distCos, topkIndices } from "dyna:simd";
import { TDigest, TopK, RoaringBitmap } from "dyna:structures";

// Whole-array kernels: one call instead of a loop body per element.
const prices = Float64Array.from({ length: 100_000 }, (_, i) => 10 + (i % 97) / 4);
const qty = Float64Array.from({ length: 100_000 }, (_, i) => 1 + (i % 5));
console.log("revenue:", f64Dot(prices, qty).toFixed(2), "mean price:", f64Mean(prices).toFixed(3));
console.log("best two:", Array.from(topkIndices(Float32Array.of(0.1, 0.9, 0.4, 0.7, 0.2), 2)).sort());
console.log("cosine distance:", distCos(Float32Array.of(1, 0, 1), Float32Array.of(1, 1, 0)).toFixed(3));

// Containers whose bookkeeping stays native.
const latency = new TDigest().addMany(Float64Array.from({ length: 50_000 }, (_, i) => (i * 7919) % 1000));
console.log("p50/p99:", Array.from(latency.quantiles([0.5, 0.99]), Math.round));
const hot = new TopK(3);
for (const path of ["/a", "/b", "/a", "/c", "/a", "/b"]) hot.add(path);
console.log("hits on /a:", hot.count("/a"));
const active = new RoaringBitmap().addRange(0, 1_000_000);
console.log("active and paid:", active.and(new RoaringBitmap([5, 10, 2_000_000])).size);

// Prototype extensions instead of helper loops.
const orders = [{ region: "eu", total: 30 }, { region: "us", total: 50 }, { region: "eu", total: 20 }];
const byRegion = Object.entries(orders.groupBy((o) => o.region)).map(([r, os]) => [r, os.pluck("total").sum()]);
console.log("by region:", JSON.stringify(Object.fromEntries(byRegion)));
console.log([3, 1, 3, 2].unique(), [4, 8, 6].median(), "hello world".capitalize(), (17).clamp(0, 10));
```

The [example programs](#example-programs) are written this way throughout: each one moves its hot loops into these three layers and checks the results against an independent oracle.

---

## Recipes

Each recipe is one complete file that uses several modules together. All but the one that needs the public internet are executed against the current binary whenever the documentation is checked.

### 1. A JSON-RPC service with auth, rate limiting and metrics

A routed service, a signed bearer token, a token-bucket rate limiter and Prometheus metrics. Client and server share the script, so it runs as is.

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

### 2. Access-log analysis with DataFrames

Parse log lines into typed columns, filter with a mask, and group. The one-off analysis that usually lives in a shell pipeline, with real types.

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

### 3. ETL: CSV in, clusters out, compressed archive on disk

Read a sales CSV, cluster customers with k-means, write the scored result as JSON with a digest, and leave a gzip archive on disk.

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
await model.fit(features);                        // runs on its own thread
const segments = await model.predict(features);
const scored = table.rows.map((r, i) => ({
    cust: r[0], revenue: Number(r[2]), segment: segments[i],
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

A key derived from a passphrase with Argon2id, authenticated encryption bound to the document's identity, and a check that swapping the identity makes decryption fail.

```js
import { AESGCM, Argon2id, RandomBytes } from "dyna:crypto";
import { SHA256Hex } from "dyna:hash";
import { writeFile, makeTempDir, Path, File, removeAll } from "dyna:file";

// Derive the vault key from the passphrase (memory-hard by construction)
// { encoded: false }: raw key bytes (the default PHC string is for storage)
const key = Argon2id.hash("correct horse battery staple", RandomBytes(16),
                           { memory: 65536, iterations: 3, parallelism: 1,
                             hashLen: 32, encoded: false });
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

A crawler skeleton: robots policy, per-host pacing and jittered retries from `dyna:scrape`, CSS-selector extraction from `dyna:html`, URL resolution from `dyna:url`. It needs the network, so the documentation check skips it.

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

### 6. A SQLite-backed inventory tool

The embedded database with bound parameters and styled terminal output: the shape of an internal ops tool, in one file.

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

### 7. A static site with a health endpoint

A static route table served from its own reactor thread, which is why the blocking client in the same script can call it; requests are logged as structured JSON.

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

### 8. Stream a compressed log through bounded memory

Fifteen thousand log lines are compressed as they are written, read back through a decompressing source into one reused 64 KiB buffer, and the 5xx lines stream into a second compressed file. The log is never resident in memory.

```js
import { deflate, inflate, fromFile, toFile } from "dyna:stream";
import { Path, makeDir, removeAll } from "dyna:file";

const dir = new Path(Path.temp(), "logflow");
makeDir(dir, { recursive: true });

// Write side: the compressing sink accepts raw bytes; the file on disk
// never holds them uncompressed. finish() finalizes the stream, flushes
// and closes the file.
const logPath = new Path(dir, "access.log.zst");
const zst = deflate(toFile(logPath), { codec: "zstd", level: 5 });
const enc = new TextEncoder();
let events = 0;
for (let day = 1; day <= 30; day++)
    for (let i = 0; i < 500; i++) {
        const line = `2026-08-${String(day).padStart(2, "0")}T10:00:` +
                     `${String(i % 60).padStart(2, "0")}Z GET /api/orders ` +
                     `${i % 25 === 0 ? 500 : 200} ${i}ms\n`;
        await zst.write(enc.encode(line));
        events++;
    }
const rawBytes = await zst.finish();

// Read side: inflate() fills ONE buffer we reuse; { stream: true } makes the
// TextDecoder carry a multi-byte character split across chunks; a partial
// line waits in `carry` for the next read. Matches stream out compressed.
const src = inflate(fromFile(logPath), { codec: "zstd" });
const report = deflate(toFile(new Path(dir, "errors.log.zst")), { codec: "zstd" });
const dec = new TextDecoder();
const buf = new Uint8Array(64 * 1024);
let carry = "", scanned = 0, serverErrors = 0, n;
while ((n = await src.read(buf)) > 0) {
    const chunk = carry + dec.decode(buf.subarray(0, n), { stream: true });
    const lines = chunk.split("\n");
    carry = lines.pop();                       // no trailing newline yet
    for (const line of lines) {
        scanned++;
        if (line.includes(" 500 ")) {
            serverErrors++;
            await report.write(enc.encode(line + "\n"));
        }
    }
}
src.close();
const errorBytes = await report.finish();

// Verify the sidecar by streaming it back through the same machinery
const back = inflate(fromFile(new Path(dir, "errors.log.zst")), { codec: "zstd" });
const rbuf = new Uint8Array(4096);
let echoed = 0, rn;
while ((rn = await back.read(rbuf)) > 0)
    for (let i = 0; i < rn; i++) if (rbuf[i] === 0x0a) echoed++;
back.close();

console.log("events:", events, "| raw bytes:", rawBytes,
            "| scanned:", scanned, "| 5xx:", serverErrors,
            "| sidecar lines:", echoed, "| sidecar bytes:", errorBytes);
console.log("counts agree:", events === scanned && serverErrors === echoed);
removeAll(dir);
```

### 9. Validate input with web globals and strict option bags

`URL`, `URLSearchParams` and `structuredClone` with no imports, one-call validators, and a typo in an option bag failing loudly.

```js
import { IsIPv4, IsPort } from "dyna:validate";
import { gzip } from "dyna:compress";

// URL, URLSearchParams and structuredClone are globals — no imports
const hook = new URL("https://mesh.example/hook?host=10.0.0.7&port=8443");
const q = hook.searchParams;
console.log("query:", q.get("host"), q.get("port"),
            "| keys:", [...q.keys()].join(","));

const problems = [];
if (!IsIPv4(q.get("host"))) problems.push("host is not IPv4");
if (!IsPort(q.get("port"))) problems.push("port is out of range");
console.log("webhook target:", problems.length === 0 ? "ok" : problems.join("; "));

// A deep, structured copy to mutate freely — the caller keeps the original
const base = { retries: 3, backoff: { baseMs: 200, maxMs: 8000 } };
const cfg = structuredClone(base);
cfg.backoff.baseMs = 500;
console.log("deep clone:", base.backoff.baseMs === 200 && cfg.backoff.baseMs === 500);

// Option bags are checked key by key: an unknown key throws, naming the
// valid set, so a typo fails loudly instead of being ignored
let strict = "";
try { gzip(new Uint8Array(16), { levle: 9 }); }        // <- typo
catch (e) { strict = e.message; }
console.log("typo refused:", strict);

await sleep(10);                                       // a promise-based pause
console.log("done after a 10ms pause");
```

---

## Example programs

[`examples/`](examples) holds 65 complete programs, each a real system in one file that combines six to ten DynaJS modules: a CDC replicator, an LSM storage engine, an order-matching engine, an identity provider, a Redis-compatible store, a build system. Each file opens with a header naming what it builds, the modules it uses, and how to run it. Run without arguments, a program builds its own data, exercises the system end to end, checks the results and exits non-zero on any failure.

```sh
dynajs examples/data/cdc-replication.js      # run one
./build.sh check-api                         # run them all
```

### Data engineering

| Program | What it builds |
|---|---|
| [CDC replication](examples/data/cdc-replication.js) | Trigger-based change capture shipped over TCP to an idempotent replica, with resume and repair. |
| [Clickstream pipeline](examples/data/clickstream-pipeline.js) | Compressed event streams to sessions, funnels, sketches, rollups and a queryable archive. |
| [Data quality platform](examples/data/data-quality-platform.js) | Profile a dataset, infer a contract, validate new batches, detect drift, publish a report. |
| [Experimentation platform](examples/data/experimentation-platform.js) | Deterministic assignment, event collection and a statistically honest readout. |
| [Master data deduplication](examples/data/master-data-dedup.js) | Entity resolution across CRM, billing and support into golden customer records. |
| [Product feed syndication](examples/data/product-feed-syndication.js) | Merge supplier feeds in three formats into one catalog and publish channel feeds. |
| [Retail warehouse](examples/data/retail-warehouse.js) | CSV extracts to a star schema, then revenue, margin, cohorts and basket metrics with exact money. |
| [Time-series database](examples/data/timeseries-db.js) | Line-protocol ingest, compressed chunks, a tag index, a query language, rollups and retention. |

### Finance

| Program | What it builds |
|---|---|
| [Accounting ledger](examples/finance/accounting-ledger.js) | An immutable double-entry general ledger with exact decimal arithmetic, multi-currency postings, FX revaluation, accruals with automatic reversal, period close and the three financial statements. |
| [Bank reconciliation](examples/finance/bank-reconciliation.js) | Import CAMT.053 XML, MT940 and CSV statements, match bank lines to open invoices and payables with a rule cascade (reference, amount+date, fuzzy name, many-to-one subset sums, fee tolerance), post the matches, and prove the book balance against the bank balance. |
| [Billing platform](examples/finance/billing-platform.js) | Subscriptions with seat, tiered and usage pricing, mid-cycle proration, metered usage with idempotent ingest, tax by jurisdiction, coupons, gap-free invoice numbering, dunning, credit notes, revenue recognition and SaaS metrics. |
| [Payroll system](examples/finance/payroll-system.js) | Gross-to-net calculation with progressive tax brackets, social contributions with ceilings, pre-tax deductions, overtime from timesheets, mid-month joiners, retro pay, payslips, a SEPA payment file, journal postings and year-to-date reconciliation. |
| [Portfolio analytics](examples/finance/portfolio-analytics.js) | Covariance shrinkage, mean-variance and risk-parity construction, a rebalancing backtest with costs, factor regression, PCA risk decomposition, VaR/CVaR, drawdowns, stress tests and attribution. |
| [Trading engine](examples/finance/trading-engine.js) | A price-time-priority matching engine behind a FIX 4.4 gateway, with pre-trade risk, self-trade prevention, a UDP market-data feed with gap detection, a replayable journal and end-of-day analytics. |

### Machine learning

| Program | What it builds |
|---|---|
| [AIOps](examples/ml/aiops-anomaly.js) | Anomaly detection over service telemetry, alert correlation into incidents, root-cause ranking, paging. |
| [Churn system](examples/ml/churn-system.js) | Feature store, training with validation, a versioned model registry, online scoring and monitoring. |
| [Credit decisioning](examples/ml/credit-decisioning.js) | A scorecard, a policy rule engine, risk-based pricing, adverse-action reasons and a fairness audit. |
| [Demand planning](examples/ml/demand-planning.js) | Hierarchical sales forecasts, backtested model selection, and the inventory policy they drive. |
| [Fraud detection](examples/ml/fraud-detection.js) | Real-time card authorisation scoring with streaming velocity features, a device/account link graph, a supervised model, rules, step-up decisions and a cost-based threshold. |
| [Property AVM](examples/ml/property-avm.js) | Scrape listing pages, clean and geocode them, value homes with an ensemble of hedonic regression, random forest and comparable sales, with honest prediction intervals and an HTML valuation report. |
| [Recommendation service](examples/ml/recommendation-service.js) | Implicit-feedback matrix factorisation, an IVF candidate index, hybrid ranking with diversity, business rules, offline evaluation and an HTTP serving layer. |
| [Ticket router](examples/ml/ticket-router.js) | Language detection, PII redaction, intent classification, duplicate merging, KB deflection and skill-based assignment for a multilingual support inbox. |

### Networking

| Program | What it builds |
|---|---|
| [CDN edge](examples/net/cdn-edge.js) | Caching reverse proxies in front of an origin: HTTP cache semantics (max-age, s-maxage, no-store, private, stale-while-revalidate, Vary), conditional revalidation, request coalescing, an origin shield tier, surrogate-key purges, byte ranges, compression negotiation, signed URLs and consistent-hash routing. |
| [Message broker](examples/net/message-broker.js) | A Kafka-style partitioned log over TCP: segment files with a sparse offset index, idempotent producers, consumer groups with rebalancing and committed offsets, retention, log compaction and crash recovery. |
| [Metrics pipeline](examples/net/metrics-pipeline.js) | A StatsD/DogStatsD UDP and Graphite TCP ingest tier, flush-interval aggregation with percentiles, cardinality protection with HyperLogLog, rollups, alert rules as expressions, and Prometheus / JSON export — the statsd + carbon + alertmanager shape. |
| [Multiplayer game server](examples/net/multiplayer-game.js) | Matchmaking by rating, an authoritative fixed-tick simulation over WebSockets, client prediction with server reconciliation, delta-compressed binary snapshots, interest management, lag-compensated hit detection, speed-hack detection and Elo updates. |
| [Redis-compatible store](examples/net/redis-compatible-store.js) | A RESP2 server with strings, counters, expiry, lists, hashes, sorted sets, pub/sub, MULTI/EXEC transactions, pipelining, an append-only file with rewrite, and LRU eviction under maxmemory, exercised through the engine's own Redis CLIENT so the protocol is checked by an independent implementation. |
| [Service discovery](examples/net/service-discovery.js) | A five-node registry cluster with SWIM failure detection over UDP, a gossiped CRDT service catalogue, active HTTP health checks, a DNS front end, and clients that balance with consistent hashing. |

### Operations

| Program | What it builds |
|---|---|
| [Backup system](examples/ops/backup-system.js) | Content-defined chunking, deduplication, compression and authenticated encryption into a repository, with snapshots, incremental runs, a grandfather-father-son retention policy, garbage collection, integrity scrubbing and verified restores (the restic / borg design). |
| [Deployment controller](examples/ops/deployment-controller.js) | A reconciling control loop with rolling updates, readiness probes, canary analysis, automatic rollback, revision history and config-driven rollouts. |
| [Job orchestrator](examples/ops/job-orchestrator.js) | DAG workflows with cron schedules, backfill, a worker-thread pool, per-pool concurrency, retries with exponential backoff and jitter, timeouts, result passing, crash recovery and SLA tracking. |
| [Observability stack](examples/ops/observability-stack.js) | Trace, log and metric ingestion with tail-based sampling, trace assembly, critical-path analysis, a service map, RED metrics, log/trace correlation and multi-window SLO burn-rate alerting. |
| [Performance lab](examples/ops/perf-lab.js) | A load-testing and benchmarking harness that measures honestly: open vs closed load models, coordinated-omission correction, a pipeline-depth sweep that separates server cost from round-trip latency, log-bucketed latency histograms, micro-benchmarks with a calibrated empty loop, and a statistically tested A/B. |
| [Status page and incident management](examples/ops/status-and-incidents.js) | Synthetic monitors, component health with flap damping, incidents with an on-call rotation and escalation policy, a public status page with an Atom feed, uptime SLAs and postmortems. |

### Science and engineering

| Program | What it builds |
|---|---|
| [Astronomy](examples/science/astronomy.js) | Positional astronomy from first principles (Julian dates, sidereal time, Kepler's equation, coordinate transforms, rise/set with refraction, the Moon's phase), an observing planner for a deep-sky catalogue, and an exoplanet transit search with a Box Least Squares periodogram. |
| [Capacity planning](examples/science/capacity-planning.js) | Traffic forecasting with Holt–Winters, queueing models (Erlang C, M/M/c waiting-time tails), a discrete-event simulator that validates them, autoscaling policies compared by cost and SLO, Little's law checks, failure headroom, and a staffing/server plan per hour of the week. |
| [Epidemic forecasting](examples/science/epidemic-forecasting.js) | An age-structured SEIR model integrated with RK4, fitted to noisy reported cases by Nelder–Mead, the time-varying reproduction number estimated with the Cori et al. Bayesian method, reporting-delay nowcasting, an ensemble forecast with prediction intervals, intervention scenarios, and an SVG report. |
| [Genomics](examples/science/genomics.js) | A small sequencing pipeline: FASTQ parsing with Phred qualities and read trimming, k-mer spectra and error correction, de Bruijn graph assembly, seed-and-extend read alignment with banded Smith–Waterman, pileup variant calling written as VCF, and gene finding (ORFs and translation) on the assembly. |
| [Logistics routing](examples/science/logistics-routing.js) | A delivery-planning engine: a road network with speed classes, shortest-path travel-time matrices, a capacitated vehicle routing problem with time windows solved by Clarke–Wright savings and improved by 2-opt, relocate and swap local search, schedule simulation with ETAs, a lower bound, GeoJSON output and a dispatch API that re-plans when an order is added. |
| [Physics](examples/science/physics.js) | A Lennard-Jones molecular dynamics engine (velocity Verlet, periodic boundaries, cell lists, a thermostat) with the measurements statistical mechanics predicts, and a heat-equation solver comparing explicit and Crank–Nicolson schemes against the analytic solution, plus symplectic vs non-symplectic orbit integration. |
| [Signal lab](examples/science/signal-lab.js) | Digital signal processing for machine-condition monitoring and telephony: a radix-2 FFT checked against the definition, windowed spectra, Kaiser-window FIR design, RBJ biquad IIR filters, decimation, Hilbert envelope analysis for bearing faults, overlap-save streaming, Goertzel DTMF decoding and WAV files. |

### Security

| Program | What it builds |
|---|---|
| [Access control service](examples/security/access-control.js) | Zanzibar-style relationship-based authorisation with schema rewrites, nested groups, folder inheritance, conditional grants, consistency tokens, explanations, a decision cache and policy tests. |
| [End-to-end encrypted messenger](examples/security/e2e-messenger.js) | X3DH key agreement, the Double Ratchet, sender-key groups, encrypted attachments |
| [Privacy pipeline](examples/security/privacy-pipeline.js) | PII discovery, keyed pseudonymisation, format-preserving tokens, k-anonymity, differential privacy, consent enforcement, data-subject requests and crypto-shredding. |
| [Secrets manager](examples/security/secrets-manager.js) | A Vault-style service: Shamir-sealed master key, envelope encryption, versioned secrets, path policies, short-lived tokens, dynamic leases, key rotation and a tamper-evident audit log. |
| [Security scanner](examples/security/security-scanner.js) | Secrets, code injection patterns, infrastructure misconfiguration and vulnerable dependencies in one pass over a repository, with stable fingerprints, a baseline, SARIF output and a CI gate. |
| [Supply-chain signing](examples/security/supply-chain-signing.js) | Reproducible release artifacts, an SBOM, signed provenance, a transparency log with Merkle proofs, TUF-style update metadata, and the verifier a deploy pipeline runs before installing anything. |
| [TLS service mesh](examples/security/tls-service-mesh.js) | A control plane that enrols workloads, mutual-TLS sidecars that prove identity, per-route authorisation, load balancing, retries, circuit breaking, certificate rotation and mesh-wide telemetry. |

### Systems

| Program | What it builds |
|---|---|
| [Build system](examples/systems/build-system.js) | A Bazel-flavoured incremental builder: targets declared in YAML, dependency scanning of imports, content-addressed action keys, a local action cache plus a shared remote cache over HTTP, parallel execution on worker threads in DAG order, early cutoff, cycle detection, and a critical-path report. |
| [Document database](examples/systems/document-database.js) | A MongoDB-shaped store: collections of JSON documents with schema validation, unique and secondary indexes, a filter language with a cost-based planner and explain(), an aggregation pipeline, atomic update operators, change streams, an append-only journal with compaction, and an HTTP API. |
| [Image pipeline](examples/systems/image-pipeline.js) | A PNG encoder and decoder written from the specification (zlib framing, CRC-checked chunks, all five scanline filters with adaptive selection), image operations (resize, Gaussian blur, unsharp mask, histogram equalisation), perceptual hashing for duplicate detection, a worker-thread thumbnail farm and an HTTP image service with on-the-fly variants and ETags. |
| [LSM storage engine](examples/systems/lsm-storage-engine.js) | A write-ahead log with group commit, a memtable, immutable SSTables with prefix-compressed blocks, per-table Bloom filters, leveled compaction, tombstones, MVCC snapshots, range scans through a k-way merge, crash recovery, and a YCSB-style benchmark (the LevelDB/RocksDB design). |
| [Version control](examples/systems/version-control.js) | A small Git: content-addressed blobs, trees and commits with Git's exact object format (so hashes match real Git), compressed loose objects, a staging index, branches and HEAD, status, log, unified diffs, merge-base computation, three-way merges with conflict markers, and fsck. |

### Text and documents

| Program | What it builds |
|---|---|
| [Email platform](examples/text/email-platform.js) | An inbound SMTP server with SPF, DKIM and DMARC evaluation, greylisting, a trained spam filter, conversation threading and a searchable mailbox; plus an outbound sender that DKIM-signs its mail. |
| [Publishing pipeline](examples/text/publishing-pipeline.js) | A technical book from Markdown chapters to a validated EPUB 3, a single-page HTML edition and plain text, with cross-references, footnotes, citations, a generated index, editorial checks and an edition-to-edition change report. |
| [Search platform](examples/text/search-platform.js) | A full-text search engine: analysis chain, positional inverted index in compressed segments, a boolean/phrase/field query language, BM25F ranking, typo tolerance, facets, highlighting, segment merging, persistence, an HTTP API and relevance evaluation against judged queries. |
| [Similarity detection](examples/text/similarity-detection.js) | Source-code plagiarism with MOSS-style winnowing, near-duplicate web pages with SimHash, topical similarity with TF-IDF vectors, passage alignment with Smith–Waterman, and clustering of the results. |

### Web services

| Program | What it builds |
|---|---|
| [API platform gateway](examples/web/api-platform-gateway.js) | Config-driven routing with breakers, retries, bulkheads, caching, auth, quotas and tracing. |
| [CMS publisher](examples/web/cms-publisher.js) | Markdown content to a fingerprinted static site with feeds, search, link checks, a crawl and a signed deploy. |
| [Commerce backend](examples/web/commerce-backend.js) | Catalog, search, accounts, carts, promotions, tax, idempotent checkout, invoices and reporting. |
| [Document management](examples/web/document-management.js) | Typed uploads, a deduplicated content store, versions, search, signed links, retention, export. |
| [Helpdesk](examples/web/helpdesk.js) | Email-to-ticket intake, ML triage, business-hours SLAs, round-robin assignment, and service reports. |
| [Identity provider](examples/web/identity-provider.js) | Accounts, 2FA, sessions, OAuth 2.0 with PKCE, token families and a tamper-evident audit trail. |
| [Realtime collaboration](examples/web/realtime-collab.js) | WebSocket rooms editing a shared JSON document with optimistic concurrency. |
| [Webhook platform](examples/web/webhook-platform.js) | Transactional outbox, signed deliveries, backoff, ordering, dead letters, replay, and verifying receivers. |

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
    --no-prototypes  do not install the Array/String/Number/Date/Function/
                     Iterator prototype extensions (env: DYNAJS_NO_PROTOTYPES=1)
-s                 strip all the debug info
    --strip-source strip the source code
    --io-threads N IO worker threads, 0..8 (default and ceiling min(ncpu,8);
                   0 runs every offload inline)
-q  --quit         just instantiate the interpreter and quit
```

Top-level `await` works in `-e` and in scripts, so `dynajs -e 'import("dyna:uuid").then(u => print(u.v7()))'` is a valid one-liner.

Module autodetection reads the first statement: a file that begins with `import` or `export` is a module, anything else is a script, and an `import` appearing later in a script is a SyntaxError. `-m` and `--script` force either mode.

### Running untrusted code

DynaJS has **no in-process sandbox**. Any script can import `dyna:sys` and `dyna:file`, which reach processes and the filesystem. Run untrusted code inside a container or jail, and add the budget flags:

```sh
dynajs --timeout-ms 5000 --memory-limit 268435456 --native-memory-limit 268435456 untrusted.js
```

- `--timeout-ms N` stops the script after N ms. JavaScript is interrupted with an uncatchable error and a non-zero exit; a native call that still has not returned after a short grace period ends the process with exit code 113.
- `--memory-limit N` caps the JavaScript heap, per runtime (each Worker has its own heap under the same cap).
- `--native-memory-limit N` caps memory the native modules allocate, process-wide. A script may lower this cap but never raise or clear it.

The `std` and `os` compatibility modules exist only under `--std`; without the flag `import "os"` fails.

### Prototype extensions

By default the engine adds about three hundred non-enumerable helpers to the standard prototypes: `Array.prototype.sum/first/unique`, `String.prototype.words`, `Number.prototype.clamp`, `Date.prototype.isLeapYear`, function combinators, lazy `Iterator` tiers, and `Object.*` statics. `for..in` and `JSON.stringify` never see them.

```sh
dynajs --no-prototypes script.js        # or: DYNAJS_NO_PROTOTYPES=1 dynajs script.js
```

Either form boots a strict-baseline engine: every standard ES builtin and every `dyna:*` module still works, and only the added helpers are absent.

---

## Shipping and embedding

Every program above is a JavaScript file. `dynajsc` compiles a file to bytecode and links the engine in statically, so the result is a native executable that needs nothing else on the target:

```sh
./build.sh build       # examples/hello.js -> ./examples/hello (native)
./examples/hello
```

Embedder notes: `JS_Eval` input must be NUL-terminated (`input[input_len] == 0`). A `dyn_aio_send` that fails after a partial write returns -1, and retrying the whole buffer sends a duplicate prefix. `dyn_iobuf_ensure_nul` refuses an mmap buffer with ENOTSUP, and `dyn_io_slurp` reads into the heap unless given `DYN_SLURP_MMAP`. MSVC is not a verified target (x64 MSVC dispatches scalar-only).

To host the engine inside a C program, link `libdynajs.a` and include `dynajs.h`. The bundled examples show the three patterns: a compiled script, compiled ES modules, and a native module written in C and loaded as a shared object.

---

## Building from source

```sh
# Full engine with the native standard library and TLS
./build.sh build CONFIG_NATIVE_MODULES=y CONFIG_TLS=y

# Minimal engine
./build.sh build
```

| Flag | Effect |
|---|---|
| `CONFIG_NATIVE_MODULES=y` | Compile the `dyna:*` standard library in |
| `CONFIG_TLS=y` | OpenSSL-backed TLS: HTTPS, RSA/ECDSA, X.509, AEAD ciphers |
| `CONFIG_SQLITE=y` | Force SQLite support (detected through pkg-config otherwise) |
| `CONFIG_ZSTD=y` | Force zstd support (detected otherwise) |
| `CONFIG_IO_URING=y` | Linux io_uring backend and the `dyna:uring` module; combines with `CONFIG_TLS=y` |
| `CONFIG_PORTABLE_CODECS=y` | macOS only: use the in-tree gzip/deflate stream codec that other platforms ship, instead of the system one |
| `CONFIG_USING=y` | The `using` / `await using` declaration syntax |
| `CONFIG_CLANG=y` | Use clang instead of gcc on Linux |
| `CONFIG_ASAN=y` / `CONFIG_UBSAN=y` / `CONFIG_TSAN=y` | Sanitizer builds |

Changing a flag rebuilds what it affects, and so does replacing a source file with an older copy; `./build.sh clean` starts from zero. `./build.sh linux` builds and tests the working tree on Linux in Docker (GCC with the full warning policy, on both the epoll and io_uring backends); the proof gate runs it as its last stage. The vector kernels (NEON, SSE4.2, AVX2, AVX-512) are selected at runtime from what the CPU reports; SVE is the exception and requires building with `-march=armv8-a+sve`. `./build.sh help` lists every command, including `./build.sh gate`, the full verification run to use before sending a change.

---

## Reliability

- **Language conformance** is checked against test262, and the URL parser against the WHATWG web-platform tests.
- **Every documented example runs.** The `js` blocks in this file and every program under `examples/` are executed against the binary, so the documentation cannot drift from the behavior.
- **The type declarations are checked against the binary's own exports**, in both directions: nothing exported is undeclared, nothing declared is missing.
- **Hostile input has its own tests**: fuzz targets for the parsers and decoders, adversarial suites for the network clients and servers, and memory-safety runs under AddressSanitizer and UndefinedBehaviorSanitizer.
- **Portability** is exercised on macOS and on Linux with both glibc and musl, on arm64 and x86-64.

---

## TypeScript and editor support

[`dynajs.d.ts`](dynajs.d.ts) declares every `dyna:*` module, the web globals and the prototype extensions, with a one-line description on each function. It is also the fastest way to read the whole library: one file, one line per idea. Reference it from `tsconfig.json` or `jsconfig.json`:

```json
{
    "compilerOptions": {
        "types": ["dynajs"]
    }
}
```

That gives completion, hover documentation and type checking across the standard library.

---

## License and links

- License: MIT, see [LICENSE](LICENSE). DynaJS is derived from QuickJS (© Fabrice Bellard and Charlie Gordon) and is heavily modified.
- Reference for every function: [dynajs.d.ts](dynajs.d.ts)
- Example programs: [examples/](examples)
- Issues: https://github.com/corporatepiyush/dynajs/issues
