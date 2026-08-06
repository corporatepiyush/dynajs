<h1 align="center">DynaJS</h1>

<p align="center">
  <b>A JavaScript runtime where the standard library is native code, compiled in.</b><br>
  Crypto, HTTP, ML, compression, dataframes, SIMD — one <code>import</code>, no install step.
</p>

<p align="center">
  <img alt="status: beta" src="https://img.shields.io/badge/status-beta-orange">
  <img alt="version: 2026-06-04" src="https://img.shields.io/badge/version-2026--06--04-blue">
  <img alt="language: C17" src="https://img.shields.io/badge/language-C17-blue">
  <img alt="test262: 99.93%" src="https://img.shields.io/badge/test262-99.93%25-brightgreen">
  <img alt="sanitizers: ASan, UBSan" src="https://img.shields.io/badge/sanitizers-ASan%20%C2%B7%20UBSan-brightgreen">
  <img alt="warnings: zero" src="https://img.shields.io/badge/warnings-zero-brightgreen">
  <img alt="platforms" src="https://img.shields.io/badge/platforms-macOS%20%7C%20Linux%20%7C%20FreeBSD-lightgrey">
  <img alt="SIMD" src="https://img.shields.io/badge/SIMD-NEON%20%7C%20SSE4.2%20%7C%20AVX2%20%7C%20AVX--512%20%7C%20SVE-orange">
  <img alt="license: MIT" src="https://img.shields.io/badge/license-MIT-green">
</p>

> [!IMPORTANT]
> **DynaJS is beta.** The language core is solid — test262 sits at 99.93%, every change ships
> through a zero-warning build under ASan and UBSan, and the engine is the part that has had the
> most mileage. What is *not* settled is the `dyna:` standard library: **module APIs may still
> change between releases**, some modules are considerably newer than others, and the surface is
> still growing. There is no deprecation policy yet, so pin a commit if you build on it.
>
> Concretely, before you depend on this: expect API churn in the newer native modules, treat the
> HTTP server as suitable for internal services rather than a hostile edge, and read
> `docs/dynajs-guide/API.md` as the
> contract — it is verified against the binary, and every example in it is executed by the test
> suite.

<p align="center">
  <sub>Every change ships through <code>./dev.sh gate</code>: a zero-warning build, the whole
  suite under <b>AddressSanitizer</b> and <b>UndefinedBehaviorSanitizer</b>, and test262 held at a
  pinned baseline. <b>ThreadSanitizer</b> and <b>MemorySanitizer</b> builds are wired in for the
  threaded and uninitialised-read passes.</sub>
</p>

```js
import { DataFrame } from "dyna:dataframe";
import { LinearRegression } from "dyna:ml";
import { gzip } from "dyna:compress";
import { SHA256Hex } from "dyna:hash";

// A million rows, built in JavaScript, aggregated in C.
const N = 1_000_000;
const region = new Int32Array(N);
const spend = new Float64Array(N);
for (let i = 0; i < N; i++) {
    region[i] = i % 6;
    spend[i] = (i * 2654435761 % 9973) / 7;
}

const df = new DataFrame({ region, spend });
const t0 = performance.now();
const total = df.GROUP_BY_SUM("region", "spend").values;
const p95 = df.QUANTILE("spend", 0.95);
console.log(`${N} rows: grouped + p95 in ${(performance.now() - t0).toFixed(1)} ms`);

const model = new LinearRegression();
model.fit([[1], [2], [3], [4]], [3, 5, 7, 9]);
console.log(`predict(10) = ${model.predict([[10]])[0].toFixed(1)}`);

const blob = gzip(new TextEncoder().encode(JSON.stringify([...total])));
console.log(`gzip -> ${blob.length} B, SHA256 ${SHA256Hex(blob).slice(0, 16)}`);
```

```
1000000 rows: grouped + p95 in 11.2 ms
predict(10) = 21.0
gzip -> 82 B, SHA256 781d573450bc4348
```

A dataframe engine, gradient descent, DEFLATE and SHA-256 — four native libraries, one process,
**0.15 s wall clock from cold start**. No `package.json`, no dependencies, no build step, nothing
to install.

---

## Contents

