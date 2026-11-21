# DynaJS

A lightweight, secure, fast and embeddable JavaScript runtime: more than 95% C17 at the core, less than 5% JavaScript as the API layer.

## DynaJS goals

- **Small C core, thin JS layer.** More than 95% of the runtime is C17. JavaScript is only the thin layer of API sugar on top.
- **Easy to fork, adopt and refactor.** Extensive test coverage lets you change the code with confidence, far more than you could with Node.js or Bun.
- **QuickJS, heavily reworked.** DynaJS starts from QuickJS. Its parser and interpreter have been extended toward the ES2026 edition and tuned for CPU and memory out of the box.
- **Richer built-ins.** `Array`, `String`, `Number`, `Object`, `Date`, `RegExp` and `Iterator` carry about three hundred extra non-enumerable utility methods on their prototypes. They run natively and much faster than Lodash or Ramda; `--no-prototypes` turns them off.
- **A fast regex engine.** It handles simple and complex patterns on tiny and very large inputs alike.
- **Hybrid concurrency.** Your JavaScript runs on a single thread, as in Node.js. Internally, modules hand network I/O, disk I/O and compute-heavy work to separate thread pools. Some long-running workloads use dedicated OS threads on demand.
- **Web standards.** Web Workers, WebRTC, Canvas and SVG, without HTML or CSS.
- **Spec first, extras opt-in.** Every API follows its web or RFC specification by default. A dyna-specific meta config, marked as an extension, may only switch off features that cost performance or raise safety limits.
- **test262 conformant.** 84,965 of 84,966 runnable [test262](https://github.com/tc39/test262) tests pass, in both strict and sloppy mode. The one failure is an Annex B test that predates the current spec text. See [Conformance](#conformance).
- **Secure by default.** Untrusted input is bounded before it is believed. Secrets compare in constant time, option bags reject unknown keys, and the build enables every hardening flag the compiler accepts.
- **Predictable CPU and memory.** No JIT, so no warm-up and no deoptimization cliffs. Memory growth (bytes per entry, peak RSS) is measured as closely as speed.
- **Self-sufficient.** A wide developer API is built in; nothing comes from a package manager. Point yourself or your coding agent at [`dynajs.d.ts`](dynajs.d.ts) to see the whole offering. The modules:
	- [`dyna:ai`](src/modules/ai/ai.d.ts): Anthropic and OpenAI API clients: messages, streaming, batches and tool use.
	- [`dyna:asn1`](src/modules/asn1/asn1.d.ts): ASN.1 DER: node builders, canonical encoding and a bounded decoder for untrusted input.
	- [`dyna:async`](src/modules/async/async.d.ts): `pmap`, `retry` with backoff, `withTimeout`, `Semaphore`, `Channel`, `Pool`, `debounce`/`throttle`, and `offload` to run work off the event loop.
	- [`dyna:bench`](src/modules/bench/bench.d.ts): an adaptive timing harness for your own code, with percentiles.
	- [`dyna:bytes`](src/modules/bytes/bytes.d.ts): byte buffers, UTF-8 and legacy charset conversion, and UTF-8-safe `Text`.
	- [`dyna:cli`](src/modules/cli/cli.d.ts): argument parsing, ANSI styling, prompts and progress bars.
	- [`dyna:compress`](src/modules/compress/compress.d.ts): zstd, brotli, snappy, gzip, LZ4, tar and zip, with deterministic decompression-bomb rejection.
	- [`dyna:config`](src/modules/config/config.d.ts): TOML 1.0, INI, `.env` and front matter.
	- [`dyna:crypto`](src/modules/crypto/crypto.d.ts): AES-GCM and ChaCha20-Poly1305, RSA/ECDSA/Ed25519 signatures, key exchange, Argon2id/scrypt/bcrypt, X.509 and JWT, all constant-time.
	- [`dyna:csv`](src/modules/csv/csv.d.ts): RFC 4180 CSV with streaming reads, type inference, a file-backed table, and `parse`/`stringify`.
	- [`dyna:dataframe`](src/modules/dataframe/dataframe.d.ts): columnar frames over TypedArrays: filter, sort, group-by, join, window functions and quantiles.
	- [`dyna:decimal`](src/modules/decimal/decimal.d.ts): exact decimal128 and money arithmetic.
	- [`dyna:dns`](src/modules/dns/dns.d.ts): a UDP DNS client and server.
	- [`dyna:docdb`](src/modules/docdb/docdb.d.ts): an in-memory document store written in C: MongoDB-style filters and updates, indexes, aggregation and snapshots.
	- [`dyna:encoding`](src/modules/encoding/encoding.d.ts): charset detection, base64/base32/hex, varints, JSON5, JSONPath and QR codes.
	- [`dyna:file`](src/modules/file/file.d.ts): atomic writes, file locks, watchers, globs, temp files and platform directories.
	- [`dyna:graph`](src/modules/graph/graph.d.ts): graphs with typed edges: traversals, shortest paths, PageRank and graph analytics. Graphs persist to disk.
	- [`dyna:hash`](src/modules/crypto/hash.d.ts): SHA-1/2/3, BLAKE2/3, hardware-accelerated CRC32 and xxHash, one-shot or streaming.
	- [`dyna:html`](src/modules/html/html.d.ts): an HTML5 tokenizer, CSS-selector queries, a sanitizer, Markdown and templating.
	- [`dyna:http`](src/modules/http/http.d.ts): WHATWG `fetch`/`Request`/`Response`/`Headers`/`FormData`, HTTP servers, WebSockets and multipart.
	- [`dyna:json`](src/modules/json/json.d.ts): JSON Pointer (RFC 6901) and JSON Patch (RFC 6902).
	- [`dyna:log`](src/modules/log/log.d.ts): structured, leveled JSON-lines logging.
	- [`dyna:matcher`](src/modules/matcher/matcher.d.ts): Aho-Corasick multi-pattern search with spans, Levenshtein and other edit distances, tries, Bloom filters and diffs.
	- [`dyna:mathx`](src/modules/mathx/mathx.d.ts): statistics, combinatorics, number theory, special functions, NDArray and linear algebra.
	- [`dyna:metrics`](src/modules/metrics/metrics.d.ts): counters, gauges and histograms with a Prometheus scrape.
	- [`dyna:ml`](src/modules/ml/ml.d.ts): decision trees, forests, boosting, k-NN, k-means, DBSCAN, linear/logistic regression, SVMs, PCA and scalers; fits run off the event loop.
	- [`dyna:net`](src/modules/net/net.d.ts): TCP and UDP sockets, frame decoders and rate limiting.
	- [`dyna:netip`](src/modules/netip/netip.d.ts): IP address and CIDR prefix parsing, canonical text and classification.
	- [`dyna:oauth2`](src/modules/oauth2/oauth2.d.ts): PKCE S256, state CSRF protection, bearer auth, `WWW-Authenticate`, and scope and `redirect_uri` checks.
	- [`dyna:otp`](src/modules/otp/otp.d.ts): HOTP/TOTP (RFC 4226/6238), `otpauth://` key URIs, QR enrolment, drift windows, replay protection and throttling.
	- [`dyna:postgres`](src/modules/postgres/postgres.d.ts): a PostgreSQL client: extended protocol, SCRAM auth, TLS, pipelining and a prepared-statement cache.
	- [`dyna:protobuf`](src/modules/protobuf/protobuf.d.ts): a protobuf wire codec driven by a plain-object schema, with a decoder hardened for hostile input.
	- [`dyna:proxy`](src/modules/proxy/proxy.d.ts): an L4 TCP reverse proxy.
	- [`dyna:random`](src/modules/random/random.d.ts): OS entropy plus a seedable PRNG with full-state checkpoints and common distributions.
	- [`dyna:raylib`](src/modules/raylib/raylib.d.ts): raylib 6 windows, drawing and input (optional build).
	- [`dyna:redis`](src/modules/redis/redis.d.ts): a Redis client over RESP2/RESP3 with TLS, pub/sub and pipelined commands.
	- [`dyna:schema`](src/modules/schema/schema.d.ts): JSON Schema Draft 2020-12 validation; schemas compile once.
	- [`dyna:scrape`](src/modules/scrape/scrape.d.ts): polite crawling: robots.txt, per-host pacing, jittered retries, conditional GETs and response caps.
	- [`dyna:sdl`](src/modules/sdl/sdl.d.ts): SDL3 windows, 2D rendering, input events and audio (optional build).
	- [`dyna:semver`](src/modules/semver/semver.d.ts): Semver 2.0.0 parsing, ranges and comparison.
	- [`dyna:serialize`](src/modules/serialize/serialize.d.ts): MessagePack, CBOR and BSON, and `structuredClone`.
	- [`dyna:sftp`](src/modules/sftp/sftp.d.ts): an SFTP v3 client and server on `dyna:ssh`: pipelined streaming reads and writes, and a server rooted in one directory that no `..` or symlink escapes, with a JS allow hook and a read-only mode.
	- [`dyna:simd`](src/modules/simd/simd.d.ts): reductions, dot products, distances and matrix ops on the fastest kernel the CPU supports, chosen at startup.
	- [`dyna:smtp`](src/modules/smtp/smtp.d.ts): an SMTP submission client (STARTTLS/implicit TLS, SASL AUTH, PIPELINING, BDAT, DSN, a connection pool) and an SMTP server with per-command hooks, streaming DATA/BDAT, BINARYMIME and server-side SCRAM.
	- [`dyna:dkim`](src/modules/dkim/dkim.d.ts): DKIM signing and verification (RFC 6376, Ed25519 per RFC 8463), canonicalization and crypto on the shared pool, with a pluggable key lookup such as a dyna:dns resolver.
	- [`dyna:sqlite`](src/modules/sqlite/sqlite.d.ts): embedded SQLite in defensive mode: bound parameters, a prepared-statement cache and `execMany`.
	- [`dyna:ssh`](src/modules/ssh/ssh.d.ts): an SSH-2 client and server with only modern crypto: strict key exchange, post-quantum hybrid KEX (ML-KEM 768 and sntrup761 with X25519), ChaCha20-Poly1305 and AES-256-GCM, Ed25519 keys and OpenSSH certificates, key-only login, `known_hosts` with hashed entries and `@cert-authority`, exec, shell and pty sessions, a JS authorization hook, local and remote port forwarding with a JS policy hook, ProxyJump, and an ssh-agent: client authentication through any agent socket and an agent of its own that `ssh-add` and `ssh` use.
	- [`dyna:stream`](src/modules/stream/stream.d.ts): web-stream helpers: file and byte streams, stdio streams, `read(buf)`/`write(view)` adapters, and chunk-safe `lines()`/`ndjson()` iterators. Codecs are `CompressionStream`/`DecompressionStream`.
	- [`dyna:structures`](src/modules/structures/structures.d.ts): heaps, B-trees, BKD trees (numeric, IP and geo), sorted maps, roaring bitmaps, k-d trees, a BM25 index, sketches, tries and LRU caches. Every one persists to disk.
	- [`dyna:svg`](src/modules/svg/svg.d.ts): SVG to PNG or raw RGBA, rendered off the event loop: shapes, paths, gradients, patterns, clips, masks, filters and text.
	- [`dyna:sys`](src/modules/sys/sys.d.ts): subprocesses, environment, arguments, and CPU and memory facts.
	- [`dyna:time`](src/modules/time/time.d.ts): RFC 5545 recurrence rules, `Duration`, civil dates and times, and time zones.
	- [`dyna:ui`](src/modules/ui/ui.d.ts): an immediate-mode UI toolkit with layout, widgets and input (optional build).
	- [`dyna:uring`](src/modules/uring/uring.d.ts): batched Linux io_uring disk reads (optional build).
	- [`dyna:url`](src/modules/url/url.d.ts): WHATWG URL parsing, IDNA 2008/UTS #46, Punycode and form encoding.
	- [`dyna:uuid`](src/modules/uuid/uuid.d.ts): UUID v1–v8, NanoID and ULID.
	- [`dyna:validate`](src/modules/validate/validate.d.ts): boolean validators for email, URL, UUID, card numbers, IBAN, JWT, IP, ports, MIME types, timestamps and password strength.
	- [`dyna:vec`](src/modules/vec/vec.d.ts): vector similarity search over f32 embeddings: HNSW and exact indexes, l2/cosine/inner product, int8 and binary quantization, bitmap or sorted 64-bit id filters, and save/load with mmap.
	- [`dyna:webrtc`](src/modules/webrtc/webrtc.d.ts): W3C `RTCPeerConnection`, data channels, transceivers, tracks and streams over ICE, DTLS, SCTP and SRTP, with DTMF and media relay.
	- [`dyna:wireguard`](src/modules/wireguard/wireguard.d.ts): userspace WireGuard (Noise_IKpsk2, cookies, rekey and roaming) with an embedded IPv4/IPv6, UDP and TCP stack (SACK, RFC 6528 ISNs), so scripts dial, listen and send datagrams through a tunnel as web streams without root; or a kernel tun with root. X25519, AEAD batches and inner checksums run on the pool; `parseConfig` reads wg-quick files.
	- [`dyna:xml`](src/modules/xml/xml.d.ts): XML as streaming SAX events, a tree or plain objects, with size caps on by default.
	- [`dyna:yaml`](src/modules/yaml/yaml.d.ts): YAML 1.2; anchors and aliases are refused by name.
- **Modules on the roadmap.** These are queued, each as its own optional module:
	- `dyna:mesh`: a Tailscale-style peer mesh on WireGuard: coordination, NAT traversal over ICE/STUN/TURN, relay fallback, name lookup and ACLs.
	- `dyna:tls`: TLS as an optional build feature; without it, `fetch` and WebSockets speak plain http and ws.
	- `dyna:smtp`, `dyna:imap` and `dyna:pop3`: an SMTP client and server (STARTTLS, OAuth, DKIM signing, per-message hooks), an IMAP4rev2 client (IDLE, CONDSTORE/QRESYNC) and a POP3 client.
	- `dyna:ftp`: an FTP/FTPS client and server with zero-copy transfers, resumable uploads, virtual roots and a pluggable filesystem.
	- `dyna:s3`: an S3 client for AWS, R2, MinIO, Garage and Backblaze: SigV4, presigned URLs, parallel multipart uploads and default integrity checksums.
	- `dyna:webauthn`: a WebAuthn/passkey relying party: registration and authentication ceremonies, attestation formats and COSE keys.
	- `dyna:fido`: CTAP2.1 over USB HID for hardware security keys, also used by `dyna:ssh` for `sk-ssh-ed25519` keys.
	- `dyna:smpp`: an SMPP 3.4/5.0 client for SMS gateways, with delivery receipts and long messages.
	- `dyna:canvas`: the web-standard `OffscreenCanvas` and Canvas 2D API, so d3 and Chart.js run unchanged, plus a node-canvas compatible extension.
	- `dyna:image`: PNG, JPEG, GIF and BMP decode and encode, with resizing and transforms on a shared raster core.
	- `dyna:pdf`: a streaming PDF 1.7 writer and a text and structure extractor.
	- `dyna:audio`: audio device I/O over a lock-free ring buffer, WAV/FLAC/Opus/MP3 codecs and a polyphase resampler.
	- `dyna:fts`: multi-field full-text search on immutable mmap segments: compressed postings, Block-Max WAND, fuzzy and prefix terms, stemmers, and rank fusion with `dyna:vec`.
	- `dyna:sql`: a SQL AST compiler for PostgreSQL, SQLite and MySQL that keeps identifiers and parameters apart, plus a schema differ and UP/DOWN migrations.
	- `dyna:kv`: an embedded LSM key-value engine: WAL, SSTables, leveled compaction, MVCC snapshots and column families.
	- `dyna:cron`: cron and `@every` schedules in IANA time zones, plus a persistent, crash-safe job queue with backoff, concurrency limits and dead letters.
	- `dyna:ts`: time-series analysis: rolling statistics, EWM, ACF/PACF, STL, ADF, Kalman filters and DTW.
	- `dyna:nlp`: BPE, WordPiece and Unigram tokenizers (`tokenizer.json` and tiktoken) and SimHash.
	- `dyna:nn`: a tensor runtime with quantized kernels and safetensors, GGUF and ONNX loaders; inference first, training second.
	- `dyna:transformer`: standard and looped transformers on `dyna:nn`, with KV-cache modes and adaptive halting.

## Install

One command on macOS, Linux or FreeBSD:

<!-- check:norun: installs software on the reader's machine -->
```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynajs/master/install.sh | bash
```

The installer builds from source and puts one binary at `/usr/local/bin/dynajs`. When that location needs root and sudo is unavailable, it uses `$HOME/.local/bin` instead. It asks no questions, so it is safe to pipe, and running it again upgrades. The first install needs `git` and a C compiler (clang preferred). On macOS the Xcode Command Line Tools provide both.

### install.sh options

| Option | What it does | When it matters |
|---|---|---|
| `--prefix DIR` | Installs to `DIR/bin` (default `/usr/local`). | No root access, or several installs side by side. |
| `--jobs N` | Sets the number of parallel build jobs (default: CPU count). | Small machines and CI runners with memory limits. |
| `--with-deps` | Installs missing prerequisites through the system package manager. Builds pinned, sha256-checked SQLite and OpenSSL 3.5 when the packaged ones are too old. | Fresh machines and old distributions. Without it, a missing SQLite or OpenSSL stops the install. |
| `--with-sdl` | Builds `dyna:sdl` (SDL 3.4.20 or newer). | Native windows, input and audio. |
| `--with-raylib` | Builds `dyna:raylib` (raylib 6.2 or newer). | Simple games and visual tools. |
| `--with-ui` | Builds `dyna:ui` and fetches its pinned font and text inputs. | Desktop UIs; add `--with-sdl` or `--with-raylib` to draw into a window. |
| `--with-uring` | Builds the io_uring I/O backend and `dyna:uring` (Linux only). | High-throughput disk and network I/O on Linux. |
| `--dry-run` | Prints the plan and preflight report, then stops. | Seeing exactly what will be installed or is missing. |
| `--verbose`, `--quiet` | Streams the full build output, or shows only warnings and errors. | Debugging a failed build, or keeping CI logs quiet. |
| `--uninstall` | Removes the installed binary. | Cleanup. |

`DYNAJS_PREFIX`, `DYNAJS_JOBS` and `DYNAJS_VERBOSE` do the same through the environment, which is handy behind a pipe; a flag wins over its variable. The full build log stays at `~/.cache/dynajs-build/install.log`, whether the install succeeds or fails.

---

## Using modules

Library code lives in `dyna:*` ES modules: `import { name } from "dyna:<module>"`. The web platform is global: `fetch`, `Request`, `Response`, `Headers`, `FormData`, `WebSocket`, `AbortController`, `URL`, `URLSearchParams`, `TextEncoder`, `TextDecoder`, `Blob`, `structuredClone`, `atob`/`btoa`, `queueMicrotask`, `console`, the timers, and a promise-based `sleep`.

A few rules hold across every module:

- **Option bags are strict.** An unknown key is a `TypeError` that names the valid keys.
- **Refuse, never guess.** Malformed input is rejected, not repaired. Limits reject rather than truncate.
- **Untrusted input is bounded first.** Anything a peer or a file can feed has documented size, depth and count caps.
- **Native resources close.** Servers, clients, handles and models have `close()` and `Symbol.dispose`, so `using` releases them.
- **Hand collections to C.** There is no JIT. One call into a module or an iterator helper beats a JavaScript loop over the same data.

---

## The API reference: dynajs.d.ts

[`dynajs.d.ts`](dynajs.d.ts) is the contract. It declares every `dyna:*` module, every web global and every prototype extension, with a description on each function. It is the fastest way to learn the library, for people and coding agents alike: one file, one idea per line. Reference it from `tsconfig.json` or `jsconfig.json` (`"types": ["dynajs"]`) for completion, hover documentation and type checking.

Each module also has its own file next to its source, `src/modules/<name>/<name>.d.ts` (linked from each module under DynaJS goals); shared types are in [`src/types/prelude.d.ts`](src/types/prelude.d.ts) and the web globals and prototype extensions in [`src/types/globals.d.ts`](src/types/globals.d.ts). The per-module files are the source; `dynajs.d.ts` is built from them by `./dynajs --std tools/dts.js bundle`, and `tools/dts.js check` fails on any drift.

---

## Command line

Usage: `dynajs [options] [file [args]]`. `dynajs --help` prints the same list.

| Option | What it does | Why it matters |
|---|---|---|
| `-e`, `--eval EXPR` | Evaluates `EXPR`; top-level `await` works. | One-liners and shell pipelines. |
| `-i`, `--interactive` | Starts the REPL. | Exploring an API. |
| `-m`, `--module` | Forces module mode. Otherwise a file that starts with `import`/`export` is a module. | Ambiguous files. |
| `--std` | Exposes the QuickJS-compatible `std` and `os` modules. | Low-level file and process access, and tooling scripts. |
| `--timeout-ms N` | Stops the script after N ms. A native call still running after a grace period exits with 113. | Untrusted or runaway code. |
| `--native-memory-limit N` | Caps native module memory process-wide. A script may lower it, never raise it. | Containing C-side allocations. |
| `--io-threads N` | Sets the I/O worker threads, 0..256 (default and ceiling: the usable CPUs); from 8 threads up CPU jobs hold at most N - 1 of them, so one is always free for I/O; one spare thread runs CPU work while that cap is reached or all N hold blocking I/O; when all N hold blocking jobs, I/O is queued and none finishes for 1 ms, queued ones run on up to 64 extra threads (at most one started per 2 ms) that exit after 10 s idle; jobs past the queue capacity wait inside the pool, never refused. It also caps the fork-join threads inside one native call; 0 runs everything inline. | Tuning throughput against CPU share. |
| `--no-prototypes` | Skips the prototype extensions (env `DYNAJS_NO_PROTOTYPES=1`). | A strict ECMAScript baseline. |
| `--no-unhandled-rejection` | Ignores unhandled promise rejections. | Legacy code that leaves promises dangling. |
| `-h`, `--help` / `-v`, `--version` | Prints help or the version. | |

The full `dynajs --help` output:

```text
usage: dynajs [options] [file [args]]
-h  --help         list options
-v  --version      print the version and exit
-e  --eval EXPR    evaluate EXPR
-i  --interactive  go to interactive mode
-m  --module       load as ES6 module (default=autodetect)
    --std          make 'std' and 'os' available to the loaded script
    --no-unhandled-rejection  ignore unhandled promise rejections
    --no-prototypes  do not install the project-added Array/String/
                     Number/Date/Function/Iterator prototype extensions
                     (env: DYNAJS_NO_PROTOTYPES=1)
    --timeout-ms N    stop the script after N ms (a native call still
                      running after a grace period exits with 113)
    --native-memory-limit N  cap native module memory at N bytes,
                      process-wide; a script may lower it, never raise it
    --io-threads N    IO worker threads, 0..256 (default and ceiling
                      the usable cpus; 0 runs every offload inline)
                      also caps the fork-join threads a native
                      starts inside one call; 0 = serial
```

### Running untrusted code

DynaJS has **no in-process sandbox**: any script can import `dyna:sys` and `dyna:file`. Run untrusted code inside a container or jail, and add the budget flags:

<!-- check:norun: names the reader's untrusted.js -->
```sh
dynajs --timeout-ms 5000 --native-memory-limit 268435456 untrusted.js
```

- `--timeout-ms N` interrupts JavaScript with an uncatchable error and a non-zero exit. A native call that has not returned after a short grace period ends the process with exit code 113.
- `--native-memory-limit N` caps native module memory process-wide. A script may lower this cap, never raise or clear it.

The `std` and `os` modules exist only under `--std`.

---

## Examples

[`examples/`](examples) holds complete, self-checking programs across data engineering, finance, machine learning, networking, operations, science, security, systems, text and web services. Each file's header names what it builds, the modules it uses, and how to run it. `./dynajs --std tools/check-apps.js` runs them all.

---

## Shipping and embedding

To host the engine inside a C program, link `libdynajs.a` and include `dynajs.h`.

Embedder notes:
- `JS_Eval` input must be NUL-terminated (`input[input_len] == 0`).
- A native must leave an uncatchable exception (the interrupt) pending: test `JS_HasUncatchableException` before `JS_GetException`, and never rethrow, reject or drop it.
  Defect rule D50 flags a take without that test.
- A `dyn_aio_send` that fails after a partial write returns -1, and retrying the whole buffer sends a duplicate prefix.
- `dyn_iobuf_ensure_nul` refuses an mmap buffer with ENOTSUP, and `dyn_io_slurp` reads into the heap unless given `DYN_SLURP_MMAP`.
- MSVC is not a verified target: x64 MSVC dispatches scalar-only.

---

## Conformance

The engine runs the official [test262](https://github.com/tc39/test262) suite (snapshot `5c8206929`, 2026-03-20), using `tools/run-test262.c` and `tools/test262.conf`.

| | Tests |
|---|---|
| Run | 84,966 |
| Pass | 84,965 |
| Fail | 1 |
| Skipped (feature not implemented) | 5,278 |
| Excluded by the config | 3,356 |

- **The failure:** `annexB/language/function-code/block-decl-func-skip-arguments.js`. DynaJS follows the current FunctionDeclarationInstantiation step 32 here, and the test expects the older behaviour.
- **Not covered:** the `intl402` directory is excluded (ECMA-402 is out of scope). Skipped features: decorators, `ShadowRealm`, source-phase imports, `import defer`, `import` of bytes and text, `Atomics.waitAsync`, canonical time zones and tail calls.
- **Further suites:** each `dyna:*` module is also held to its own conformance suites: WPT-derived tests, RFC test vectors, and differential tests against reference implementations.

## Building from source

<!-- check:norun: builds the engine from source (minutes) -->
```sh
./build.sh           # ./dynajs, ./libdynajs.a, then the ASan+UBSan engine in .obj-san/
./build.sh release   # only the release engine
./build.sh clean
```

The build needs OpenSSL 3.5 and SQLite through pkg-config. It links zstd when present, and brotli on Linux (macOS uses its own compression library). Warnings are errors, and every hardening flag the compiler accepts is on.

| Variable | Effect |
|---|---|
| `J=N` | Parallel compile jobs (default: CPU count) |
| `CC=gcc` | Compiler (default clang on macOS, gcc elsewhere) |
| `WERROR=0` | Leave warnings as warnings |
| `IO_URING=1` | Linux io_uring backend and `dyna:uring` |
| `SDL3=1` / `RAYLIB=1` / `UI=1` | `dyna:sdl`, `dyna:raylib`, `dyna:ui` |
| `EXTRA_CFLAGS=...` / `EXTRA_LDFLAGS=...` | Extra compile and link flags |

The vector kernels (NEON, SSE4.2, AVX2, AVX-512) are picked at runtime from what the CPU reports. SVE needs `-march=armv8-a+sve`.

---

## License and links

- License: MIT, see [LICENSE](LICENSE). DynaJS is derived from QuickJS (© Fabrice Bellard and Charlie Gordon) and is heavily modified.
- API reference: [dynajs.d.ts](dynajs.d.ts)
- Examples: [examples/](examples)
- Issues: https://github.com/corporatepiyush/dynajs/issues
