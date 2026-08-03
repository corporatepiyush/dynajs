<p align="right"><i><a href="03-language-and-runtime.md">← 3 · Language &amp; Runtime</a> · Chapter 4 of <a href="README.md">The DynaJS Guide</a> · <a href="API.md">★ API Reference →</a></i></p>

# Chapter 4 — The Standard Library, Module by Module

Twenty-one native modules, all C17, all compiled into the binary with `CONFIG_NATIVE_MODULES=y`,
all imported under `dyna:`, all dependency-free.

**You don't need to read this front to back.** Find the capability you want in the map below, jump
to it, and steal the example.

> [!NOTE]
> Every snippet in this chapter was run against the real modules while writing this book — the
> printed outputs are what the binary produced, not what we expected it to produce.

### Find your module

| I want to… | Module | Section |
|---|---|---|
| Search one or many patterns at GiB/s, or measure how nearly two strings match | `dyna:matcher` | [4.1](#dynamatcher--compiled-search) |
| Read/write ints & floats in a byte buffer | `dyna:bytes` | [4.1](#dynabytes--the-byte-buffer-javascript-never-shipped) |
| Validate UTF‑8, transcode UTF‑16 or Latin‑1 | `dyna:bytes` | [4.1](#simd-text-kernels-in-the-same-module) |
| hex / base64 / base32 / Ascii85 / var-ints | `dyna:encoding` | [4.1](#dynaencoding--codecs-beyond-atob) |
| Parse JSON5, or hash a value canonically (RFC 8785) | `dyna:encoding` | [4.1](#json5-and-canonical-json) |
| Query a document with JSONPath | `dyna:encoding` | [4.12](#querying-a-document-jsonpath) |
| Parse or write XML | `dyna:xml` | [4.12](#xml) |
| Read a legacy charset (windows-125x, iso-8859-x, koi8) | `dyna:bytes` | [4.1](#legacy-charsets) |
| Read INI, .env or front matter | `dyna:config` | [4.13](#413-configuration-formats-dynaconfig) |
| Parse or build a URL, or a form body | `dyna:url` | [4.14](#414-urls-and-form-bodies-dynaurl) |
| Parse argv, style terminal output | `dyna:cli` | [4.18](#418-command-line-programs-dynacli) |
| Validate an e-mail, IBAN or card number | `dyna:validate` | [4.19](#419-format-validators-dynavalidate) |
| Log structured lines with levels | `dyna:log` | [4.15](#415-structured-logging-dynalog) |
| Parse Content-Type, Range, cookies or an ETag | `dyna:net` | [4.16](#416-http-message-codecs-dynanet) |
| Generate a NanoID or a ULID | `dyna:uuid` | [4.17](#417-compact-identifiers-dynauuid) |
| Hash something (SHA/MD5/CRC/xxhash) | `dyna:hash` | [4.2](#dynahash-and-dynacrypto--digests-macs-key-derivation) |
| MAC, derive a key, random bytes | `dyna:crypto` | [4.2](#dynahash-and-dynacrypto--digests-macs-key-derivation) |
| Generate UUIDs (v4, v7, v3/v5) | `dyna:uuid` | [4.2](#dynauuid--rfc-9562-uuids) |
| Reproducible random numbers | `dyna:random` | [4.2](#dynarandom--seedable-prng) |
| gamma / erf / gcd / isPrime / exact factorial | `dyna:mathx` | [4.3](#dynamathx--the-math-math-is-missing) |
| A heap, trie, LRU, bloom filter, sorted map… | `dyna:structures` | [4.4](#dynastructures--the-data-structures-javascript-is-missing) |
| Read/write files, stat, glob, temp dirs | `dyna:file` | [4.5](#dynafile--the-filesystem-module) |
| Bulk file reads at high queue depth (Linux, opt-in) | `dyna:uring` | [4.5](#dynauring--io_uring-bulk-file-read-linux-opt-in) |
| Serve HTTP / JSON‑RPC / WebSocket, or be a client | `dyna:net` | [4.6](#dynanet--http-a-client-an-application-server-and-a-static-reactor) |
| Parse IPs and CIDR ranges | `dyna:net` | [4.6](#dynanet--ip-addresses-and-cidr) |
| Resolve names, or talk to Redis / PostgreSQL / SQLite | `dyna:net` | [4.6](#dynanet--dns-and-clients-for-redis-postgresql-and-sqlite) |
| Durations, a monotonic clock, RFC 3339 | `dyna:time` | [4.7](#dynatime--durations-monotonic-clock-rfc-3339) |
| Vector math: dot, norms, GEMM, activations | `dyna:simd` | [4.8](#dynasimd--a-multi-isa-vector-math-engine) |
| Fit a model: regression, trees, SVM, clustering | `dyna:ml` | [4.8](#dynaml--classic-ml-models) |
| gzip / gunzip / LZ4 | `dyna:compress` | [4.9](#dynacompress--deflate-gzip-and-lz4) |
| Treat a CSV file like a small database | `dyna:csv` | [4.9](#dynacsv--csv-files-as-a-mini-database) |
| Filter, aggregate and group columns of numbers | `dyna:dataframe` | [4.9](#dynadataframe--columnar-analytics-over-typed-arrays) |
| Sort, or binary-search a sorted array | *(on `Array.prototype`)* | [4.9](#sorting--binary-search--on-arrayprototype) |
| Shortest paths, MST, topological sort | `dyna:structures` | [4.4](#graph--a-graph-with-the-algorithms-built-in) |
| Env vars, args, cwd, platform, pid, machine facts | `dyna:sys` | [4.10](#410-process-and-environment-dynasys) |
| Compare semantic versions / npm ranges | `dyna:semver` | [4.11](#411-semantic-versioning-dynasemver) |

---

## 4.0 How modules are shaped

Three shapes recur, and the difference between the last two is **what the object owns**, not which
module it came from.

| Shape | Modules | Lifecycle |
|---|---|---|
| **Plain-function** | `bytes`, `encoding`, `mathx`, `hash`, `crypto`, `uuid`, `netip`, `simd`, `time`, `semver`, `compress`, `sys`, most of `file` | none — call it, get a JavaScript value back |
| **Collection classes** | `structures`, `graph`, `matcher`, `random`, `hash`'s `Hasher` | plain garbage-collected objects with **no `.close()`**; the GC reclaims them (and any cycles through them) exactly like a `Map` |
| **Resource classes** | `file`'s `FileReader`/`FileWriter`, `http`, `csv`, `ml` | they own a descriptor, a socket, or megabytes of training data — `close()` them (or `[Symbol.dispose]()`, or a `DisposableStack`). Release is **deterministic**: memory returns immediately (Chapter 3 §3.2) |

A `Hasher` and a `Random` hold a few hundred bytes of arithmetic state, so they are plain objects; a
`FileWriter` holds a descriptor and an `SVC` can hold a large support-vector set, so those are
resources. **If a class has a `.close()`, releasing it early buys you something measurable.**

One rule the whole library obeys: **native results are copied into fresh JavaScript values at the
call boundary.** No native pointer escapes into the JS heap; nothing you hold can dangle.

---

## 4.1 Text & bytes

### `dyna:matcher` — compiled search

Search is a module because a compiled pattern is an object:

```js
import { Matcher, MultiMatcher } from "dyna:matcher";

const m = new Matcher("ss");
m.firstIn("mississippi");                 // 2
m.allIn("mississippi");                   // [2, 5]
m.replaceAllIn("mississippi", "S");       // "miSiSippi"

const alerts = new MultiMatcher(["ERROR", "FATAL", "panic:"]);
alerts.test("ERROR: disk full");          // true
alerts.test("all fine");                  // false
```

`Matcher` binds one pattern for readability — the search underneath is a SIMD kernel running at
multiple GiB/s, which is faster than any precomputed table, so there is no speed to be had from
compiling one. **`MultiMatcher` is the one that pays**: it compiles an Aho-Corasick automaton and
finds every pattern in a single pass, where N separate searches cost N passes.

**Approximate matching lives here too**, because "does this match, and how nearly" is
the same question:

```js
import { Levenshtein, DiceCoefficient } from "dyna:matcher";

Levenshtein("kitten", "sitting");             // 3, in CODE POINTS
Levenshtein("kitten", "sitting", { max: 2 }); // 3 — exceeded max, so max + 1
DiceCoefficient("healed", "sealed");          // 0.8
```

`max` bands the search rather than truncating the answer: it is exact while
`<= max`, so `d <= max` is always a correct "within max" test.

`DiffLines`, `DiffWords` and `DiffChars` return `{ op: -1|0|1, text }` hunks in
source order — Myers O(ND), minimal, with the shared prefix and suffix trimmed
first and an explicit stack instead of recursion:

```js
import { DiffWords } from "dyna:matcher";

for (const h of DiffWords("the quick brown fox", "the quick red fox"))
    print((h.op === 0 ? "  " : h.op < 0 ? "- " : "+ ") + h.text);
```

Concatenating the hunks with `op !== 1` rebuilds the first string and those with
`op !== -1` rebuild the second; that pair of properties is the oracle the tests
run on every diff they make.

**The single-string utilities live on `String.prototype`**, where the rest of the language's string
surface is, and their offsets are UTF-16 code units like everything else there:

```js
print("foobar".trimPrefix("foo"));                    // "bar"
print("xxhixx".trimChars("x"));                       // "hi"
print("hello".indexOfAny("le"));                      // 1
print(JSON.stringify("abababa".indexOfAll("aba")));   // [0,2,4]  — overlapping
print("Ada".equalsIgnoreCase("ADA"));                 // true
print(JSON.stringify("a,b,c,d".splitN(",", 2)));      // ["a","b,c,d"]
print("apple".compareBytes("banana"));                // -1
```

Two of those are worth calling out:

- **`splitN` keeps the tail.** `"a,b,c,d".split(",", 2)` **discards** everything past the limit;
  `splitN` keeps it as the final piece — almost always what a parser wants.
- **`compareBytes` orders by code point.** The `<` operator compares UTF-16 code units, and therefore
  sorts every non-BMP character before U+E000–U+FFFF.

**Terminal text is there too** — ANSI stripping, display width, wrapping and grapheme
clustering, over one shared CSI/OSC grammar and Unicode 15.1 width tables:

```js
const red = "\u001b[31m" + "warning" + "\u001b[0m";
print(red.stripAnsi());                    // "warning"
print(red.displayWidth());                 // 7   — escapes are invisible
print("\u4f60\u597d".displayWidth());      // 4   — two wide characters
print("hello world".wrapAnsi(5));          // "hello\nworld"
print("e\u0301x".graphemes().length);      // 2   — clusters, not code units
```

`displayWidth` counts **grapheme clusters**, so an emoji ZWJ sequence is 2 cells
rather than the sum of its parts, and `wrapAnsi` re-emits the active SGR state
after each break so colour survives the newline.

### `dyna:bytes` — the byte buffer JavaScript never shipped

`DataView` gives you one accessor at a time and nothing else. `dyna:bytes` is the ergonomic, fast
byte toolkit — comparison, search, concatenation, fixed-width accessors, and the UTF-8 boundary.

```js
import { fromUtf8, toUtf8, indexOf, count, concat } from "dyna:bytes";
import { HexEncode, HexDecode, Base64Encode } from "dyna:encoding";

print(HexEncode(new Uint8Array([0xde, 0xad, 0xbe, 0xef])));  // "deadbeef"
print(Base64Encode(fromUtf8("hi")));                         // "aGk="

const a = fromUtf8("the quick brown fox");
print(indexOf(a, fromUtf8("quick")));                        // 4
print(count(a, fromUtf8("o")));                              // 2
print(HexEncode(concat([HexDecode("dead"), HexDecode("beef")])));  // "deadbeef"
```

Note the two imports. **`dyna:bytes` does byte manipulation and the UTF-8 boundary; `dyna:encoding`
owns every binary-to-text codec.** One owner per capability, one set of names, one return type per
decoder.

Every buffer argument is a **byte-addressed view** (`Uint8Array`, `Int8Array`, `Uint8ClampedArray`,
`DataView`) or a plain `ArrayBuffer`, always read through the view's own `byteOffset`/`byteLength`
window. A `DataView` works directly, so you can mix these functions with the engine's own accessors
on the same bytes.

A **wider** view (`Uint16Array`, `Float64Array`, …) is rejected rather than silently reinterpreted —
with a multi-byte element type an `offset` would be ambiguous — so state the reinterpretation with
`bytesOf`:

```js
import { bytesOf, readDoubleLE } from "dyna:bytes";

const f = new Float64Array([1.5, -2.5]);
print(readDoubleLE(bytesOf(f), 8));   // -2.5   (bytesOf aliases f, it does not copy)
```

The fixed-width accessors cover every width and both byte orders — `readUint8` … `readUint32BE/LE`,
`readFloatBE/LE`, `readDoubleBE/LE`, and `readBigInt64BE/LE` / `readBigUint64BE/LE` for the 64-bit
integers `Number` cannot hold exactly. Each has a matching `write*`.

#### SIMD text kernels, in the same module

Lower-level, throughput-oriented text operations built directly on the SIMD engine: UTF-8 validation
and code-point counting, Latin-1↔UTF-8, and UTF-8↔UTF-16 transcoding — the primitives a parser or
protocol codec leans on.

```js
import { isValidUtf8, countUtf8, latin1ToUtf8 } from "dyna:bytes";

print(isValidUtf8(new Uint8Array([0xc3, 0xa9])));        // true  (é)
print(countUtf8(new Uint8Array([0xc3, 0xa9, 0x61])));    // 2     (code points, not bytes)
print(Array.from(latin1ToUtf8(new Uint8Array([0xe9]))).map(b => b.toString(16)).join(" "));
//   "c3 a9"   (Latin-1 é → UTF-8, vectorized)
```

```js
import { utf8ToUtf16, utf16ToUtf8, isValidUtf16, countUtf16 } from "dyna:bytes";

const u16 = utf8ToUtf16("hello 世界 😀");           // → UTF-16LE bytes (Uint8Array)
print(countUtf16(u16));                            // 10  (code points)
print(isValidUtf16(u16));                          // true — round-trips exactly
print(isValidUtf16(new Uint8Array([0x00, 0xD8]))); // false (lone high surrogate)
```

These accept `Uint8Array` views, `ArrayBuffer`, or strings, and run at multiple GiB/s on long
inputs — this is the code the engine itself reuses for HTTP header scanning.

The transcoding policy is **strict and lossless**: a lone or misordered surrogate is an error (the
transcoders throw; `isValidUtf16` returns false), never silently replaced with U+FFFD.

### `dyna:encoding` — codecs beyond `atob`

Hex, standard and URL-safe base64, base32 and base32hex, Ascii85/base85, and LEB128 var-ints — the
encodings a data-plane program actually needs. `atob`/`btoa` cover only standard base64 of Latin-1
strings; this module covers the rest, over bytes, natively.

```js
import { HexEncode, HexDecode, Base64URLEncode,
         Base85Encode, Base85Decode, PutUvarint, Uvarint } from "dyna:encoding";

print(HexEncode(new Uint8Array([1, 255])));             // "01ff"
print(Base64URLEncode(new Uint8Array([251,255,191])));  // "-_-_"  (URL-safe alphabet)

// Ascii85 — compact binary-to-text, round-trips exactly:
const a85 = Base85Encode(HexDecode("deadbeef"));
print(a85, "→", HexEncode(Base85Decode(a85)));          // hQ=N\ → deadbeef

// LEB128 var-ints (the wire format Protobuf and DWARF use):
const encoded = PutUvarint(300);                        // Uint8Array [0xac, 0x02]
const [value, read] = Uvarint(encoded);                 // [300, 2]  (value, bytes read)
print(value, "in", read, "bytes");                      // 300 in 2 bytes
```

---

## 4.2 Cryptographic hashing & identity

### `dyna:hash` and `dyna:crypto` — digests, MACs, key derivation

Two modules, split by **what the operation is for** rather than by algorithm family:

| Module | Holds | Why |
|---|---|---|
| `dyna:hash` | SHA-1/224/256/384/512, SHA-3/Keccak-256/SHAKE, BLAKE3, BLAKE2b/2s, MD5, CRC-32, CRC-32C, xxHash32/64, Murmur3-128, streaming `Hasher` | reduces bytes to a tag with **no secret involved** — checksums, cache keys, content ids |
| `dyna:crypto` | HMAC (one-shot and streaming `Hmac`), HKDF, PBKDF2, `TimingSafeEqual`, `RandomBytes` | everything that depends on **a secret or on constant-time execution** |

That puts MD5 and SHA-1 in `dyna:hash`, which is the correct signal: they are fine as content ids
and are not security primitives. It also stops CRC-32 sitting next to HMAC.

```js
import { SHA256Hex, CRC32, XXHash64, Hasher } from "dyna:hash";

print(SHA256Hex("hello world"));
//   b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
print(CRC32(new Uint8Array([1, 2, 3])) >>> 0);  // 1438416925   IEEE CRC-32, unsigned
print(XXHash64("hello world"));                 // "45ab6734b21e6968"  hex, not a lossy number

// Streaming: hash data you never fully hold in memory. update() is chainable,
// and the Hasher is a plain object — nothing to close.
const h = new Hasher("sha256");
h.update("part one ").update("part two");
print(h.digestHex() === SHA256Hex("part one part two"));   // true
```

```js
import { HMACHex, Hmac, HKDF, PBKDF2, RandomBytes } from "dyna:crypto";

print(HMACHex("sha256", "key", "message"));    // keyed MAC, hex
//   6e9ef29b75fffc5b7abae527d58fdadb2fe42e7219011976917343065f58ed4a

const mac = new Hmac("sha256", secretKey);     // the key is the configuration
if (!mac.verify(body, headerTag)) reject();    // constant-time, not ===

const key    = HKDF({ key: masterSecret, salt, info: "session", length: 32 });
const stored = PBKDF2({ hash: "sha256", password, salt, iterations: 600000, length: 32 });
const nonce  = RandomBytes(24);                // OS entropy, not the seeded PRNG
```

> [!IMPORTANT]
> **Use `mac.verify(msg, tag)` rather than `signHex(msg) === tag`.** String equality exits at the
> first differing character, which publishes how much of the MAC an attacker has already guessed.

Everything is verified against the published standard vectors — FIPS 180-4, RFC 1321, RFC 2104/4231,
IEEE 802.3, RFC 5869 for HKDF, RFC 6070 and RFC 7914 for PBKDF2, and the xxHash specification. A key
derivation function checked only against itself agrees with its own bugs, and the failure is silent:
you get a good-looking key no other implementation derives.

### `dyna:uuid` — RFC 9562 UUIDs

```js
import { v4, v7, v5, version, validate, NAMESPACE_DNS } from "dyna:uuid";

print(v4());                              // random
print(v7());                              // time-ordered: sorts by creation time
print(v5(NAMESPACE_DNS, "www.example.com"));
//   2ed6657d-e927-568b-95e1-2665a8aea6a2   (deterministic, name-based)
print(version(v7()));                     // 7
print(validate("not-a-uuid"));            // false
```

**Why v7 matters:** v4 UUIDs are random, so as primary keys they scatter B-tree inserts. v7 embeds a
millisecond timestamp in the high bits, so freshly-minted ids increase monotonically — index locality
without a central sequence. JavaScript has no built-in UUID generator at all; DynaJS ships v4, v7,
and the name-based v3/v5.

### `dyna:random` — seedable PRNG

A fast, **seedable** generator — reproducible streams for tests, simulations, and sampling
(`Math.random` is neither seedable nor reproducible).

```js
import { Random } from "dyna:random";

const rng = new Random(42);               // seed with a Number or BigInt
print(rng.nextFloat().toFixed(4));        // 0.0839  (deterministic for seed 42)
print(rng.nextBounded(6) + 1);            // a fair die roll in [1,6]
print(rng.nextU64());                     // a full 64-bit BigInt

const buf = new Uint8Array(16);
rng.fill(buf);                            // fill a buffer with random bytes

// Same seed ⇒ identical stream (Number and BigInt seeds agree):
print(new Random(7).nextU64() === new Random(7n).nextU64());   // true
```

---

## 4.3 Numbers

### `dyna:mathx` — the math `Math` is missing

Special functions, exact integer helpers, and constants that `Math` never included.

```js
import { gamma, erf, hypot, gcd, lcm, factorial, isPrime, cbrt, Phi } from "dyna:mathx";

print(gamma(5));                  // 24        (Γ(5) = 4!)
print(erf(1).toFixed(6));         // 0.842701  (the error function)
print(hypot(3, 4));               // 5         (overflow-safe)
print(cbrt(27));                  // 3
print(gcd(462n, 1071n));          // 21n
print(lcm(4n, 6n));               // 12n
print(factorial(25));             // 15511210043330985984000000n  (exact BigInt)
print(isPrime(1000000007n));      // true
print(Phi);                       // 1.618033988749895  (golden ratio)
```

`gcd`/`lcm`/`factorial` return `BigInt` because their results routinely exceed `Number` range —
`factorial(25)` is a 26-digit integer, computed exactly. `isPrime` is a deterministic Miller-Rabin
test valid across the full 64-bit range.

The module goes well beyond that list:

| Family | Members |
|---|---|
| Special functions | `besselj` `bessely` `besseli` `besselk` `besselh` · `ellipj` `ellipke` · `gammainc`/`gammaincinv` · `betainc`/`betaincinv` · `erfinv` `erfcinv` `erfcx` · `legendre` `legendreP` · `psi` `polygamma` · `expint` |
| Float introspection | `frexp` `ldexp` `nextafter` `eps` `logb` `ilogb` `scalbn` `signbit` `copysign` `realmax` `realmin` `flintmax` |
| Integer & bit helpers | `bitLen` `popcount` `nextpow2` `pow2` `idivide` `mod` `rem` `fix` `roundToEven` · the `bits` namespace |
| Combinatorics | `nchoosek` `perms` `primes` `factor` `rat` |
| Array builders | `linspace` `logspace` `CUM_SUM` `CUM_PROD` `DIFF` |

```js
import { frexp, nextafter, nchoosek, linspace } from "dyna:mathx";

print(JSON.stringify(frexp(8)));      // [0.5,4]     mantissa and exponent
print(nextafter(1, 2));               // 1.0000000000000002   the next representable double
print(nchoosek(10, 3));               // 120
print(linspace(0, 1, 5).join(","));   // 0,0.25,0.5,0.75,1
```

See the [API Reference](API.md#mathx) for the full set.

---

## 4.4 Data structures

### `dyna:structures` — the data structures JavaScript is missing

`Array`, `Map`, `Set`, and the TypedArrays are engine intrinsics — already native C, already fast —
so this module does **not** reimplement them. It ships only what the language has no builtin for.

| Need | Class | What it gives you |
|---|---|---|
| **Priority queue** | `Heap` | a binary heap driven by your comparator — the priority queue JS never shipped |
| **Set membership, packed** | `BitSet` | a dynamic bit set with word-parallel `and`/`or`/`xor` and popcount `COUNT` |
| **Probabilistic membership** | `BloomFilter` | O(1)-space "definitely-absent / maybe-present" tests, no false negatives |
| **Disjoint sets** | `UnionFind` | near-O(1) `union`/`connected`/component `COUNT` (Kruskal, clustering, connectivity) |
| **Both-ended queue** | `Deque` / `List` | O(1) `push`/`pop` at *both* ends — `Deque` is array-backed (indexable); `List` is a doubly-linked list (stable, iterable) |
| **Sliding window** | `RingBuffer` | fixed capacity, `push` overwrites the oldest (keeps the last *N*) |
| **Ordered collections** | `SortedMap` / `SortedSet` | numeric keys kept sorted; `FIRST`/`LAST`/`FLOOR`/`CEIL`/`rangeQuery` |
| | `BTree` | the same surface, B+ shaped -- ~2x on lookup and update, and an ordered scan that walks linked leaves |
| **Prefix lookups** | `Trie` | a string set with `keysWithPrefix` and `longestPrefix` |
| **Range aggregates** | `Fenwick` / `SegTree` | O(log n) point update + prefix / range `SUM` (Fenwick) or sum/min/max (SegTree) |
| **Bounded cache** | `LRU` | a capacity-bounded cache that evicts the least-recently-used entry, with optional per-entry TTL, an `onEvict` callback and hit/miss counters |
| **Graphs** | `Graph` | an adjacency list whose traversals, shortest paths and spanning trees are native methods |

```js
import { Heap, BitSet, UnionFind, Deque, Fenwick, SortedMap, Trie, LRU } from "dyna:structures";

const pq = new Heap();                                   // numeric min-heap, compared in C
// new Heap((a, b) => b - a) for a max-heap, or any other order
[5, 1, 4, 2, 8].forEach(v => pq.push(v));
const drained = []; while (pq.size) drained.push(pq.pop());
print(drained.join(","));                                // "1,2,4,5,8"

const seen = new BitSet();
seen.set(3).set(65).set(130);
print(seen.count, seen.get(65), seen.nextSet(4));        // 3 true 65

const uf = new UnionFind(6);
uf.union(0, 1); uf.union(2, 3); uf.union(1, 3);
print(uf.connected(0, 2), uf.count);                     // true 3   ({0,1,2,3},{4},{5})

const dq = new Deque();
dq.pushBack(1); dq.pushFront(0);                         // O(1) at both ends
print(dq.toArray().join(","), dq.popFront());            // "0,1" 0

const f = new Fenwick(8);
f.update(0, 5); f.update(3, 2);
print(f.prefixSum(3), f.rangeQuery(1, 3));               // 7 2

const sm = new SortedMap();
sm.set(30, "c"); sm.set(10, "a"); sm.set(20, "b");
print(sm.keys().join(","), sm.floorKey(25));             // "10,20,30" 20

const t = new Trie();
t.insert("car"); t.insert("card"); t.insert("cat");
print(t.keysWithPrefix("car").SORT().join(","));         // "car,card"

const cache = new LRU(2);
cache.put("a", 1); cache.put("b", 2); cache.get("a"); cache.put("c", 3);
print(cache.has("b"), cache.get("a"));                   // false 1   (b was evicted)
```

**Lifetime: these are plain garbage-collected objects, exactly like `Map` and `Set`.** There is
nothing to close and nothing to dispose. Create one, use it, and the engine reclaims it (and any
values it holds — even a reference cycle *through* it) when it becomes unreachable:

```js
function countDistinct(ids) {
    const seen = new BitSet();
    for (const id of ids) seen.set(id);
    return seen.count;
}                                       // `seen` is collected here, like a Map would be
```

### The collection types, and the sketches

The same module carries a second family: richer keyed collections, and two probabilistic counters
that stand alongside `BloomFilter`. Same `import`, one module, one charter.

| Need | Class | What it gives you |
|---|---|---|
| **Count occurrences** | `Multiset` | a set that counts. `size` is distinct keys, `totalSize` the sum |
| **One key, many values** | `Multimap` | duplicates kept, insertion order preserved; `entries()` yields one pair *per value* |
| **Unique in both directions** | `BiMap` | a string↔string map with an O(1) inverse. `set` **throws** if the value is taken — that refusal is what makes the inverse a function |
| **Two-dimensional key** | `Table` | sparse `(row, column) → value`; O(1) cell access, O(size) row/column projections |
| **Which spans are covered** | `RangeSet` | half-open `[lo, hi)` intervals that **coalesce** — always the minimal disjoint list, plus `complement` for the gaps |
| **What value applies here** | `RangeMap` | `[lo, hi) → value`, disjoint but **not** coalesced; the newest `put` trims or splits what it overlaps |
| **Which intervals overlap** | `IntervalTree` | closed `[lo, hi]` with a payload, kept distinct — the question `RangeSet` destroys by merging |
| **Both ends of a priority queue** | `MinMaxHeap` | O(log n) `popMin` *and* `popMax`. Priority is a number, not a comparator, so the sift loop never leaves C |
| **How often, in fixed space** | `CountMinSketch` | frequency estimates that never under-count; error bounded by `totalCount / width` |
| **How many distinct, in fixed space** | `HyperLogLog` | 0.81% standard error in 16 KiB at the default precision, whatever the cardinality; sketches `merge` |

```js
import { Multiset, Multimap, BiMap, Table, RangeSet, RangeMap,
         IntervalTree, MinMaxHeap, CountMinSketch, HyperLogLog } from "dyna:structures";

const words = new Multiset();
for (const w of "the cat the hat the end".split(" ")) words.ADD(w);
print(words.COUNT("the"), words.size, words.totalSize);        // 3 4 6

const tags = new Multimap();
tags.put("post1", "js").put("post1", "c").put("post2", "js");
print(tags.get("post1").join(","), tags.keyCount);             // "js,c" 2

const codes = new BiMap();
codes.set("US", "1").set("FR", "33");
print(codes.get("FR"), codes.keyOf("1"));                      // "33" "US"

const grid = new Table();
grid.put("r1", "qty", 7).put("r1", "price", 2.5);
print(grid.get("r1", "qty"), grid.row("r1").length);           // 7 2

const covered = new RangeSet();                                // 8..10 overlaps, so it merges
covered.ADD(0, 10).ADD(8, 20).ADD(30, 40);
print(JSON.stringify(covered.ranges()), covered.measure);      // [[0,20],[30,40]] 30
print(JSON.stringify(covered.complement(0, 40)));              // [[20,30]]   — the gap

const tiers = new RangeMap();                                  // a lookup table over ranges
tiers.put(0, 100, "free").put(100, 1000, "pro");
print(tiers.get(250));                                         // "pro"

const cal = new IntervalTree();                                // overlaps are kept, not merged
cal.insert(9, 10, "standup").insert(9.5, 11, "review");
print(cal.overlapping(9.75, 9.8).length);                      // 2   — double-booked

const win = new MinMaxHeap();
[42, 7, 19, 3].forEach(v => win.push(v));
print(win.peekMin(), win.peekMax(), win.popMax());             // 3 42 42

const freq = new CountMinSketch(2048);
for (let i = 0; i < 500; i++) freq.ADD("hot");
print(freq.COUNT("hot"), freq.totalCount);                     // 500 500

const uniq = new HyperLogLog();
for (let i = 0; i < 100000; i++) uniq.ADD("user" + i);
print(uniq.COUNT() | 0);                                       // 100330  — 0.33% off
```

The sketches are the ones to reach for deliberately. `CountMinSketch` and `HyperLogLog` answer in a
**fixed** amount of memory no matter how much you feed them, and both `merge`: per-shard sketches
combine into a global answer without ever shipping the keys. The price is that neither can enumerate
what it saw, and `HyperLogLog`'s count is an estimate — see the API reference for the error bounds.

### `Graph` — a graph, with the algorithms built in

A native graph whose traversal, shortest-path, and spanning-tree algorithms are **methods** — each is
one JS→C call over a native adjacency list, not an interpreted loop. Nodes are integer ids
(`addNode` returns the next id; `addEdge` auto-creates its endpoints).

```js
import { Graph } from "dyna:structures";

const g = new Graph({ directed: true, weighted: true });
const [a, b, c] = [g.addNode(), g.addNode(), g.addNode()];
g.addEdge(a, b, 2).addEdge(b, c, 3).addEdge(a, c, 10);

g.bfs(a);                 // [0, 1, 2]        traversal order
g.dijkstra(a);            // [0, 2, 5]        shortest distance to every node
g.dijkstra(a, c);         // 5                to one destination
g.topologicalSort();      // [0, 1, 2]        (throws if cyclic)
```

The full method set: `bfs`/`dfs`, `dijkstra` (rejects negative weights), `bellmanFord` (allows
negatives, detects negative cycles), `floydWarshall`, `topologicalSort`, `connectedComponents`,
`mst` (Kruskal), and `aStar(src, dst, heuristic)`. Every result is a fresh JS array or object; the
`Graph` itself is a plain garbage-collected object.

### Persistence: `serialize()` / `deserialize()`

Every container writes itself to a compact binary blob and reads back, so a warmed cache or a built
sketch survives a restart:

```js
import { Multiset } from "dyna:structures";

const blob = words.serialize();             // Uint8Array — 88 bytes for the Multiset above
const restored = Multiset.deserialize(blob);
print(restored.COUNT("the"));               // 3
```

`serialize` is an instance method — it writes *this* container. `deserialize` is **static**, because
there is no instance yet, and it **refuses a record of any other type**: handing `Multiset` a `Trie`
record is a `TypeError` naming both, not a surprise object.

---

## 4.5 Files & paths

### `dyna:file` — the filesystem module

One module for the whole filesystem:

| Group | Functions |
|---|---|
| **Content, one-shot** | `readFile`, `writeFile` |
| **Content, buffered** | `FileReader`, `FileWriter`, and the `File` handle |
| **Metadata** | `stat`, `lstat`, `exists` |
| **Directories** | `readDir`, `makeDir`, `remove`, `removeAll`, `rename` |
| **Links & modes** | `symlink`, `readLink`, `realPath`, `chmod` |
| **Patterns** | `glob`, and the compiled `Glob` class |
| **Temporaries** | `tempDir`, `makeTempDir`, `makeTempFile` |
| **Path strings** | the `Path` handle every one of these functions takes |

Under the hood the content I/O uses the platform's best primitives — `F_RDAHEAD`/`F_PREALLOCATE`/
`F_FULLFSYNC` on macOS, `fadvise`/`fallocate`/io_uring on Linux — behind one identical API.
Everything is synchronous.

```js
import { readFile, writeFile, FileReader, FileWriter, Path } from "dyna:file";

// One-shot:
writeFile(new Path("/tmp/demo.txt"), "line one\nline two\n");
print(readFile(new Path("/tmp/demo.txt")).length);        // 18

// Buffered writer with preallocation (fewer syscalls, no fragmentation):
const w = new FileWriter(new Path("/tmp/big.txt"), { bufferSize: 1 << 16, preallocate: 1 << 20 });
for (let i = 0; i < 100000; i++) w.write(`row ${i}\n`);
w.sync();                                        // durable flush (F_FULLFSYNC on macOS)
w.close();

// Buffered reader, line by line across buffer refills:
const r = new FileReader(new Path("/tmp/big.txt"), { bufferSize: 1 << 16 });
let lines = 0, line;
while ((line = r.readLine()) !== null) lines++;
r.close();
print("read", lines, "lines");                  // read 100000 lines
```

`writeFile` returns the byte count; `readLine()` returns `null` at EOF; `sync()` forces a durable
flush. The buffering and preallocation matter: a naïve write-per-line loop is syscall-bound, while
this batches into large buffers and preallocates the file extent.

**`Path` is a value, not a string**, and it does the path-string algebra:

```js
import { Path } from "dyna:file";

const p = new Path("/var/log/app.tar.gz");
print(String(p.dirname), String(p.basename), p.extname, p.isAbsolute);
//   /var/log app.tar.gz .gz true
print(String(p.join("..", "x.txt")));            // /var/log/x.txt
```

**`File` binds a path once** and gives you the whole per-file surface as methods — handy when one
path is read, appended to and stat'd repeatedly:

```js
import { File, Path } from "dyna:file";

const f = new File(new Path("/tmp/handle.txt"));
f.writeText("one\n");
f.append("two\n");
print(JSON.stringify(f.readText()));             // "one\ntwo\n"
print(f.exists(), f.stat().size);                // true 8
f.remove();
```

### Directories, globbing and temp trees

```js
import { stat, readDir, makeDir, removeAll, glob, exists,
         makeTempDir, writeFile } from "dyna:file";

const root = makeTempDir("demo-");               // a Path, not a string
makeDir(root.join("a", "b"), { recursive: true });
writeFile(root.join("a", "x.txt"), "hi");
writeFile(root.join("a", "b", "y.js"), "1");

print(readDir(root.join("a")).map(e => e.name + (e.isDir ? "/" : ""))); // ["b/", "x.txt"]
print(glob(root.join("**", "*.js")).length);     // 1   — ** / * / ? / [a-c] / [!…]
print(stat(root.join("a", "x.txt")).size, stat(root.join("a")).isDir);  // 2 true

removeAll(root);                                 // recursive, symlink-safe, missing = no-op
print(exists(root));                             // false
```

Three details worth relying on:

- **`glob` is a self-contained matcher** (`*`, `**`, `?`, `[...]`, ranges, negation), symlink-cycle-safe.
- **`removeAll` uses `openat` + `O_NOFOLLOW`**, so it can never delete outside the tree through a symlink.
- **Every error throws an `Error` carrying `.code`** (`"ENOENT"`) and `.errno`; `readDir` returns
  sorted `{ name, isDir, isFile, isSymlink }` entries.

### `dyna:uring` — io_uring bulk file read (Linux, opt-in)

A high-queue-depth whole-file reader and checksummer built on **io_uring**: true async disk I/O for
throughput-bound bulk reads. io_uring submits many read requests without a syscall per block, so
large sequential reads saturate the device.

This is the one module that is not in every build. It compiles only when all three hold — Linux,
`CONFIG_IO_URING=y`, and `liburing` available — so `import "dyna:uring"` throws everywhere else, and
portable code uses `dyna:file` instead.

```js
// Linux, built with CONFIG_IO_URING=y:
import { readFile, checksum } from "dyna:uring";

const data = readFile(new Path("/var/log/big.log"));
print(data.length, "bytes");
print("crc:", checksum("/var/log/big.log"));    // streamed, high queue depth
```

---

## 4.6 Networking

### `dyna:net` — HTTP: a client, an application server, and a static reactor

Three tools on one foundation: a synchronous **`HTTPClient`**, a full application server (**`App`**)
where *you* write the handlers, and a lower-level static-only reactor (`HTTPServerAsync`). All three
sit on the same single-thread event-loop reactor — kqueue on macOS, epoll or io_uring on Linux — so
they scale to thousands of connections on one thread.

**The client** is the simplest piece — a blocking request/response object:

```js
import { HTTPClient } from "dyna:net";

const client = new HTTPClient();
try {
  const r = client.get("http://example.com/");
  print(r.status);                       // 200
  print(r.headers["Content-Type"]);      // e.g. "text/html; charset=UTF-8"
  print(r.body.length, "bytes");
} finally {
  client.close();
}
```

A response is a plain `{ status, headers, body }` object. `client.post(url, body, headers?)` and the
general `client.request(url, { method, headers, body })` round it out.

**`App` is the server you build on.** You never touch raw HTTP — you register a handful of *typed
routes* and DynaJS takes care of the wire protocol, parsing, and connection lifecycle:

| Route | What it does |
|---|---|
| `app.rpc(path, methods)` | a strict **JSON-RPC 2.0** endpoint. Each method is just `(params) => result`; DynaJS parses the request, calls the right one, and serializes the reply (including proper JSON-RPC error objects). No REST guesswork. |
| `app.static(prefix, dir, opts)` | serve files from a directory over a zero-copy `sendfile` path. `opts.allow` is a whitelist of `.ext`s or MIME types; `opts.maxFileSize` bounds them. |
| `app.upload(path, opts, handler)` | stream an upload straight to disk, then call `handler(savedPath, meta)` with where it landed (`meta` has `size` and `contentType`). |
| `app.ws(path, handlers)` | a full **RFC 6455 WebSocket** endpoint — `{ open, message, close }`. |

<!-- check:skip -->
```js
import { App } from "dyna:net";
import { Path } from "dyna:file";

const app = new App({ port: 8080 });

// Business logic — a JSON-RPC 2.0 service:
app.rpc("/rpc", {
  add:   ([a, b]) => a + b,
  greet: ({ name }) => `hello ${name}`,
});

// Static files, extension-whitelisted:
app.static("/assets", new Path("/var/www/assets"),
           { maxFileSize: 8 << 20, allow: [".css", ".js", ".png"] });

// Uploads streamed to disk, then handed to you:
app.upload("/upload", { dir: new Path("/var/uploads"), maxFileSize: 32 << 20, allow: ["image/png"] },
  (savedPath, meta) => print("saved", meta.size, "bytes to", savedPath));

// A WebSocket echo endpoint:
app.ws("/ws", {
  open:    (ws) => ws.send("welcome"),
  message: (ws, data, isBinary) => ws.send(isBinary ? data : "echo: " + data),
  close:   (ws) => {},
});

app.start();
print("serving on", app.port);
```

Because the server runs on *this program's* event loop, you drive it from another process. From a
shell — a real, verified round-trip:

```sh
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"add","params":[2,3]}' \
  http://127.0.0.1:8080/rpc
#   {"jsonrpc":"2.0","result":5,"id":1}
```

A few things worth knowing:

- **Handlers run on the JS thread**, right on the reactor loop — so a handler shares the same heap
  as the rest of your program (no cross-thread copying), and a request arrives as a zero-copy view.
  The catch: **don't block the loop.** A CPU-heavy handler stalls every other connection — push that
  work to an `os.Worker` (Chapter 3 §3.4). For the same reason, a *same-thread* blocking `HTTPClient`
  can't call your own `App` (one thread can't both wait and serve) — drive it from outside.
- **Give `App` an explicit port.** `app.port` reports the port you asked for, so `port: 0` is not
  resolved to an OS-assigned one — pick a real port.
- **An RPC method may return a value or a `Promise`** for single requests; a *batched* JSON-RPC call
  needs synchronous methods.

**`HTTPServerAsync` is the bare static reactor underneath.** It maps paths to fixed responses and
serves them without ever entering the JavaScript world — good for static content and for showing off
the reactor's raw concurrency, but it **cannot run your code.** For anything with logic, use `App`.

```js
import { HTTPServerAsync, HTTPClient } from "dyna:net";

const server = new HTTPServerAsync({ port: 0, routes: {   // port 0 → a free OS port
  "/":     "hello world\n",
  "/json": { status: 200, contentType: "application/json", body: '{"a":1}' },
}});
server.start();

const client = new HTTPClient();
print(client.get(`http://127.0.0.1:${server.port}/json`).status);   // 200
client.close();
server.stop();
```

(`HTTPServer` is a thread-pool variant with the same `start`/`stop`/`port` shape; prefer the reactor.)

### `dyna:net` — IP addresses and CIDR

Parsing and reasoning about IPv4/IPv6 addresses and CIDR prefixes — a capability JavaScript lacks
entirely. This is the module for allow-lists, subnet routing, and address classification.

```js
import { parseAddr, contains, masked, canonical, isValid, isLoopback } from "dyna:net";

const a = parseAddr("::ffff:127.0.0.1");
print(a.is4, a.is6);                     // false true   (a mapped address is an IPv6 value)
print(isLoopback("::ffff:127.0.0.1"));   // true         (classifiers unmap IPv4-in-IPv6 first)
print(canonical("2001:0db8:0000:0000:0000:0000:0000:0001")); // "2001:db8::1"  (RFC 5952)
print(contains("10.0.0.0/8", "10.1.2.3"));                   // true
print(contains("192.168.0.0/16", "10.0.0.1"));               // false
print(masked("192.168.5.130/24"));                           // "192.168.5.0"
print(isValid("999.1.1.1"));                                 // false
```

One important detail: **`parseAddr` keeps the 16-byte form**, so `parseAddr("::ffff:127.0.0.1")` is
an IPv6 value and `is4` is `false`. The *classifiers* (`isLoopback`, `isPrivate`, …) unmap an
IPv4-in-IPv6 address first, so `isLoopback("::ffff:127.0.0.1")` is `true`. `canonical` produces the
RFC 5952 form (longest zero-run compressed, lowercase); `contains` does the masked-prefix comparison
at any bit boundary.

### `dyna:net` — DNS, and clients for Redis, PostgreSQL and SQLite

The same reactor also carries a resolver and three data-store clients. All four are in `dyna:net`
because they are network endpoints with the same lifecycle: construct, use, `close()`.

```js
import { DNSResolver, Redis, PostgreSQL, SQLite } from "dyna:net";

const r = new DNSResolver({ server: "1.1.1.1" });
r.query("example.com", 1, (err, recs) => { print(recs[0].address); r.close(); });

const cache = new Redis({ host: "127.0.0.1", port: 6379 });
await cache.command("SET", "k", "v");
await cache.command("GET", "k");                        // "v"
await cache.pipeline([["INCR", "n"], ["GET", "k"]]);    // one round trip
cache.close();

const db = new PostgreSQL({ user: "app", database: "shop", password: "…" });
const res = await db.query("SELECT id, name FROM users WHERE active");
res.rows[0].name;                                       // "ada"
await db.query("INSERT INTO events(kind) VALUES ($1)", ["login"]);
db.close();

const local = new SQLite(":memory:");
local.exec("CREATE TABLE t (name TEXT)");
local.exec("INSERT INTO t VALUES (?)", ["alice"]);
local.query("SELECT * FROM t WHERE name = ?", ["alice"]);
local.close();
```

Four things are worth knowing before you reach for them, because each is a decision rather than an
omission:

**Neither Redis nor PostgreSQL speaks TLS, and both refuse it by name.** `tls: true` throws at
construction rather than connecting in plaintext, and a peer that answers with a TLS record is
reported as exactly that. PostgreSQL authenticates with SCRAM-SHA-256 and refuses cleartext and MD5
unless `insecureAuth` is set — SCRAM protects the credential, not the session, so terminate TLS in
front of the server or keep the traffic on a Unix socket.

**A malformed reply destroys the connection.** Both clients match replies to requests by position, so
a client that tries to find its place again after a parse error is one that can return one key's
value for another. There is no resynchronisation.

**Values that a double cannot hold stay text.** A Redis integer or a PostgreSQL `int8` past 2^53, a
`numeric`, a timestamp — all come back as strings, because rounding them silently is worse than
making you parse them. `parameters` are always sent length-prefixed and separate from the statement.

**The resolver defends against forged answers on four independent axes** — a connected socket, a
randomised source port, an id from the OS entropy source, and the question echoed back — so a
spoofing attempt produces a timeout rather than someone else's address.

Full reference, including every option and the reply mapping: [API.md](API.md).

---

## 4.7 Time

### `dyna:time` — durations, monotonic clock, RFC 3339

`Date` handles wall-clock timestamps. `dyna:time` adds nanosecond-precise typed durations, a
monotonic clock, and precise formatting and parsing.

```js
import { Minute, durationString, parseDuration,
         monotonicNano, formatRFC3339, fromUnix } from "dyna:time";

print(durationString(90 * Number(Minute)));   // "1h30m0s"
print(parseDuration("1h30m"));                // 5400000000000  (nanoseconds)
print(formatRFC3339(fromUnix(0, 0)));         // "1970-01-01T00:00:00Z"

// Monotonic clock for measuring elapsed time (immune to wall-clock jumps):
const t0 = monotonicNano();
for (let i = 0; i < 1e6; i++) { /* work */ }
const elapsedMs = Number(monotonicNano() - t0) / 1e6;
print(`took ${elapsedMs.toFixed(2)} ms`);
```

`monotonicNano()` is the right clock for benchmarks and timeouts — unlike `Date.now()`, it never
goes backwards. `formatRFC3339`/`parseRFC3339` handle the interchange format APIs actually use, and
`Nanosecond`/`Microsecond`/`Millisecond`/`Second`/`Minute`/`Hour` are the duration constants.

---

## 4.8 Compute: SIMD & ML

### `dyna:simd` — a multi-ISA vector-math engine

DynaJS's signature capability. A native SIMD kernel set — dispatched at runtime to the best
instruction set your CPU has (scalar / NEON / SSE4.2 / AVX2 / AVX-512 / SVE) — exposed directly to
JavaScript over typed arrays. No native addon, no build step.

```js
import { dot, sum, normL2, distCos, axpy, softmax, gemm, argmax, f64Sum } from "dyna:simd";

const a = new Float32Array([1, 2, 3, 4]);
const b = new Float32Array([5, 6, 7, 8]);

print(dot(a, b));                                // 70   (vectorized dot product)
print(sum(a));                                   // 10
print(normL2(new Float32Array([3, 4])));         // 5    (Euclidean norm)
print(distCos(a, b).toFixed(4));                 // 0.0311  cosine *distance*
print(argmax(a));                                // 3    (index of the max)

// BLAS-style: y = alpha·x + y  (in place, returns y)
const y = new Float32Array([1, 1, 1, 1]);
axpy(y, 10, a);
print(Array.from(y).join(","));                  // "11,21,31,41"

// Neural-net activations, elementwise, vectorized:
const logits = new Float32Array([2.0, 1.0, 0.1]);
softmax(logits);                                 // in place → a probability distribution
print(Array.from(logits).map(x => x.toFixed(3)).join(","));  // "0.647,0.252,0.102"

// GEMM: C = alpha·A·B + beta·C.  A is m×k, B is k×n, C is m×n.
const A = new Float32Array([1, 2, 3, 4]);        // 2×2
const I = new Float32Array([1, 0, 0, 1]);        // 2×2 identity
const C = new Float32Array(4);
gemm(C, A, I, 2, 2, 2, 1, 0);                    // C = A·I
print(Array.from(C).join(","));                  // "1,2,3,4"

// Double precision (f64) — because JS Number *is* f64:
print(f64Sum(new Float64Array([0.1, 0.2, 0.3])).toFixed(1));  // 0.6
```

The full surface, by family:

| Family | Functions |
|---|---|
| Reductions | `SUM` `MAX` `MIN` `ARG_MAX` `ARG_MIN` |
| Elementwise | `ADD` `SUB` `MUL` `DIV` `ABS` `fma` `scale` `axpy` `addScalar` `affine` |
| Norms & distances | `normL1` `normL2` · `distL1` `distL2` `distCos` `distCheb` |
| Activations | `relu` `relu6` `leakyRelu` `elu` `gelu` `silu` `sigmoid` `tanhFast` `softmax` `logSoftmax` |
| Transcendentals | `vexp` `vlog` `vsqrt` `vrsqrt` `vinv` |
| BLAS-2/3 | `gemv` `gemvT` `gemm` |
| Utilities | `clamp` `threshold` `topkIndices` |
| Double precision | `f64Sum` `f64Dot` `f64Max` `f64Min` `f64Scale` `f64Axpy` |
| Integer (`Int32Array`) | `i32Sum` (exact via int64) · `i32Min` `i32Max` `i32Dot` · `i32Add` `i32Mul` `i32Scale` (two's-complement wrap, like `Math.imul`) |
| Prefix scans | `CUM_SUM` `CUM_MAX` (over `Int32Array` or `Float32Array`) |

```js
import { i32Sum, i32Dot, cumsum, cummax } from "dyna:simd";

const a = new Int32Array([5, 2, 8, 1, 9]);
print(i32Sum(a));                 // 25       (widened, exact)
print(i32Dot(a, a));              // 175      (Σ aᵢ²)
const c = new Int32Array([1, 2, 3, 4]); cumsum(c);
print(Array.from(c).join(","));   // "1,3,6,10"   (inclusive prefix sum, in place)
const m = new Float32Array([3, 1, 4, 1, 5]); cummax(m);
print(Array.from(m).join(","));   // "3,3,4,4,5"  (running maximum)
```

Why this matters: vector maths driven from JavaScript usually means either a scalar loop the
compiler cannot vectorise (aliasing and early-exit rules defeat it) or a per-platform native
extension you have to build and ship yourself. DynaJS puts a verified, cross-ISA SIMD library *in
the runtime*. It is **dual-use**: the same kernels accelerate the engine internally (HTTP header
scanning uses the SIMD substring search) and are available to your code.

> A note on honesty: the SIMD kernels are differentially verified (each ISA against a scalar
> reference) on the hardware and emulation available. Certain AVX-512 paths are conservatively gated
> and fall back to the verified AVX2 path until they can be exercised on real AVX-512 silicon —
> DynaJS ships the proven path rather than an unverified one.

### `dyna:ml` — classic ML models

Fourteen algorithm families, native, in the binary: linear and logistic regression, k-nearest
neighbours, decision trees, random forests, gradient boosting, a kernel SVM, Gaussian naive Bayes,
k-means, DBScan, Gaussian mixtures, PCA, feature scalers, and the usual metrics — plus `Pipeline`,
`kFold`/`stratifiedKFold`, `trainTestSplit`, `gridSearch` and `randomSearch`.

```js
import { LinearRegression, KMeans } from "dyna:ml";

// Fit y = 2x + 1 and predict.
const lr = new LinearRegression();
try {
  const X = [], y = [];
  for (let x = 0; x < 20; x++) { X.push([x]); y.push(2 * x + 1); }
  lr.fit(X, y);
  print(lr.predict([[100], [0]]));          // ≈ [201, 1]  (recovered y = 2x + 1)
} finally {
  lr.close();
}

// Cluster 2-D points into k groups.
const km = new KMeans(2);
try {
  km.fit([[0, 0], [0.1, 0], [10, 10], [10.1, 10]]);
  print(km.predict([[0.05, 0], [10, 10]]));  // [0, 1]
  print("inertia:", km.inertia.toFixed(3));  // .inertia is a getter, not a method
} finally {
  km.close();
}
```

The pieces compose, which is the point of having them in one module — scale, reduce, classify:

```js
import { StandardScaler, PCA, RandomForestClassifier, accuracy } from "dyna:ml";

const sc = new StandardScaler(), pca = new PCA(2);
const clf = new RandomForestClassifier({ nEstimators: 50, seed: 1 });
try {
  const Z = pca.fitTransform(sc.fitTransform(X));   // features on different scales, then 2-D
  clf.fit(Z, y);
  print(accuracy(y, clf.predict(Z)));
} finally { sc.close(); pca.close(); clf.close(); }
```

Three things worth knowing before you use them (all detailed in the API reference):

- **Pass big data as a flat `Float64Array` with an explicit shape** — `fit(X, y, rows, cols)`. That
  form aliases the buffer with no copy and no per-cell JS crossing. Matrix results then come back as
  a flat `Float64Array` too; pass rows and you get rows back.
- **Labels are values, not indices.** A classifier fitted on `[7, -3.5]` predicts `7` and `-3.5`.
- **Compare model outputs with a tolerance, not `===`.** The inner loops are vectorised, so sums
  accumulate in several lanes rather than strictly in order; a coefficient can move by an ULP between
  builds or targets. Every stochastic algorithm is seeded, so *that* part is exactly reproducible.

A note on where the speed comes from, because it is not where you would guess. These loops were
measured four ways (`bench/ML_REPORT.md`): the version that calls into the shared `simd.*` kernel
table is **not** the fastest one. Writing each reduction as portable C with four independent
accumulators compiles to the same NEON instructions *and* keeps the loop fused into its caller, which
beat the kernel call on every case that does real work — `logreg.fit` on 500×128 went 184.6 → 128.1 ms
by *removing* the kernel calls. The tree code, meanwhile, makes no SIMD claim at all: its inner loop
is a data-dependent gather with a branch per element, and no lane packing survives that. Its speed is
algorithmic — incremental split statistics over a sorted sweep.

---

## 4.9 Data formats & algorithms

### `dyna:compress` — DEFLATE (gzip) and LZ4

```js
import { gzip, gunzip, lz4Frame, lz4Unframe } from "dyna:compress";

const original = "the quick brown fox ".repeat(100);        // 2000 bytes
const packed = gzip(original);                              // real DEFLATE
print(original.length, "→", packed.length, "bytes");        // 2000 → 56 bytes
print(gunzip(packed).length === original.length);           // true

const f = lz4Frame(original);          // the format `lz4 -d` reads
print(lz4Unframe(f, { asString: true }) === original);      // true
```

Two tiers over the same input:

| | Ratio | Speed | Use it when |
|---|---|---|---|
| **gzip** | better | slower | you need universal interop |
| **LZ4** | ~1.5× larger | ~3× faster to compress, ~6× faster to decompress | throughput matters more than bytes |

`level: 12` buys back most of gzip's ratio while staying far quicker to decode. The LZ4 frame is
checked against the system `lz4` binary in both directions.

For a stream of records, hoist a **`Compressor`**: it owns the match-finder scratch instead of
allocating and clearing it per call, which is 4× on small payloads.

```js
import { Compressor } from "dyna:compress";

const lz4 = new Compressor({ algo: "lz4" });
for (const rec of stream) send(lz4.compress(rec));
```

A `{ dict }` option seeds the match window with a prefix, which is what makes short templated
payloads compress at all — a 103-byte JSON-RPC frame goes to 24 bytes with a 100-byte dictionary,
where plain LZ4 and gzip both leave it larger than it started.

### `dyna:csv` — CSV files as a mini database

`dyna:csv` treats a CSV *file* as an editable dataset — create it, page through it, mutate rows and
columns — with RFC-4180 quoting, a SIMD-accelerated parse, and **atomic writes** (a crash mid-write
never corrupts the file).

The module exports a single class, **`CSVFile`**: construct it with a path, then call methods on it.
Each method takes one options object; row indices are 0-based over data rows.

```js
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";

const people = new CSVFile(new Path("/tmp/people.csv"));
people.create({ headers: ["Name", "Age", "City"],
                rows: [["Alice", "30", "NYC"]], overwrite: true });
people.addRow({ rows: [{ Name: "Bob", Age: "25", City: "LA" }] });
people.updateCell({ row: 0, column: "City", value: "Brooklyn" });
people.addColumn({ column: "Active", defaultValue: "yes" });

const page = people.read({ offset: 0, limit: 50, columns: ["Name", "City"] });
print(page.headers, page.rows, "of", page.totalRows);
// ["Name","City"] [["Alice","Brooklyn"],["Bob","LA"]] of 2
```

The eleven methods — `create`, `read`, `addRow`, `updateCell`, `removeRow`, `addColumn`,
`removeColumn`, `renameColumn`, `readColumnValuesRange`, `readRowRange`, `selectColumnRange` — each
take a single options object (easy to expose as an MCP tool). The path is bound once at construction,
so a single instance is reused across operations. Reads mmap the file; a 100k-row file creates and
reads back in a few milliseconds each. See the [API Reference](API.md#csv) for every option.

### `dyna:dataframe` — columnar analytics over typed arrays

Where `dyna:csv` edits a file, `DataFrame` computes over columns already in memory. Construct it
from an object of columns — a TypedArray per numeric column, a plain string array per label column —
and the numeric ones are **aliased zero-copy**, not copied.

```js
import { DataFrame } from "dyna:dataframe";

const city = ["NY", "SF", "NY", "LA", "SF", "NY"];        // label column
const amt  = new Float64Array([1, 2, 3, 4, 5, 6]);        // numeric column, aliased
const df   = new DataFrame({ city, amt });

print(df.rows, df.cols, df.columns.join(","));            // 6 2 city,amt
print(df.SUM("amt"), df.MEAN("amt"), df.MAX("amt"));      // 21 3.5 6

// A comparison builds a mask: one byte per row, reusable across reductions.
const big = df.GT("amt", 2);
print(Array.from(big).join(""));                          // 001111
print(df.SUM("amt", big), df.COUNT("amt", big));          // 18 4

// Group and sum in one native pass; keys come out in first-appearance order.
const g = df.GROUP_BY_SUM("city", "amt");
print(g.keys.join(","), Array.from(g.values).join(","));  // NY,SF,LA 10,7,4
```

| Group | Methods |
|---|---|
| Shape | `rows` `cols` `columns` |
| Reductions | `SUM` `MEAN` `MIN` `MAX` `COUNT` `PRODUCT` `VARIANCE` `STDDEV`, each `(column, mask?)` |
| | `DOT_PRODUCT(a, b, mask?)` |
| Bitwise reductions | `BITWISE_AND` `BITWISE_OR` `BITWISE_XOR`, each `(column, mask?)` — integer columns only |
| Mask builders | `GT` `GE` `LT` `LE` `EQ` `NE`, each `(column, value) → Uint8Array` |
| | `between(column, lo, hi)` · `isna(column)` · `notna(column)` |
| Mask consumers | `all(mask)` `any(mask) → boolean` · `bitmask(mask) → Uint32Array` |
| | `where(mask, a, b) → Float64Array`, where `a`/`b` is a column name or a number |
| Element-wise | `ABS` `ROUND` `FLOOR` `CEIL` `SQRT` `LOG` `EXP` `SIGN`, each `(column) → Float64Array` |
| | `clip(column, lo, hi)` · `fillna(column, value)` |
| Arithmetic | `ADD` `SUB` `MUL` `DIV` `POW`, each `(column, columnName \| number) → Float64Array` |
| | `rsub(column, k)` = `k − column` · `rdiv(column, k)` = `k / column` |
| Grouping | `GROUP_BY_SUM(keyColumn, valueColumn, mask?) → { keys, values }` |

Every signature, and the numeric contract that governs `NaN`, infinities, empty columns and
floating-point reassociation, is in the [API Reference](API.md#dataframe).

Two rules the module enforces rather than guesses:

- **Column type is read from the array's class, not its element width.** An `Int32Array` is summed as
  signed 32-bit integers, never as `Float32Array` — the width-based guess would silently return
  garbage. `Int16Array` is signed, `Uint8Array` unsigned, and `BigInt64Array` is rejected.
- **A group key column must be integers or strings.** A float key column throws, as does a negative
  integer key; the columns must all be the same length.

### Sorting & binary search — on `Array.prototype`

Sorting is not a module: the array methods already cover it, natively.

```js
[10, 9, 1, 2].sortBy();                 // [1, 2, 9, 10]   — NUMERIC, unlike bare .SORT()
[10, 9, 1, 2].SORT();                   // [1, 10, 2, 9]   — the lexicographic footgun
[3, "apple", 1].sortBy();               // [1, 3, "apple"] — mixed types preserved
[5, 3, 9, 1].toSorted((a, b) => b - a); // [9, 5, 3, 1]    — your own comparator

const sorted = [1, 3, 5, 7, 9];
sorted.sortedIndexOf(7);                // 3    — O(log n)
sorted.sortedIndexOf(4);                // -1
[9, 7, 5, 3].sortedIndexOf(5, (el, t) => t - el);   // 2  — comparator for other orderings
```

`sortedIndexOf` **trusts the array to be sorted by the ordering you search with** — that is what
makes it O(log n), and it cannot be checked more cheaply than the search itself. Use it with `sortBy`
(no comparator) or with the same comparator you sorted by.

For sorted *containers* rather than arrays, `dyna:structures`' `SortedSet`/`SortedMap` already answer
`has`/`FLOOR`/`CEIL`/`rangeQuery` by binary search internally.

---

## 4.10 Process and environment (`dyna:sys`)

`dyna:sys` holds the process surface — environment, arguments, working directory, identity — while
everything filesystem-shaped lives in [`dyna:file`](#dynafile--the-filesystem-module).

```js
import { platform, pid, cwd, hostName, homeDir, getEnv, env, args, memoryUsage } from "dyna:sys";

print(platform(), pid());                 // "darwin" 63467
print(getEnv("HOME") !== undefined);      // true
print(typeof env(), args().length);       // "object" 3

const m = memoryUsage();                  // the engine's own allocator counters
print(m.objCount, m.shapeCount, m.peakRss);
```

Full surface: `env`, `getEnv`, `setEnv`, `args`, `cwd`, `chDir`, `platform`, `pid`, `hostName`,
`homeDir`, `memoryUsage`.

`memoryUsage()` reports the engine's own accounting (`mallocCount`, `mallocSize`, `objCount`,
`shapeCount`, `peakRss`, …) — the numbers to trust when you are checking whether something leaks,
because they move with the JS heap rather than with the allocator's retention policy.

---

## 4.11 Semantic versioning (`dyna:semver`)

SemVer 2.0.0 parsing, comparison, and npm-style range satisfaction, as a native module.

```js
import { compare, satisfies, inc, maxSatisfying, coerce, sort } from "dyna:semver";

print(compare("1.0.0-alpha", "1.0.0"));  // -1   (a prerelease is lower than the release)
print(satisfies("1.2.9", "^1.2.3"));     // true
print(satisfies("0.3.0", "^0.2.3"));     // false (caret is special for 0.x: >=0.2.3 <0.3.0)
print(inc("1.2.3", "minor"));            // "1.3.0"
print(maxSatisfying(["1.0.0", "1.2.0", "1.9.0", "2.0.0"], "^1.2.0")); // "1.9.0"
print(coerce("v2.3.4-x"));               // "2.3.4"
print(sort(["2.0.0", "1.0.0-rc.1", "1.0.0"]).join(" < ")); // "1.0.0-rc.1 < 1.0.0 < 2.0.0"
```

The full npm range grammar is supported — exact, comparators (`>` `>=` `<` `<=` `=`), caret `^`
(including the 0.x rules), tilde `~`, hyphen `a - b`, x-ranges (`1.x`, `1.2.*`, `*`), AND (space) and
OR (`||`), plus npm's prerelease rule: a prerelease only satisfies a range whose comparator carries a
prerelease at the same `major.minor.patch`.

For a range you test many versions against, `new Range(spec)` compiles it once.

---

## 4.12 The module map at a glance

### JSON5 and canonical JSON

`dyna:encoding` carries two JSON dialects beyond the builtin.

```js
import { JSON5Parse, JSON5Stringify, StableStringify } from "dyna:encoding";

JSON5Parse("{a: 1, /* note */ b: [2,]}");   // { a: 1, b: [2] }
JSON5Parse("0x10");                          // 16
StableStringify({ b: 1, a: 2 });             // '{"a":2,"b":1}'
```

Valid JSON is valid JSON5 and parses identically. Nesting is capped at 256 and
the cap is checked **before** descending, so a nest bomb is a `RangeError`
rather than a stack overflow. `StableStringify` is RFC 8785 (JCS): keys sort by
UTF-16 code unit — *not* code point, which disagrees above the BMP — numbers
take the shortest round-trip form, and a non-finite number is rejected because
it has no canonical form.

### tar and zip archives

```js
import { TarPack, TarList, TarExtract, ZipPack, ZipList, ZipRead } from "dyna:compress";

const t = TarPack([{ name: "a.txt", data: "hello" }, { name: "d/" }]);
TarExtract(t)[0].data;                  // the bytes back

const z = ZipPack([{ name: "a.txt", data: "hello ".repeat(50) }]);
ZipRead(z, "a.txt");                    // decompressed, and CRC verified
```

**Every entry name is checked at the parse boundary**, so writing `entry.name`
into a directory is safe without knowing to check: `../`, an absolute path, a
backslash or a drive letter is refused on both the reading and the writing
side. Tar-slip and zip-slip are the same bug and both die there.
`{ allowUnsafeNames: true }` is the explicit opt-out.

The tar reader handles ustar, GNU long names and PAX records, which is what
`tar(1)` writes; the zip reader walks the central directory and verifies each
member's CRC and its local header.

### Binary value interchange

```js
import { MsgPackEncode, MsgPackDecode, CBOREncode, ValueHash,
         structuredClone } from "dyna:serialize";

MsgPackDecode(MsgPackEncode({ id: 7, tags: ["a", "b"] }));   // the same value
ValueHash({ a: 1, b: 2 }) === ValueHash({ b: 2, a: 1 });     // true -- canonical

const a = { name: "a" };
a.self = a;
structuredClone(a).self === structuredClone(a);              // false; each clone
                                                             // points at itself
```

MessagePack and RFC 8949 CBOR share one graph walker. A **cycle is refused** by
both — no wire format can express one — while `structuredClone` preserves it,
which is the difference between the two tools. Decoding checks every declared
length against the bytes that remain **before allocating**, so a four-byte
count cannot become a four-gigabyte allocation.

### Exact decimal arithmetic and money

```js
import { Decimal, Money } from "dyna:decimal";

new Decimal("0.1").add(new Decimal("0.2")).toString();   // "0.3"
0.1 + 0.2;                                               // 0.30000000000000004

const price = new Money(1999, "USD");                    // 1999 cents = $19.99
price.format();                                          // "$19.99"
price.allocate([1, 1, 1]).map((m) => m.amount());        // [667, 666, 666]
```

`Decimal` is arbitrary-precision arithmetic at IEEE 754 decimal128 by default
(34 significant digits, half-even). `add`, `sub` and `mul` are **exact**; only
`div` rounds, and it says by how much and how.

`Money` is a different type on purpose: an integer count of the smallest unit
with a currency tag. Adding USD to EUR throws, because that is a missing
exchange rate rather than arithmetic, and `allocate` splits an amount into
shares that sum back to it exactly.

### YAML

`dyna:yaml` reads the YAML 1.2 core schema — and refuses everything else by name.

```js
import { Parse, ParseAll, Stringify } from "dyna:yaml";

Parse("name: app\nports:\n  - 80\n  - 443\n");   // { name: "app", ports: [80, 443] }
Parse("country: no").country;                     // "no" -- a STRING, per 1.2
Stringify({ a: [1, 2] });                         // "a:\n  - 1\n  - 2\n"
```

`no`, `yes`, `on` and `off` are strings, which is the 1.2 rule and the end of
the Norway problem. Anchors (`&x`), aliases (`*x`), tags (`!x`), merge keys
(`<<`) and directives (`%YAML`) are **refused with a message naming them** —
they are YAML's amplification and object-injection surface, and a parser that
ignored them would return a document missing values with nothing to say so.

### Querying a document (JSONPath)

`dyna:encoding` also carries RFC 9535 JSONPath, compiled once and reused.

```js
import { JSONPath } from "dyna:encoding";

const cheap = new JSONPath("$.data.rows[?@.price < 10].name");
cheap.all(body);     // every matching name, in document order
cheap.paths(body);   // "$['data']['rows'][0]['name']", ...
```

Selectors are names, indices (negative counts from the end), slices with an
optional step, unions, `*`, the descendant segment `..`, and filters combining
comparisons with `&&`, `||` and `!`. **A query never runs code:** an accessor
property is skipped rather than invoked and inherited properties are invisible,
so an expression is safe to run over a document you did not write. The accepted
grammar is RFC 9535's, minus its function extensions; see the API reference.

### Running another program

```js
import { Exec, Which } from "dyna:sys";

const r = Exec("git", ["rev-parse", "HEAD"], { timeoutMs: 5000 });
r.code;      // 0
r.stdout;    // the commit id
Which("git") // "/usr/bin/git", or null
```

**There is no shell.** `Exec` takes an argv array, so `Exec("echo", ["$(id)"])`
prints the four characters `$(id)` — command injection is unrepresentable rather
than something to escape against. Both pipes are drained while the child runs,
so a program that writes more than a pipe buffer cannot deadlock, and a
`timeoutMs` sends `SIGTERM` to the child's process group and then `SIGKILL`.

A signalled child reports `signal: "SIGKILL"` and `code: null`, because being
killed and exiting nonzero are different things.

### HTML: parse, select, sanitize

```js
import { HTMLParse, Selector, Sanitizer } from "dyna:html";

const doc = HTMLParse('<div id="m"><p class="x">one</p><p>two</p></div>');
new Selector("div > p.x").all(doc).length;        // 1

const san = new Sanitizer({ allow: { p: [], b: [] } });
san.clean('<p onclick="x()">hi <script>alert(1)</script><b>b</b></p>');
// "<p>hi <b>b</b></p>"
```

`MarkdownToHTML(text)` renders CommonMark plus GFM tables through the same
escaper. **Raw HTML in the source is escaped** unless `{allowRawHTML: true}`,
and a `javascript:` link gets an empty href -- a renderer that emits its input
verbatim is an XSS hole with a nice API.

The tree is `dyna:xml`'s node shape. Void elements, implied end tags and
raw-text elements are handled, and an unmatched close tag is ignored rather
than unwinding the document.

**The sanitizer has no default policy** — a default is a policy nobody read. A
disallowed element loses its tag and keeps its text; a raw-text element's
content goes with it, because that content *is* script.

### XML

`dyna:xml` parses to a tree, streams as SAX, and serializes back.

```js
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";

const doc = XMLParse('<rss><channel><title>News</title></channel></rss>');
doc.children[0].children[0].children[0];        // "News"
XMLToObject(doc);                               // { rss: { channel: { title: "News" } } }

const p = new SAXParser({ onOpen: (n, a) => print(n) });
p.write("<a>"); p.write("<b/></a>"); p.end();   // a chunk may split any token
```

**No DTD is read, ever.** A `<!DOCTYPE` is skipped with a bracket counter and
never interpreted, so an external entity has nothing to declare it — XXE is
unrepresentable rather than disabled, and an entity bomb fails on its first
reference instead of on an expansion counter. The five predefined entities and
numeric character references are the whole entity language.

The tree keeps mixed content and element order (`children` holds strings and
elements); `XMLToObject` is the collapsed shape for config files and is
documented as lossy. Nesting is capped at 256 and attributes at 1024 per
element.

### Legacy charsets

`dyna:bytes` decodes and encodes 28 single-byte charsets.

```js
import { decode, encode, encodingExists } from "dyna:bytes";

decode(new Uint8Array([0x80]), "windows-1252");   // "\u20AC" — EURO SIGN
decode(new Uint8Array([0x80]), "iso-8859-1");     // "\u0080" — a C1 control
encode("\u4F60", "iso-8859-1");                   // Uint8Array [0x3F] — '?'
encodingExists("shift_jis");                      // false, honestly
```

The `0x80`–`0x9F` block is where windows-1252 and latin-1 disagree, and getting
it wrong is the most common charset bug there is. Tables are generated by
`tools/gen-charsets.py` and committed. **The multi-byte CJK families are not
built**: `encodingExists` reports `false` and `decode` refuses by name, so a
caller finds out without a throw mid-request.

## 4.13 Configuration formats (`dyna:config`)

Three grammars a project checks in. Each is a namespace, because a bare `parse`
across three grammars is ambiguous.

```js
import { INI, Env, FrontMatter } from "dyna:config";

INI.parse("[db]\nhost = localhost\nport = 5432\n");  // { db: { host: "localhost", port: "5432" } }
INI.parse("debug\nlist[] = a\nlist[] = b\n");         // { debug: true, list: ["a","b"] }
Env.parse("export PORT=8080 # http\n");               // { PORT: "8080" }
FrontMatter.split("---\ntitle: hi\n---\nbody\n");     // { data:"title: hi", body:"body\n", lang:"yaml" }
```

Every key is written with **define** semantics, so a file containing
`__proto__` — as a key *or* as a section name — produces an own property and
cannot reach `Object.prototype`. An unclosed front-matter fence is not front
matter: `data` is `null` and the whole input is the body, rather than silently
swallowing the document.

## 4.14 URLs and form bodies (`dyna:url`)

```js
import { URL, formEncode, formDecode } from "dyna:url";

new URL("../g", "http://a/b/c/d").href;   // "http://a/b/g"
new URL("http://a.com:80/").href;         // "http://a.com/" — a default port is dropped
formDecode("a=hello+world");              // { a: "hello world" }
formEncode({ a: "x&y" });                 // "a=x%26y"
```

Relative resolution follows RFC 3986 section 5.2 including its abnormal cases:
`..` past the root is dropped rather than allowed to escape. A malformed percent
escape stays **literal** rather than being dropped — losing bytes silently turns
a bad request into a different one.

## 4.15 Structured logging (`dyna:log`)

`Logger` is a compiled capability: level, name, base fields and timestamp mode
are parsed once, and the per-call path only appends.

```js
import { Logger, Debug } from "dyna:log";

const log = new Logger({ level: "info", name: "api", base: { pid: 7 } });
log.info({ userId: 42 }, "logged in");
log.error(new Error("boom"), "request failed");     // -> {type, message, stack}
const child = log.child({ requestId: "r1" });        // fields serialized once
if (log.enabled("debug")) log.debug({ plan: cost() }, "query plan");
```

A call below the level does **no work** — it does not serialize its fields. A
cycle logs as `"[Circular]"` and nesting past 8 as `"[deep]"`, so neither hangs
and the two stay distinguishable. **Every line is one `write(2)`**: nothing is
buffered, so a line survives a crash and concurrent writers cannot interleave
mid-line.

## 4.16 HTTP message codecs (`dyna:net`)

```js
import { ContentTypeParse, Negotiate, RangeParse, CookieParse, CookieSerialize,
         ETagMatch } from "dyna:net";

ContentTypeParse("text/html; charset=utf-8");
Negotiate("text/html;q=0.1, application/json", ["application/json", "text/html"]);
RangeParse("bytes=-500", 1000);              // [{ start: 500, end: 999 }]
CookieParse("sid=abc; theme=dark");          // { sid: "abc", theme: "dark" }
CookieSerialize("sid", "abc", { httpOnly: true, secure: true });
ETagMatch('W/"abc"', '"abc"');               // true — weak comparison
```

Parser and serializer share **one** `tchar` table, so a value the parser rejects
as a token is exactly one the serializer quotes. `CookieSerialize` **refuses** a
delimiter-carrying value rather than escaping it, and `RangeParse` distinguishes
satisfiable ranges from `"unsatisfiable"` (a 416) from `null` (no byte range).

## 4.17 Compact identifiers (`dyna:uuid`)

```js
import { NanoID, ULID, ULIDTime } from "dyna:uuid";

NanoID();                       // 21 URL-safe characters, 126 bits
ULID();                         // 26 Crockford base32, sortable by time
ULIDTime(ULID(1469918176385));  // 1469918176385
```

Both map random bytes to symbols by **rejection sampling**, not modulo — modulo
over-produces the first `256 % n` symbols whenever the alphabet is not a power
of two. A ULID sorts lexicographically by its timestamp, which is the reason to
pick it over `v4()`.

## 4.18 Command-line programs (`dyna:cli`)

`Command` is a compiled capability: the spec is read once, `parse()` takes the
argv.

```js
import { Command, StyleText, ColorDepth } from "dyna:cli";

const cmd = new Command("mytool").describe("does a thing")
  .option("-v, --verbose", "chatty", { type: "boolean" })
  .option("-o, --out <path>", "output", { type: "string", required: true })
  .option("-n, --count <n>", "how many", { type: "number", default: 1 });

cmd.parse(["-v", "--out=f.txt", "-n5", "--", "--not-a-flag"]);
// { options: { verbose: true, out: "f.txt", count: 5 },
//   arguments: ["--not-a-flag"], command: null }

if (ColorDepth() > 0) print(StyleText(["red", "bold"], "error"));
```

`--long`, `--long=v`, `-o v`, `-ov`, `-abc` bundling, `--no-flag` negation and
`--` are all accepted. **Types are declared, never guessed** — `--x=3` is the
string `"3"` unless the option says `type: "number"`, because a guessed type is
how a CLI surprises the script calling it. An unknown option is an error unless
`allowUnknown()` is asked for.

`StyleText` matches Node's `util.styleText` signature rather than chalk's
chaining proxy, and refuses an unknown style instead of dropping it. Styled text
carries no display width, so it composes with `displayWidth` and `wrapAnsi`.

## 4.19 Format validators (`dyna:validate`)

```js
import { IsEmail, IsIBAN, IsCreditCard } from "dyna:validate";

IsEmail("first.last@example.co.uk");   // true
IsIBAN("GB82 WEST 1234 5698 7654 32"); // true -- mod-97 checked
IsCreditCard("4242 4242 4242 4242");   // true -- Luhn checked
```

`IsIBAN` and `IsCreditCard` verify the **check digit**, so a single mistyped
character or a transposed pair is rejected rather than merely well-shaped.
`IsEmail` matches what mail systems accept rather than RFC 5322, which admits
forms no mail system round-trips.

`IsUUID`, `IsIP`, `IsURL`, `IsBase64` and `IsHexadecimal` are **deliberately
absent** — `dyna:uuid`, `dyna:net`, `dyna:url` and `dyna:encoding` already own
them, and a second spelling is a second thing to keep correct.

| Module | One-line why |
|---|---|
| `dyna:matcher` | compiled single- and multi-pattern search at GiB/s, plus edit distance and bigram similarity |
| `dyna:bytes` | ergonomic byte ops, fixed-width accessors, SIMD UTF-8/UTF-16 kernels |
| `dyna:encoding` | hex, base64/base64url, base32, Ascii85, LEB128 var-ints |
| `dyna:hash` | SHA-1/2 family, MD5, CRC-32/32C, xxHash, streaming `Hasher` |
| `dyna:crypto` | HMAC, HKDF, PBKDF2, constant-time compare, OS entropy |
| `dyna:uuid` | v4 **+ v7** + v3/v5 (RFC 9562) — JavaScript has none |
| `dyna:random` | reproducible seeded streams, which `Math.random` cannot give you |
| `dyna:mathx` | gamma, erf, Bessel, gcd/lcm/factorial, primality, float introspection |
| `dyna:structures` | heap, trie, LRU, bit set, sketches, and a graph whose algorithms are native methods — the containers JS lacks |
| `dyna:file` | buffered I/O with per-OS fast paths, plus the whole filesystem surface |
| `dyna:uring` | high-queue-depth async bulk reads; Linux with `CONFIG_IO_URING=y` only |
| `dyna:sys` | env, args, cwd, platform, pid, engine memory counters |
| `dyna:net` | `App` server (rpc/static/upload/ws) + client on one reactor; `TCPServer`/`UDPSocket`; `DNSResolver`/`DNSServer`; `Redis`, `PostgreSQL` and `SQLite` clients; plus IP/CIDR parsing, comparison and classification |
| `dyna:time` | typed durations, monotonic clock, RFC 3339 |
| `dyna:semver` | SemVer 2.0.0 parse/compare + the npm range grammar |
| `dyna:simd` | multi-ISA vector math, in the runtime, no addon |
| `dyna:ml` | 14 native model families and their metrics |
| `dyna:compress` | real DEFLATE gzip and LZ4, with dictionaries |
| `dyna:csv` | CSV file CRUD: RFC 4180, mmap reads, atomic writes |
| `dyna:dataframe` | zero-copy columnar reductions, masks and group-by |
| `dyna:config` | INI, `.env` and front-matter parsing |
| `dyna:url` | RFC 3986 URLs with relative resolution, plus the form-urlencoded codec |
| `dyna:log` | leveled structured logging and the `debug()` shape |
| `dyna:cli` | argv parsing, terminal styling, TTY queries |
| `dyna:validate` | e-mail, IBAN and card-number validation with real check digits |
| `dyna:xml` | XML: a document tree, streaming SAX, and a serializer |
| `dyna:html` | HTML parsing, CSS selectors, and an allow-list sanitizer |
| `dyna:yaml` | YAML 1.2 core schema, with anchors and tags refused by name |
| `dyna:decimal` | exact decimal arithmetic and an integral money type |
| `dyna:serialize` | MessagePack, CBOR, value hashing, structured clone |

---

*For complete signatures and every method of every module, see the [API Reference](API.md).*

---

<p align="center">
<b>Next:</b> <a href="API.md">★ The API Reference →</a><br>
<sub>every module, every signature, every throwing condition · <a href="README.md">← Back to the guide index</a></sub>
</p>