[Install](#install-or-upgrade) · [Why](#why) · [Examples](#examples) · [Standard library](#the-standard-library)
· [Measured](#measured) · [Documentation](#documentation) · [Build from source](#build-from-source)

## Install (or Upgrade)

```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynajs/master/install.sh | bash
```

That clones, builds with the whole native standard library, and installs one binary to
`/usr/local/bin/dynajs`. It asks nothing, reads nothing from stdin, and prints everything it is about
to do before it does it — including whether it will need `sudo`. Re-running it is how you upgrade.

```sh
dynajs app.js             # run a file
dynajs -e 'print(1+1)'    # run an expression
dynajs -i                 # REPL
```

<details>
<summary>Options, and installing without root</summary>

```sh
# No root anywhere: install under your home directory instead.
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynajs/master/install.sh \
  | bash -s -- --prefix "$HOME/.local"

# Or clone first and read it before running it, which is the better habit.
git clone https://github.com/corporatepiyush/dynajs && cd dynajs
./install.sh --help          # every flag, with what it does
./install.sh --dry-run       # the preflight report, and then stop
./install.sh --verbose       # a live build transcript
./install.sh --uninstall     # remove the binary again
```

The build log is kept at `~/.cache/dynajs-build/install.log` whether the install succeeds or fails,
and a failure prints its last 25 lines rather than telling you to go and find it.

**Requirements**: `git`, `make`, and `clang` or `gcc`, on macOS, Linux or FreeBSD. `--with-deps`
installs them for you with whatever package manager you have — and where there is none, it installs
**Homebrew** first and uses that, which is the usual case on a fresh Mac. It also installs
`pkg-config` and `sqlite`, without which the build quietly omits `dyna:net`'s `SQLite` class; the
preflight report says which way that went before anything is downloaded.
</details>

|  |  |
|---|---|
| **Startup** | microseconds |
| **Idle memory** | ~2.9 MB peak RSS |
| **Standard library** | 20 native modules, in the binary |
| **Dependencies** | none |

## Why

**The library is native, not JavaScript.** `dyna:ml` fits a gradient-boosted tree in C with SIMD
reductions. `dyna:compress` is a real DEFLATE. `dyna:matcher` searches at ~14 GiB/s. You call them
from JavaScript, but they do not run *as* JavaScript.

**One binary is the whole system.** Copy it to a machine and every capability below is present.
Nothing to resolve, nothing to audit, nothing that can change under you between deploys.

**Small enough to hold in your head.** Microsecond startup and ~3 MB idle mean you can use it where
a runtime is normally too heavy: a CLI that runs a thousand times, a sidecar, a hook, a short-lived
job in a container.

DynaJS does not implement a package-manager ecosystem, and is not trying to. The capabilities other
projects reach for packages to get are instead first-party native modules under `dyna:` — curated,
audited, SIMD-accelerated, designed together rather than assembled. **Fewer, better, built in.**

## Examples

Every snippet below runs as shown. Save it to a file and `dynajs the-file.js`.

### One file, no dependencies: an observability pipeline

Ingest 50k request records from disk, aggregate them columnwise, then
content-address, sign, compress and encode the result for transport. Seven
standard-library modules, **no package manifest, no build step, no native
addon to compile** — and nothing here is a shim over a shell command.

Elsewhere this is a dependency tree: a CSV reader, a dataframe, a hash, an
HMAC, a gzip binding and a base64 codec, each with its own version, its own
build and its own supply chain. Here they are in the binary.

```js
import { makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
import { DataFrame } from "dyna:dataframe";
import { SHA256Hex } from "dyna:hash";
import { HMACHex } from "dyna:crypto";
import { gzip, gunzip } from "dyna:compress";
import { Base64URLEncode } from "dyna:encoding";
import { formatRFC3339, now } from "dyna:time";

// 1. Ingest 50k request records through the filesystem as real bytes.
const dir = makeTempDir("obs-");
const routes = ["/api/orders", "/api/users", "/health", "/api/search"];
const lines = [];
for (let i = 0; i < 50000; i++) {
  const r = routes[i % 4];
  // i % 4 === 3 IS /api/search, so the spike condition must be reachable
  // from it: i % 500 === 0 never is, because 500 % 4 === 0.
  const slow = r === "/api/search" && i % 501 === 3;
  lines.push(`${r},${slow ? 1800 + (i % 400) : 5 + (i % 90)},${i % 991 === 0 ? 503 : 200}`);
}
const log = dir.join("requests.csv");
writeFile(log, lines.join("\n"));

// 2. Columnar analysis: no loop in JS, the aggregates run over typed columns.
const cells = readFile(log).split("\n").map((l) => l.split(","));
const df = new DataFrame({
  route: cells.map((c) => c[0]),
  ms: new Float64Array(cells.map((c) => +c[1])),
  status: new Float64Array(cells.map((c) => +c[2])),
});
const worst = df.GROUP_BY_MAX("route", "ms");
const mean = df.GROUP_BY_MEAN("route", "ms");
const p999 = df.QUANTILE("ms", 0.999);
const failures = df.COUNT("status", df.GE("status", 500));

// 3. A signed, content-addressed, compressed report.
const report = JSON.stringify({
  at: formatRFC3339(now()),
  requests: df.ROWS,
  p999_ms: Math.round(p999),
  failures,
  routes: worst.keys.map((k, i) => ({
    route: k, worst_ms: worst.values[i], mean_ms: +mean.values[i].toFixed(1),
  })),
  sample: lines.slice(0, 40),
});
const digest = SHA256Hex(report);
const wire = Base64URLEncode(gzip(report));

print(`${df.ROWS} requests | p99.9 ${Math.round(p999)}ms | ${failures} failures`);
worst.keys.forEach((k, i) => print(`  ${k.padEnd(13)} worst ${worst.values[i]}ms`));
print(`report ${report.length}B -> gzip+base64url ${wire.length}B`);
print(`sha256 ${digest.slice(0, 16)} | hmac ${HMACHex("sha256", "deploy-key", digest).slice(0, 16)}`);
print(`round trip intact: ${gunzip(gzip(report)).length === report.length}`);
removeAll(dir);
```

```
50000 requests | p99.9 94ms | 51 failures
  /api/orders   worst 93ms
  /api/users    worst 94ms
  /health       worst 93ms
  /api/search   worst 1899ms
report 1086B -> gzip+base64url 484B
sha256 a2b90b209f85cc3c | hmac 904f2611090c1222
round trip intact: true
```


### Untrusted input, handled properly

Sanitise attacker-supplied HTML, canonicalise the payload so key order cannot
change its digest, and give every record a time-ordered id. Six modules, and
the sanitiser is a real parser rather than a regular expression.

```js
import { Sanitizer } from "dyna:html";
import { v7 } from "dyna:uuid";
import { StableStringify } from "dyna:encoding";
import { SHA256Hex } from "dyna:hash";
import { Logger } from "dyna:log";
import { parseDuration } from "dyna:time";

// A signed, rate-limited comment endpoint. Untrusted HTML is sanitised, the
// payload is canonicalised before hashing so key order cannot change the
// digest, and every post gets a time-ordered id.
const log = new Logger({ level: "error" });
const san = new Sanitizer({ allow: { p: [], b: [], i: [], a: ["href"] },
                            protocols: { "a.href": ["https"] } });

const ttl = parseDuration("15m");
const posts = new Map();

function accept(body) {
  const id = v7();                                  // sortable by creation
  const clean = san.clean(body.html);            // drops script, js: urls
  const canonical = StableStringify({ id, html: clean, author: body.author });
  posts.set(id, { canonical, digest: SHA256Hex(canonical) });
  return id;
}

const id = accept({
  author: "kim",
  html: '<p>hi <b>there</b> <a href="javascript:alert(1)">x</a><script>steal()</script></p>',
});
const post = posts.get(id);

print(`id ${id.slice(0, 8)}… sorts by time, ttl ${ttl / 1e9}s`);
print(`sanitised: ${JSON.parse(post.canonical).html}`);
print(`digest ${post.digest.slice(0, 16)}`);
// Canonical form: the same object with keys in another order hashes the same.
print(`stable: ${SHA256Hex(StableStringify({ author: "kim", html: JSON.parse(post.canonical).html, id })) === post.digest}`);
```

```
id 019fc1df… sorts by time, ttl 900s
sanitised: <p>hi <b>there</b> <a>x</a></p>
digest 10a3b8a90e8d713e
stable: true
```

The `<script>` element and the `javascript:` URL are gone because the
sanitiser is allowlist-driven: anything the policy does not name is dropped,
so a vector nobody thought of still fails closed.

### Data structures that are actually data structures

A spell-correcting, deduplicating lookup path across six modules. The Bloom
filter is bits, the trie is nodes and the LRU is an intrusive list — none of
them is an `Object` or a `Map` standing in for the real thing.

```js
import { BloomFilter, Trie, LRU } from "dyna:structures";
import { Levenshtein, MultiMatcher } from "dyna:matcher";
import { Base58Encode } from "dyna:encoding";
import { Murmur3_128Hex } from "dyna:hash";
import { Random } from "dyna:random";
import { Duration } from "dyna:time";

// A spell-correcting, deduplicating lookup path. Every structure here is
// native: the Bloom filter is bits, the trie is nodes, the LRU is a list --
// none of them is an Object pretending to be a set.
const vocab = ["deploy", "deployment", "database", "datacenter", "debug", "delete"];

const seen = new BloomFilter(10000, 0.01);
const trie = new Trie();
const cache = new LRU(128);
for (const w of vocab) { trie.insert(w, w.length); seen.add(w); }

// 1. Prefix completion straight out of the trie.
print(`"depl" -> ${trie.keysWithPrefix("depl").join(", ")}`);

// 2. Membership without storing the strings.
print(`bloom may contain "database": ${seen.mayContain("database")}, "nonsense": ${seen.mayContain("nonsense")}`);

// 3. Nearest correction by edit distance, bounded so a typo cannot cost O(n*m).
const typo = "databse";
let best = null;
for (const w of vocab) {
  const d = Levenshtein(typo, w, { max: 3 });
  if (d <= 3 && (best === null || d < best.d)) best = { w, d };
}
print(`"${typo}" -> "${best.w}" (distance ${best.d})`);

// 4. Scan a document for every vocabulary term in one pass.
const mm = new MultiMatcher(vocab);
const doc = "the deployment hit the database during debug";
print(`found: ${mm.allIn(doc).map((h) => vocab[h.index]).join(", ")}`);

// 5. Cache the answer under a short, non-sequential key.
const rng = new Random(42);
const key = Base58Encode(Murmur3_128Hex(typo).slice(0, 16));
cache.set(key, best.w);
print(`cached ${key} -> ${cache.get(key)}, ttl ${new Duration({ minutes: 90 })}`);
print(`deterministic rng: ${rng.nextBounded(1000)}`);
```

```
"depl" -> deploy, deployment
bloom may contain "database": true, "nonsense": false
"databse" -> "database" (distance 1)
found: deploy, deployment, database, debug
cached 861qrMctA23m4pHwSm16DQ -> database, ttl PT1H30M
deterministic rng: 742
```

`MultiMatcher` finds every term in one pass rather than one scan per pattern,
and the `{ max: 3 }` on `Levenshtein` bounds the edit-distance band — without
it a long pair is quadratic and the runtime refuses it outright.

### Files and paths

```js
import { Path, readFile, writeFile, exists, makeTempDir, removeAll } from "dyna:file";

const dir = makeTempDir("demo-");
const note = dir.join("note.txt");

writeFile(note, "hello\nworld\n");
print(exists(note));              // true
print(readFile(note).length);     // 12

removeAll(dir);
```

`Path` is a handle, not a string: it carries the OS-specific representation, so `join`, globbing and
metadata all avoid re-parsing on every call.

### Hashing, HMAC and encoding

```js
import { SHA256Hex, CRC32, Hasher } from "dyna:hash";
import { HMACHex } from "dyna:crypto";
import { HexEncode, Base64Encode, Base64URLEncode } from "dyna:encoding";

print(SHA256Hex("abc"));
print(CRC32("abc"));
print(HMACHex("sha256", "secret-key", "message"));

// Hasher is a capability: build the state once, feed it many chunks.
const h = new Hasher("sha256");
h.update("hello ");
h.update("world");
print(h.digestHex());

print(Base64Encode("hello"));      // aGVsbG8=
print(Base64URLEncode("hello"));   // aGVsbG8 (never padded)
```

### Compression

```js
import { gzip, gunzip } from "dyna:compress";

const text = "the quick brown fox ".repeat(500);
const packed = gzip(text);

print(`${text.length} → ${packed.length} bytes`);
print(gunzip(packed).length === text.length);   // true
```

### Searching text at memory speed

```js
import { Matcher, MultiMatcher } from "dyna:matcher";

// One pattern, compiled once, then reused across many inputs.
const m = new Matcher("ss");
print(m.firstIn("mississippi"));        // 2

// Many patterns, all found in a single pass (Aho-Corasick).
const multi = new MultiMatcher(["error", "warn", "fatal"]);
print(multi.firstIn("log line: warn disk almost full"));
```

Compiling the pattern is the point: a `Matcher` pays the setup once and then scans with a SIMD
kernel, so it beats a fresh `indexOf` whenever the same needle is used more than a few times.

### Columnar data

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
  region:  ["east", "west", "east", "west"],
  revenue: new Float64Array([10, 20, 30, 40]),
});

