# DynaJS

> **Status: BETA.** The core engine is stable and the standard library surface is complete; pre-1.0 releases may change names, options and defaults. Production use is at your own judgement; pin a version (`install.sh --ref <tag>`).

DynaJS is an embeddable, high-performance JavaScript engine and runtime: a from-scratch JS engine (ES2023-class surface) with a built-in native standard library (`dyna:*`), multi-architecture SIMD acceleration, HTTP/TLS networking, data science tooling, and a single static, dependency-free binary for macOS, Linux and FreeBSD.

Everything ships in one binary. There is no node_modules, no runtime dependency, and no package registry — modules are compiled in or linked from the system's security-tracked libraries (OpenSSL, SQLite, zstd, brotli).

**Engineering highlights:**

- **A complete ECMAScript runtime, not a wrapper.** Parser, bytecode compiler, interpreter and GC are original C with no upstream engine dependency; the language surface targets ES2023 and conformance is enforced by a test262 gate (`./dev.sh gate`).
- **Single static binary, zero runtime dependencies.** Deploy by copying one executable on macOS, Linux or FreeBSD — no package registry, no lockfile, no runtime resolution step. The only linked libraries are the OS's security-tracked OpenSSL, SQLite, zstd and brotli, which keeps supply-chain surface minimal.
- **Ergonomic extensions beyond the ECMA standard.** `Array`, `String` and `Number` gain the data-wrangling surface that otherwise requires a utility dependency: grouping, keyed sorting, deduplication, sampling, transposition, string case conversion, template formatting, byte-level transforms and human-readable formatting. Implemented as compiled C — no interpreter step per element — with string scanning, counting, matching and RegExp fast paths dispatched through the same runtime-selected SIMD kernels as `dyna:simd`.
- **Web platform APIs without a web browser.** WHATWG `fetch`/`Request`/`Response`/`Headers`/`FormData`, WebSockets, an HTTP server with strict JSON-RPC 2.0 endpoints (batched, promise-aware) and an L4 TCP proxy — all driven by the native event reactor on kqueue (macOS/BSD) and epoll (Linux), whose backend fd folds into an outer event loop.
- **A full network stack beneath it.** TCP/UDP sockets, a DNS client and server, IP/CIDR arithmetic, Redis/PostgreSQL/SQLite clients, rate limiting and metrics — service-building blocks that otherwise span three dependencies.
- **Cryptography with a hardened backend.** AEAD ciphers (AES-GCM, ChaCha20-Poly1305), RSA/ECDSA/Ed25519/X25519, Argon2id/scrypt/bcrypt, HKDF, X.509 and JWT — OpenSSL-linked, constant-time wherever secrets are compared.
- **Columnar DataFrames.** Tabular data over TypedArrays — filter, sort, group-by, join, window functions and quantiles — with numeric columns aliasing the typed buffer (zero-copy) and string columns dictionary-encoded.
- **ML algorithms as a module.** `dyna:ml` ships classifiers, regressors, clustering, decomposition and scalers — decision trees, random forests, k-NN, K-means, DBSCAN, linear/logistic regression, SVMs, gradient boosting, PCA — fitted in C over contiguous doubles, with persistence built in: every fitted model round-trips through `serialize()`/`deserialize()` to bit-identical predictions. Train and serve in one process, no Python bridge.
- **A documented, observable SIMD portfolio.** scalar, NEON, SSE4.2, AVX2, AVX-512, SVE — the fastest kernel the host supports is selected at startup and reported via `simd.active()`, so a deployment can audit which code path it runs.
- **Serialization for real systems.** protobuf wire codec, ASN.1 DER, MessagePack, CBOR and BSON for machine interchange; TOML, INI, `.env`, YAML, JSON5, CSV and XML for configuration and text.
- **Compression and archives as primitives.** zstd, brotli, snappy, gzip, LZ4, tar and zip ship as library calls — with output caps that reject decompression bombs deterministically.
- **Embedding is a first-class mode.** The engine builds as `libdynajs.a`; `dynajsc` compiles JavaScript to bytecode embedded in a single native executable, with the engine statically linked in — the target needs no JS source and no separate runtime to install.
- **Adversarial input is part of the design.** Untrusted parsers carry size caps and refusal lists, fuzz targets run in the gate, and ASan/UBSan/TSan builds plus glibc/musl/amd64 container legs are part of the default verification flow.
- **Predictable performance.** No JIT warm-up: cold start is a REPL with history and completion or an `-e` one-liner with top-level `await`, the HTTP server is syscall-bound, and heavy loops run through the SIMD kernels — timings do not depend on optimization history.
- **Unicode and URL handling to spec.** WHATWG URL parsing, IDNA 2008/UTS #46 over Unicode 16.0.0 tables, Punycode and form encoding.
- **Structured JSON tooling.** JSON Schema 2020-12 validation, JSON Pointer and Patch, JSON5 and JSONPath.
- **Built-in data structures with serialization.** Graphs (Dijkstra, A*, MST), heaps, segment trees, tries, disjoint sets and LRU caches — native C, GC-managed — and every container persists as a compact type-tagged record via `serialize()`/`deserialize()`, so state moves across a process boundary as bytes.
- **Temporal and scheduling primitives.** RFC 5545 recurring events (`RRule`), civil calendar types (`PlainDate`, `PlainDateTime`), durations and time zones.