print(df.SUM("revenue"));                            // 100
print(df.MEAN("revenue"));                           // 25
print(df.GROUP_BY_SUM("region", "revenue").keys);    // east,west
print(df.DESCRIBE("revenue").stddev);                // 12.909944487358056
```

Columns backed by TypedArrays stay in native memory, so masked and grouped operations run in C
rather than as a JavaScript loop. Methods are `UPPER_SNAKE_CASE` so a column operation never reads
like an ordinary property access.

### CSV

```js
import { CSVFile } from "dyna:csv";
import { Path, makeTempDir, removeAll } from "dyna:file";

const dir = makeTempDir("csv-");
const file = new CSVFile(dir.join("people.csv"));

file.create({
  headers: ["Name", "Age", "City"],
  rows: [["Alice", "30", "NYC"], ["Bob", "25", "LA"]],
});

const data = file.read();
print(data.headers);      // [ "Name", "Age", "City" ]
print(data.rows.length);  // 2

removeAll(dir);
```

### Collections the language does not have

```js
import { Heap, BitSet, Trie, LRU, BloomFilter, UnionFind } from "dyna:structures";

const heap = new Heap();                  // numeric min-heap, compared in C
// new Heap((a, b) => b - a) for a max-heap
for (const n of [5, 1, 4, 2]) heap.push(n);
print(heap.pop());                 // 1

const bits = new BitSet();
bits.set(3); bits.set(70);
print(bits.get(3), bits.count);    // true 2 — count is a getter

const cache = new LRU(2);
cache.set("a", 1); cache.set("b", 2); cache.set("c", 3);
print(cache.has("a"));             // false — evicted

const uf = new UnionFind(5);
uf.union(0, 1); uf.union(1, 2);
print(uf.connected(0, 2));         // true
```

### Graphs

```js
import { Graph } from "dyna:structures";

const g = new Graph({ directed: true, weighted: true });
const a = g.addNode(), b = g.addNode(), c = g.addNode();

g.addEdge(a, b, 1);
g.addEdge(b, c, 2);
g.addEdge(a, c, 10);

print(g.dijkstra(a)[c]);        // 3 — via b, not the direct edge

```

### Machine learning

```js
import { LinearRegression, KMeans } from "dyna:ml";

const model = new LinearRegression();
model.fit([[1], [2], [3], [4]], [3, 5, 7, 9]);
print(model.predict([[5]])[0]);    // ~11

const km = new KMeans(2);          // 2 clusters
km.fit([[1, 1], [1.2, 1.1], [8, 8], [8.2, 8.1]]);
print(km.predict([[8.1, 8.05]])[0]);
```

### Vector math

```js
import { f64Dot, f64Sum } from "dyna:simd";        // double precision
import { dot, normL2, distL2, softmax } from "dyna:simd";  // single precision