---

## Table of contents

- [Install](#install)
- [Quick start](#quick-start)
- [Command line](#command-line)
- [The standard library at a glance](#the-standard-library-at-a-glance)
- [Walkthrough examples](#walkthrough-examples)
- [Embedding DynaJS](#embedding-dynajs)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Performance](#performance)
- [Project layout](#project-layout)
- [AI agent guide](#ai-agent-guide)

---

## Install

One command on macOS, Linux or FreeBSD:

```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynajs/master/install.sh | bash
```

The installer clones the source into `~/.cache/dynajs-build`, builds from scratch with the full native standard library, and installs one static binary to `/usr/local/bin/dynajs` (falling back to `$HOME/.local/bin` when that needs root and sudo is unavailable). It asks no questions, so it is safe to pipe. Re-running it is how you upgrade: it pulls the latest source, rebuilds, and overwrites the old binary. The full build log is kept at `~/.cache/dynajs-build/install.log`.

Options:

```sh
install.sh --prefix "$HOME/.local"   # install to a different prefix
install.sh --ref v1.2.3              # pin a tag instead of master
install.sh --with-deps               # also install missing build tools (brew/apt/dnf/...)
install.sh --dry-run                 # print the plan and preflight report, install nothing
install.sh --uninstall               # remove the installed binary
install.sh --verbose                 # stream the build output
```

A first install needs `git`, `make` and a C compiler (clang preferred). On macOS the Xcode Command Line Tools provide all three. The preflight report says exactly what is missing before anything is downloaded.

---

## Quick start

```js
// hello.js
import { zstd, unzstd } from "dyna:compress";
const data = new Uint8Array(Array.from({ length: 1000 }, (_, i) => i % 7));
console.log("round trip ok:", unzstd(zstd(data)).every((v, i) => v === data[i]));
```

```sh
dynajs hello.js          # run a file
dynajs -e 'print(1 + 1)' # evaluate an expression
dynajs -i                # REPL with history, tab-completion and \h help
```

```js
// Standard WHATWG globals work out of the box
const res = new Response(JSON.stringify({ hello: "world" }), {
    status: 200,
    headers: { "Content-Type": "application/json" }
});
console.log("Response ok:", res.ok);
res.json().then(data => {
    console.log("JSON:", JSON.stringify(data));
});
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

## The standard library at a glance

Every module is documented function-by-function in the [API reference](docs/dynajs-guide/API.md).

| Module | What it provides |
|---|---|
| [`dyna:bytes`](docs/dynajs-guide/API.md#dynabytes) | Work with raw binary data: build, slice and search byte buffers, fixed-width reads/writes, hex/base64 views, and UTF-8-safe `Text` strings |
| [`dyna:cli`](docs/dynajs-guide/API.md#dynacli) | Build friendly command-line tools: argument parsing, ANSI-styled output, prompts, progress bars, spinners |
| [`dyna:compress`](docs/dynajs-guide/API.md#dynacompress) | Squeeze data and pack archives: zstd, brotli, snappy, gzip, LZ4, tar, zip — decompression caps refuse bombs instead of running out of memory |
| [`dyna:config`](docs/dynajs-guide/API.md#dynaconfig) | Read configuration files: TOML 1.0, INI, `.env`, front matter |
| [`dyna:crypto`](docs/dynajs-guide/API.md#dynacrypto) | Anything secret-dependent: AEAD encryption (AES-GCM, ChaCha20-Poly1305), signatures (RSA, ECDSA, Ed25519), key exchange, password hashing (Argon2id, scrypt, bcrypt), X.509 certificates, JWT |
| [`dyna:csv`](docs/dynajs-guide/API.md#dynacsv) | Read and write CSV (RFC 4180): streaming, type inference, and a file-backed table |
| [`dyna:dataframe`](docs/dynajs-guide/API.md#dynadataframe) | Columnar data in plain JS: filter, sort, group-by, join, window functions and quantiles over TypedArrays |
| [`dyna:decimal`](docs/dynajs-guide/API.md#dynadecimal) | Exact decimal and money arithmetic (IEEE decimal128, 34 significant digits) — no binary-float rounding surprises |
| [`dyna:encoding`](docs/dynajs-guide/API.md#dynaencoding) | Format everything: charset detection, base64/base32/hex and friends, varints, JSON5, JSONPath, QR codes |
| [`dyna:file`](docs/dynajs-guide/API.md#dynafile) | Filesystem work: atomic writes, file locks, watchers, globs, temp files, platform directories |
| [`dyna:hash`](docs/dynajs-guide/API.md#dynahash) | Digest data: SHA-1/2, SHA-3, BLAKE2/3, CRC32 (hardware-accelerated), xxHash, fingerprints — one-shot or streaming |
| [`dyna:html`](docs/dynajs-guide/API.md#dynahtml) | Web content, server-side: an HTML5 tokenizer, CSS-selector queries, a sanitizer, markdown rendering, templating — every context escapes through one audited escaper |
| [`dyna:http`](docs/dynajs-guide/API.md#dynahttp) | The web platform: WHATWG `fetch`/`Request`/`Response`/`Headers`/`FormData`, an HTTP server in a few lines, WebSockets, multipart and header codecs |
| [`dyna:json`](docs/dynajs-guide/API.md#dynajson) | Edit JSON documents: JSON Pointer (RFC 6901) lookups, JSON Patch (RFC 6902) with copy-on-write |
| [`dyna:log`](docs/dynajs-guide/API.md#dynalog) | Structured logging: one JSON object per line, leveled, grep- and pipe-friendly |
| [`dyna:matcher`](docs/dynajs-guide/API.md#dynamatcher) | Search text fast: Levenshtein edit distance, tries, Bloom filters, Aho-Corasick multi-pattern search, diffs |
| [`dyna:mathx`](docs/dynajs-guide/API.md#dynamathx) | The math toolbox: statistics, combinatorics, big integers, number theory, linear algebra, interpolation, special functions |
| [`dyna:ml`](docs/dynajs-guide/API.md#dynaml) | Train models in-process: trees, forests, k-NN, K-means, DBSCAN, linear/logistic regression, SVMs, gradient boosting, PCA, scalers — C over contiguous doubles |
| [`dyna:net`](docs/dynajs-guide/API.md#dynanet) | Everything under HTTP: IP/CIDR arithmetic, TCP/UDP sockets, a DNS client and server, Redis/PostgreSQL/SQLite clients, rate limiting, metrics, an L4 proxy (statically import this or `dyna:http`, not both) |
| [`dyna:random`](docs/dynajs-guide/API.md#dynarandom) | Randomness done right: OS-entropy floats, ints, bytes, shuffles, choices — plus a seedable PRNG for reproducible runs |
| [`dyna:schema`](docs/dynajs-guide/API.md#dynaschema) | Validate data against JSON Schema (Draft 2020-12); schemas compile once, validation is pure dispatch |
| [`dyna:scrape`](docs/dynajs-guide/API.md#dynascrape) | Crawl politely: robots.txt, per-host pacing, retries and backoff, response caps — the politeness layer that keeps you off blocklists |
| [`dyna:semver`](docs/dynajs-guide/API.md#dynasemver) | Version numbers done right: semver 2.0.0 parsing, ranges, comparison, incrementing |
| [`dyna:serialize`](docs/dynajs-guide/API.md#dynaserialize) | Binary interchange: protobuf wire codec, ASN.1 DER, MessagePack, CBOR, BSON — for talking to anything that isn't JSON |
| [`dyna:simd`](docs/dynajs-guide/API.md#dynasimd) | Explicit vector math: reductions, dot products, distances, matrix ops — the fastest kernel your CPU supports, picked at startup |
| [`dyna:structures`](docs/dynajs-guide/API.md#dynastructures) | The data structures JS never shipped: LRU caches, graphs (Dijkstra, A*, MST, BFS/DFS), heaps, segment trees, disjoint sets, tries |
| [`dyna:sys`](docs/dynajs-guide/API.md#dynasys) | Talk to the OS: subprocesses, environment, arguments, CPU/memory facts, terminal formatting |
| [`dyna:time`](docs/dynajs-guide/API.md#dynatime) | Dates that work: RFC 5545 recurring events (`RRule`), `Duration`, civil calendar types (`PlainDate`, `PlainDateTime`), time zones, formatting |
| [`dyna:uring`](docs/dynajs-guide/API.md#dynauring) | Linux-only blistering disk I/O: io_uring batches many block reads into one submit/complete cycle per batch (built with `CONFIG_IO_URING`) |
| [`dyna:url`](docs/dynajs-guide/API.md#dynaurl) | URLs and domains: WHATWG URL parsing, IDNA 2008/UTS #46, Punycode, form encoding |
| [`dyna:uuid`](docs/dynajs-guide/API.md#dynauuid) | Identifiers: UUID v1–v7 from the OS CSPRNG, plus NanoID and ULID |
| [`dyna:validate`](docs/dynajs-guide/API.md#dynavalidate) | One-line checks: is this an email, a URL, a UUID, a credit card, a semver, an IP? Each a boolean predicate |
| [`dyna:xml`](docs/dynajs-guide/API.md#dynaxml) | XML parsing — streaming SAX, a tree, or a plain object with XPath-lite access — with sane size caps on by default |
| [`dyna:yaml`](docs/dynajs-guide/API.md#dynayaml) | YAML 1.2 parsing; anchors and aliases are refused by name, never silently dropped |

Core engine extensions (also in the API reference): `Array`, `String`, `Number`, `Object`, `Date`, `RegExp` extras.

---

## Walkthrough examples

Each of these blocks is executed by the project's own documentation gate, so they are known to run against the current binary.

### HTTP and Fetch

<!-- check:skip -->
```js
import { fetch, Request, Response, Headers, AbortController, FormData } from "dyna:http";

// Global fetch is also available on globalThis
const response = await fetch("https://api.example.com/data", {
    method: "GET",
    headers: { "Accept": "application/json" },
    timeout: 5000
});
const data = await response.json();
```

A real server in a few lines:

<!-- check:skip -->
```js
import { HTTPServer } from "dyna:http";

const server = new HTTPServer({ port: 8765 });
server.on("request", (req, res) => {
    res.json({ route: req.url, method: req.method });
});
server.listen();
// curl http://localhost:8765/hello
```

### Cryptography & X.509

```js
import { RSA, ECDSA, X509, Bcrypt } from "dyna:crypto";

// Password hashing
const hash = Bcrypt.hash("my_secure_password", 10);
const valid = Bcrypt.verify("my_secure_password", hash);
console.log("Bcrypt valid:", valid);

// Key generation and self-signed X.509 certificate
const keypair = RSA.generate(2048);
const certPem = X509.generateSelfSigned({
    key: keypair.privateKey,
    subject: "localhost",
    days: 365
});
const parsed = X509.parse(certPem);
console.log("Cert subject:", parsed.subject);
console.log("Cert serial:", parsed.serialNumber);
```

### JSON Schema & JSON Patch

```js
import { Schema } from "dyna:schema";
import { Pointer, Patch } from "dyna:json";

// JSON Schema validation (2020-12)
const schema = {
    type: "object",
    properties: {
        id: { type: "integer", minimum: 1 },
        name: { type: "string" }
    },
    required: ["id", "name"]
};
const result = Schema.validate(schema, { id: 10, name: "Alice" });
console.log("Schema valid:", result.valid);

// JSON Pointer (RFC 6901) & Patch (RFC 6902)
const doc = { users: [{ name: "Alice" }, { name: "Bob" }] };
console.log("Pointer read:", Pointer.get(doc, "/users/1/name"));

const patched = Patch.apply(doc, [
    { op: "replace", path: "/users/1/name", value: "Charlie" }
]);
console.log("Patched:", JSON.stringify(patched));
```

### TOML Configuration

```js
import { TOML } from "dyna:config";

const tomlText = `
title = "DynaJS Config"
[owner]
name = "Admin"
ports = [ 8000, 8001, 8002 ]
`;

const config = TOML.parse(tomlText);
console.log("Config title:", config.title);
console.log("Owner name:", config.owner.name);

const formatted = TOML.stringify(config);
console.log("TOML output length:", formatted.length);
```

### URL, IDNA & Punycode

```js
import { URL, domainToASCII, domainToUnicode, punycodeEncode } from "dyna:url";

const url = new URL("https://bücher.example.de/path?q=1#hash");
console.log("Host:", url.host);
console.log("Punycode ASCII:", domainToASCII("bücher.de"));
console.log("Unicode Domain:", domainToUnicode("xn--bcher-kva.de"));
```

### Recurrence Rules (RFC 5545)

```js
import { RRule } from "dyna:time";

// Weekly meeting every Tuesday for 4 occurrences
const rule = new RRule({
    freq: "WEEKLY",
    byday: ["TU"],
    count: 4,
    dtstart: new Date(2026, 7, 18, 10, 0, 0)
});
const dates = rule.all();
console.log("Recurrence count:", dates.length);
```

### DataFrames

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    id: new Int32Array([1, 2, 3, 4]),
    score: new Float64Array([95.5, 82.0, 88.5, 77.0])
});
console.log("mean:", df.MEAN("score"));
console.log("top scores:", Array.from(df.N_LARGEST("score", 2)));
```

### Machine learning

```js
import { DecisionTreeClassifier } from "dyna:ml";

const X = [[1, 0], [2, 0], [3, 0], [4, 0], [5, 0], [6, 0]];
const y = [1, 1, 1, 2, 2, 2];
const tree = new DecisionTreeClassifier({ maxDepth: 3 });
tree.fit(X, y);
console.log("predictions:", tree.predict([[1.5, 0], [5.5, 0]]));
```

### Graph algorithms

```js
import { Graph } from "dyna:structures";

const g = new Graph({ directed: true, weighted: true });
const a = g.addNode(), b = g.addNode(), c = g.addNode();
g.addEdge(a, b, 3);
g.addEdge(b, c, 4);
g.addEdge(a, c, 10);
const dist = g.dijkstra(a);
console.log("shortest a->c:", dist[c]);
```

---

## Embedding DynaJS

DynaJS is a library first. The engine compiles to `libdynajs.a`; `dynajsc` compiles JavaScript to bytecode embedded in a C file, and the engine is statically linked in, so a JavaScript program can ship as a single self-contained native executable with no JS source and no runtime to install on the target:

```sh
# examples/hello.js becomes examples/hello — a native binary with no JS source
make examples/hello
./examples/hello
```

See `examples/` for three embedding patterns: static JS compilation (`hello.js`), ES modules (`hello_module.js`), and an external native module (`fib.c` → `fib.so`). The public headers are `src/dynajs.h` and `src/dyna-libc.h`.

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

A flag change is not tracked by make; run `make clean` after changing flags, or a stale object will silently drop conditionally-compiled code.

---

## Testing

The gates, cheapest to most expensive:

```sh
make test          # core engine suite (39 suites, parallel)
make test-native   # the 159 native-module suites + examples + docs + installer tests
make test-api      # API surface, params, differential, roundtrip, vectors, kernels, properties, fuzz
make test-security # adversarial/pen tests
make prepush       # THE pre-push gate: codegraph, clean build, fuzz link proof,
                   # test, test-native, test-api, test-security, test-repl, TLS
./dev.sh gate      # zero-warning build + ASan + UBSan + make test + test262 baseline
bash docker/build-and-test.sh   # glibc + musl + emulated amd64, in parallel
```

`make prepush` is installed as the `pre-push` git hook (`make install-hooks`); it is the one thing to run before anything leaves the machine. `./dev.sh` is the developer entry point — `./dev.sh` alone lists every subcommand (build, quick, asan, ubsan, t262, bench, rss, amd64, gate, ...).

The docker gates reuse cached toolchain images (`dynajs:deps`, `dynajs:deps-musl`, `dynajs:deps-amd64`) and run the three libc/ISA legs concurrently with persistent object caches, so a warm run recompiles only what changed.

---

## Performance

- SIMD kernels (`dyna:simd`) are a runtime-dispatched portfolio: scalar, NEON, SSE4.2, AVX2, AVX-512, SVE — the fastest kernel the CPU can actually run is chosen at startup and is observable (`simd.active()`).
- Numeric work is columnar: DataFrames store typed columns, and ML kernels reduce over contiguous typed arrays.
- The HTTP server is syscall-bound; per-request work is bounded and peer-demanded work is capped.
- `bench/` holds the benchmark suites (parser, regexp, stdio, numeric, the C cores); each covers the area it is named for. Run the one that covers what you touched.

---

## Project layout

```text
src/            engine + stdlib sources (dyna-*.c per module; src/core/* pure C)
tests/          JS test suites (test_*.js) + shell harnesses (test_*.sh)
examples/       runnable JS examples, embedding demos, fib native module
tools/          code generators, doc/example checkers, api-inventory
bench/          benchmark suites and the code-graph analyzer (bench/codegraph.py)
docker/         container gates: Dockerfile (cached toolchains), build-and-test.sh, linux.sh
docs/           the API reference (docs/dynajs-guide/API.md) and reports
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
bash docker/build-and-test.sh                    # glibc/musl/amd64 legs, parallel, cached images
```

### The order that keeps you fast

1. Make ALL edits first, without compiling.
2. Static checks that need no binary: shellcheck for scripts, `python3 bench/codegraph.py` for source shape (padding, complexity, hand-written link lines).
3. Build once, then run the whole gate — never edit source while a gate runs.

### Verification is load-bearing

- The documentation gates execute every ```` ```js ```` block in `README.md` and `docs/dynajs-guide/API.md` against the current binary (`make check-readme`, `make check-api`). A documented call must exist and run; a block that cannot run standalone must carry `<!-- check:skip -->` on the line before its fence. Skipped blocks are counted and printed.
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

Ambient declarations for every `dyna:*` module, the WHATWG globals, and the core prototype extensions ship in [`types/dynajs.d.ts`](types/dynajs.d.ts). Reference it from your `tsconfig.json` / `jsconfig.json`:

```json
{
    "compilerOptions": {
        "types": ["../types/dynajs"]
    }
}
```

That gives autocomplete, hover documentation and typechecking over the full standard library — the declarations are generated against the binary's own export inventory and the gate (`make check-types`) fails when they drift.

---

## License and docs

- Source of truth for every function: [API reference](docs/dynajs-guide/API.md)
- Issues: https://github.com/corporatepiyush/dynajs/issues