const a = new Float64Array(1e6).fill(2);
const b = new Float64Array(1e6).fill(3);
print(f64Dot(a, b));                           // 6000000
print(f64Sum(a));                              // 2000000

print(dot(new Float32Array([1, 2, 3]),
          new Float32Array([4, 5, 6])));       // 32
print(normL2(new Float32Array([3, 4])));       // 5
print(distL2(new Float32Array([0, 0]),
             new Float32Array([3, 4])));       // 5
print(softmax(new Float32Array([1, 2, 3])));
```

The `f64*` kernels take `Float64Array`; the unprefixed ones are `Float32Array`, where twice as many
lanes fit in a vector register.

The kernel is selected at run time from what the CPU actually has — NEON, SSE4.2, AVX2, AVX-512 or
SVE — with a scalar path everywhere else.

### An HTTP service

<!-- check:skip -->
```js
import { App } from "dyna:net";
import { Path } from "dyna:file";

const app = new App({ port: 3000 });

app.static("/", new Path("./public"));          // sendfile, no copy through JS
app.rpc("/api", {                               // JSON-RPC 2.0
  add: ([a, b]) => a + b,
  echo: (params) => params,
});

app.start();
```

One thread, one event loop — kqueue on macOS/BSD, epoll or io_uring on Linux.

### IP addresses and CIDR

```js
import * as netip from "dyna:net";

print(netip.isPrivate("10.0.0.5"));       // true
print(netip.isLoopback("::1"));           // true

// A Prefix compiles the CIDR once, then tests membership cheaply.
const prefix = new netip.Prefix("10.0.0.0/8");
print(prefix.contains("10.1.2.3"));       // true
print(prefix.contains("11.0.0.1"));       // false
```

### Databases and caches

```js
import { SQLite } from "dyna:net";

const db = new SQLite(":memory:");
db.exec("CREATE TABLE hit (path TEXT, ms REAL)");
db.exec("INSERT INTO hit VALUES (?, ?)", ["/api", 1.5]);

print(JSON.stringify(db.query("SELECT * FROM hit WHERE path = ?", ["/api"])));
// [{"path":"/api","ms":1.5}]

// Parameters are bound, never interpolated, so this matches nothing.
print(db.query("SELECT * FROM hit WHERE path = ?", ["'; DROP TABLE hit --"]).length);  // 0
db.close();
```

Redis and PostgreSQL work the same way, over the same reactor, one `Promise` per command:

<!-- check:skip -->
```js
import { Redis, PostgreSQL } from "dyna:net";

const cache = new Redis({ host: "127.0.0.1", port: 6379 });
await cache.pipeline([["SET", "k", "v"], ["INCR", "hits"]]);   // one round trip

const pg = new PostgreSQL({ user: "app", database: "shop", password: "…" });
const res = await pg.query("SELECT name FROM users WHERE id = $1", [7]);
print(res.rows[0].name);
```

PostgreSQL authenticates with SCRAM-SHA-256 and refuses cleartext and MD5. Neither client speaks
TLS: `tls: true` throws at construction rather than connecting in plaintext.

### UUIDs

```js
import { v4, v7, parse, validate, version } from "dyna:uuid";

const id = v4();
print(id, validate(id), version(id));     // ... true 4

print(v7());   // time-ordered: sorts by creation time
```

### Time and durations

```js
import { nowMillis, monotonicNano, formatRFC3339, parseDuration, durationString,
         Second, Millisecond } from "dyna:time";

print(formatRFC3339(Math.floor(nowMillis() / 1000)));   // seconds since epoch

const timeout = parseDuration("1h30m");   // nanoseconds
print(durationString(timeout));           // 1h30m0s
print(timeout / Second);                  // 5400

// monotonicNano() is a BigInt, so the clock cannot lose precision on a long run
const t0 = monotonicNano();
for (let i = 0; i < 1e5; i++) { /* work */ }
print(`${Number(monotonicNano() - t0) / Millisecond} ms`);
```

### Version ranges

```js
import * as semver from "dyna:semver";

print(semver.compare("1.2.3", "1.10.0") < 0);   // true — numeric, not lexical
print(semver.satisfies("1.2.3", "^1.0.0"));     // true
print(semver.satisfies("2.0.0", "^1.0.0"));     // false

const v = semver.parse("1.2.3-beta.1+build5");
print(v.major, v.prerelease);
```

### Bytes and binary formats

```js
import { bytesOf, writeUint32LE, readUint32LE, indexOf, fromUtf8, toUtf8 } from "dyna:bytes";

const buf = new Uint8Array(8);
writeUint32LE(buf, 0, 0xdeadbeef);
print(readUint32LE(buf, 0).toString(16));   // deadbeef

// bytesOf reinterprets any TypedArray as its raw bytes, without copying
print(bytesOf(new Float64Array([1, 2])).length);   // 16

const hay = fromUtf8("hello world");        // string -> bytes
print(indexOf(hay, fromUtf8("world")));     // 6
print(toUtf8(hay));                         // bytes -> string
```

### Without any import

Native methods are installed directly on `String`, `Array`, `Object`, `Number` and `Function`, so
common work needs no import at all:

```js
print("hello world".titleize());          // Hello World
print("user_name".camelize());            // UserName
print("user_name".camelize(false));       // userName — lower first letter
print("<b>hi</b>".stripTags());           // hi — SIMD-scanned
print("a long sentence here".truncate(10));
print(("\u001b[31mred\u001b[0m").stripAnsi());   // red — CSI/OSC grammar
print("\u4f60\u597d".displayWidth());     // 4 — terminal cells, not code units
print("hello world".wrapAnsi(5));         // hello\nworld — SGR-aware wrap

print([3, 1, 4, 1, 5].unique());          // [ 3, 1, 4, 5 ]
print([1, 2, 3, 4].splitEvery(2));        // [ [1,2], [3,4] ]
print([1, 2, 3, 4].sum());                // 10
print([1, 2, 3, 4].average());            // 2.5
print([{n:"a"},{n:"b"}].pluck("n"));      // [ "a", "b" ]
```

See [the guide](docs/dynajs-guide/04-standard-library.md) for the full set.

## The standard library

20 modules, each one `import` away. Build with `CONFIG_NATIVE_MODULES=y` (the installer does this).

### Text and bytes

| Module | What it gives you |
|---|---|
| `dyna:bytes` | `Bytes` and `Text` handles; buffer compare/search/copy/fill; read/write every int and float width in LE and BE; SIMD UTF-8/16 kernels; 28 legacy charsets |
| `dyna:encoding` | hex, base64, base64url, base32, base32hex, Ascii85, base58 (+check) and a generic baseX, LEB128 varints, JSON5, RFC 8785 canonical JSON, RFC 9535 JSONPath |
| `dyna:xml` | streaming SAX, a document tree and a serializer; no DTD is read, so XXE is unrepresentable |
| `dyna:html` | a lenient HTML parser, CSS selectors, an allow-list sanitizer, a markdown renderer, a Mustache template |
| `dyna:yaml` | the YAML 1.2 core schema; anchors, aliases and tags are refused by name |
| `dyna:decimal` | exact decimal arithmetic (decimal128) and an integral `Money` type |
| `dyna:serialize` | MessagePack, CBOR, canonical value hashing, and a cycle-preserving `structuredClone` |
| `dyna:matcher` | `Matcher` (one pattern, SIMD kernel), `MultiMatcher` (Aho-Corasick, all patterns in one pass), and approximate matching: `Levenshtein` (Myers bit-parallel), `DiceCoefficient`, `DiffLines`/`DiffWords`/`DiffChars` (Myers O(ND), minimal) |
| `dyna:config` | INI (dotted sections, `key[]` arrays), `.env` (dotenv grammar), front-matter splitting |
| `dyna:url` | RFC 3986 `URL` with relative resolution, plus `formEncode`/`formDecode` |
| `dyna:log` | `Logger` (leveled, structured, child bindings) and the `debug()` shape |
| `dyna:cli` | `Command` argv parser (bundling, negation, `--`, subcommands), `StyleText`, TTY queries |
| `dyna:validate` | `IsEmail`, `IsIBAN` (mod-97), `IsCreditCard` (Luhn) and character classes |
| `dyna:csv` | RFC 4180 read/write, row and column edits, mmap plus atomic writes |

### Data and math

| Module | What it gives you |
|---|---|
| `dyna:dataframe` | Columnar tables over TypedArrays; masked and grouped ops run 67–89× a JS loop |
| `dyna:ml` | 14 model families: linear and logistic regression, kNN, decision trees, random forests, gradient boosting, kernel SVM, naive Bayes, k-means, DBSCAN, Gaussian mixtures, PCA, scalers, metrics |
| `dyna:simd` | Multi-ISA vector math over f32/f64/i32: dot, norm, distance, GEMM, activations, scans |
| `dyna:mathx` | Special functions (gamma, erf, Bessel, elliptic) and exact integer math (gcd, lcm, factorial, isPrime) , a safe compiled `Expression` |
| `dyna:structures` | 25 data structures the language has no builtin for. Core: `Heap`, `BitSet`, `UnionFind`, `Deque`, `List`, `Fenwick`, `SegTree`, `RingBuffer`, `Trie`, `LRU` (with TTL, `onEvict` and stats), `SortedSet`, `SortedMap`, `BTree`. Collections: `Multiset`, `Multimap`, `BiMap`, `Table`, `RangeSet`, `RangeMap`, `IntervalTree`, `MinMaxHeap`. Sketches: `BloomFilter`, `CountMinSketch`, `HyperLogLog`. Graphs: `Graph` with BFS, DFS, Dijkstra, Bellman-Ford, Floyd-Warshall, topo-sort, components, MST, A\*. Every one has `.serialize()` / `.deserialize()` for binary persistence |

### Security and identity

| Module | What it gives you |
|---|---|
| `dyna:hash` | SHA-1/224/256/384/512, MD5, CRC-32/32C, xxHash32/64, and a streaming `Hasher` capability, SHA-3/Keccak-256/SHAKE, BLAKE3, BLAKE2b/2s, Murmur3-128 |
| `dyna:crypto` | HMAC (free function and a keyed `Hmac` capability), HKDF, PBKDF2, CSPRNG bytes, constant-time compare |
| `dyna:random` | CSPRNG plus a seedable PRNG for reproducible streams |
| `dyna:uuid` | UUID v3/v4/v5/v7, parse, validate, version and variant, plus the standard namespace constants; `NanoID` and `ULID` |

### System and network

| Module | What it gives you |
|---|---|
| `dyna:net` | HTTP client, and `App` — typed routes (JSON-RPC 2.0, static `sendfile`, upload, WebSocket) on a single-thread kqueue/epoll/io_uring reactor. `TCPServer` (TCP and AF_UNIX IPC) and `UDPSocket` on the same reactor. `DNSResolver` and `DNSServer` (RFC 1035, four independent anti-spoofing defences, per-source rate limiting). Clients for **Redis** (RESP2/RESP3, pipelining, pub/sub), **PostgreSQL** (protocol 3.0, SCRAM-SHA-256, simple and extended query) and **SQLite** (bound parameters only). Plus IPv4/IPv6 parsing, canonicalisation and classification (loopback, private, multicast, link-local) and a compiled `Prefix` for CIDR membership; HTTP message codecs (Content-Type, negotiation, Range, cookies, ETag), `RateLimiter` (a bounded token bucket), `Metrics` (Prometheus exposition) |
| `dyna:file` | `Path` handle, buffered reader and writer with per-OS fast paths, metadata, dirs, glob, links, temp |
| `dyna:uring` | High-queue-depth bulk file reads via Linux io_uring. Linux only, and opt-in: needs `CONFIG_IO_URING=y` and `liburing`. Absent everywhere else — use `dyna:file` |
| `dyna:sys` | Process and environment: env, args, cwd, platform, pid, hostname, home, memory usage, machine facts (CPU/memory/load/uptime/disk), subprocesses (argv only, no shell) |
| `dyna:time` | Nanosecond durations, monotonic clock, RFC 3339 |
| `dyna:semver` | SemVer 2.0.0 parsing and comparison, full npm range grammar |
| `dyna:compress` | gzip/gunzip (real DEFLATE), LZ4 block and frame, reusable `Compressor`, tar and zip archives |

## Measured

Numbers from the repo's own benchmarks — 36 suites under `tests/bench_*.js`. Reproduce one with
`./dev.sh bench tests/bench_regexp.js`; `make bench-core` covers the C cores.

| Operation | Throughput |
|---|---|
| `dyna:matcher` `firstIn()` | 14,642 MiB/s |
| Regex literal scan | 20,690 MB/s |
| `String.prototype.indexOf` (40 KB) | 1,887 ns |
| `readAsString` (4 MB file) | 6,126 MB/s |
| Parse real framework bundles | 22 MB/s |
| HTTP `App` | ~114k req/s, 3.0 MB peak RSS |

## Documentation

| | |
|---|---|
| [**The Guide**](docs/dynajs-guide/README.md) | Start here. Philosophy, install, the language, the library. |
| [**API Reference**](docs/dynajs-guide/API.md) | Every module, every function, every signature. |
| [Introduction](docs/dynajs-guide/01-introduction-and-philosophy.md) | What DynaJS is and why it is shaped this way |
| [Installation](docs/dynajs-guide/02-installation-and-first-steps.md) | Build options and first steps |
| [Language and runtime](docs/dynajs-guide/03-language-and-runtime.md) | ES2024, the event loop, workers |
| [Standard library](docs/dynajs-guide/04-standard-library.md) | The `dyna:` modules in depth |

## Build from source

```sh
git clone https://github.com/corporatepiyush/dynajs && cd dynajs
make CONFIG_NATIVE_MODULES=y -j8
./dynajs -e 'print("ok")'
```

Requires `git`, `make`, and a C compiler (clang preferred; C17). `./install.sh --with-deps` will
fetch them for you.

| Target | What it does |
|---|---|
| `make test` | The JavaScript test suite |
| `make test-native` | The `dyna:*` module tests (needs `CONFIG_NATIVE_MODULES=y`) |
| `./dev.sh gate` | Full gate: zero-warning build, ASan, UBSan, `make test`, test262 |

## Status

ES2024, **99.93% of test262** (58 failures out of 83,744). Built and tested on macOS (arm64) and
Linux (x86-64, via Docker) with clang and gcc.

Origin: a fork of Fabrice Bellard's [QuickJS](https://bellard.org/quickjs/), release `2026-06-04`.
The engine core keeps QuickJS's design — the standard library is new.

## License

MIT. QuickJS is MIT, © 2017-2026 Fabrice Bellard and Charlie Gordon.
