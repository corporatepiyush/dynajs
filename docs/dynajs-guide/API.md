<p align="right"><i><a href="04-standard-library.md">← 4 · Standard Library</a> · The API Reference · <a href="README.md">Guide index</a></i></p>

# DynaJS API Reference

Complete, per-function documentation for every `dyna:*` standard-library module — the page to keep
open in a tab while you code. If you'd rather learn by example first,
[Chapter 4](04-standard-library.md) is the tutorial-style tour; this is the
look-up-the-exact-signature companion.

## Modules at a glance

<table>
<tr><th align="left">Text &amp; bytes</th><th align="left">Crypto &amp; identity</th><th align="left">Numbers</th></tr>
<tr valign="top"><td>

- [`matcher`](#matcher) — compiled search patterns, SIMD and Aho-Corasick
- [`bytes`](#bytes) — byte buffers, fixed-width accessors
- [`encoding`](#encoding) — hex, base64, base32, Ascii85, var-ints
- [`csv`](#csv) — CSV files as a mini database

</td><td>

- [`hash`](#hash) — SHA-1/2, MD5, CRC-32/32C, xxHash, `Hasher`
- [`crypto`](#crypto) — HMAC, PBKDF2, HKDF, constant-time compare
- [`uuid`](#uuid) — RFC 9562 v4 / v7 / v3 / v5
- [`random`](#random) — seedable PRNG

</td><td>

- [`mathx`](#mathx) — gamma, erf, gcd, primes, MATLAB parity, `bits`
- [`simd`](#simd) — multi‑ISA vector math
- [`ml`](#ml) — 14 model families

</td></tr>
<tr><th align="left">I/O &amp; system</th><th align="left">Networking</th><th align="left">Data</th></tr>
<tr valign="top"><td>

- [`file`](#file) — buffered reader/writer, fs
- [`uring`](#uring) — Linux io_uring bulk reads
- [`sys`](#sys) — env, args, cwd, platform
- [`time`](#time) — durations, monotonic clock

</td><td>

- [`net`](#net) — `App` server, HTTP client, TCP/UDP/IPC, DNS,
  Redis / PostgreSQL / SQLite, IP &amp; CIDR
- [`semver`](#semver) — SemVer 2.0.0 + npm ranges

</td><td>

- [`structures`](#structures) — heap, trie, LRU, graph, …
- [`dataframe`](#dataframe) — columnar tables, masks, group-by
- [`compress`](#compress) — gzip / gunzip

</td></tr>
</table>

**No import required:** [**Built-in prototype extensions**](#built-in-prototype-extensions)
— the always-on native methods installed on `Array`/`String`/`Number`/`Object`/`Function`/`Date`,
plus the `Lens` type. They are part of every build, not `dyna:*` modules, and the ⚡ marker flags the
ones backed by SIMD kernels.

## Conventions used in this reference

**Importing.** Every module is imported by its `dyna:` specifier, either statically in a module
(`import { fn } from "dyna:hash"`) or dynamically anywhere (`const m = await import("dyna:hash")`).
The native standard library is present only in a binary built with `CONFIG_NATIVE_MODULES=y`.

**Type notation.** This reference uses TypeScript-style types for clarity (DynaJS is plain
JavaScript; the types are documentation, not enforced syntax):

| Notation | Meaning |
|---|---|
| `number` | a JavaScript IEEE‑754 double. |
| `BigInt` | an arbitrary-precision integer literal, e.g. `42n`. Used wherever a value may exceed 2⁵³ or must be bit-exact. |
| `bytes` | a **bytes-like** value: a `Uint8Array`, an `ArrayBuffer`, or a `string`. A string is interpreted as its UTF‑8 bytes (Latin‑1 where noted). A `Uint8Array` view is read through its backing buffer at the correct offset. |
| `T[]` | a JavaScript `Array` of `T`. |
| `A \| B` | either type. `T?` after a parameter means it is optional. |

**Error model.** On invalid input a function throws a JavaScript `Error` (or a subclass:
`TypeError` for wrong argument types, `RangeError` for out-of-range values/offsets, `SyntaxError`
for malformed text input). The specific subclass and condition are listed per function under
**Throws**. Functions documented as "no throw" return a sentinel (`false`, `null`, `undefined`,
`-1`) instead.

**Return values are copies.** Every value a module returns is a fresh JavaScript value; it never
aliases native memory, so a returned `Uint8Array`/string/object can be held indefinitely.

**Resource classes.** Some modules export classes that own native memory (noted per class). They
must be released explicitly with `.close()`, or `[Symbol.dispose]()`, or a `DisposableStack`.
Release is deterministic (memory returns immediately). After release, every method throws
`TypeError: use of a closed native resource`; the `.closed` getter reports the state. The class
finalizer is a safety net for a leaked object, not the intended release path.

**Argument coercion order.** Every method coerces its JavaScript arguments to native values
*before* it touches its native resource, so passing an adversarial argument (a `valueOf` that
closes the object) fails cleanly rather than corrupting memory.

---

# Compiled capabilities

Some classes in this library take **configuration** in their constructor and are then reused across
unbounded inputs, the way `new RegExp(pattern)` is. They are not value handles: they hold no
particular datum, and one instance serves the whole program.

| Class | Module | Constructed with | Reused for |
|---|---|---|---|
| `Matcher` | `dyna:matcher` | a pattern | many texts |
| `MultiMatcher` | `dyna:matcher` | a set of patterns | many texts |
| `Hasher` | `dyna:hash` | an algorithm name | many messages |
| `Hmac` | `dyna:crypto` | an algorithm and a key | many messages |
| `Range` | `dyna:semver` | a version range | many versions |
| `Prefix` | `dyna:net` | a CIDR | many addresses |
| `Format` | `dyna:time` | a layout | many timestamps |
| `Compressor` | `dyna:compress` | `{algo, level, dict}` | many payloads |
| `Dictionary` | `dyna:compress` | a list of phrases | many records |

**Build one, hoist it out of the loop, reuse it.** That is the whole point, and it is the only way
any of them pays.

## When a capability pays

Every one of these replaces a free function you could have called directly, and at **one use each of
them is slower** — an object allocation plus a method call cannot beat a single direct call. What
they remove is a *fixed* configuration cost, so the rule is:

> **A capability pays when the configuration it parses once is expensive relative to the operation
> that follows, and you then perform that operation many times.**

`Range` and `Prefix` are the clearest case: parsing a version range or a CIDR dwarfs the comparison
that follows, so a handful of uses is enough. `Compressor` hoists a 64 KiB match-finder table, which
is most of the work on a short record and almost none of it on a large page — so its value depends on
the size of what you feed it, not only on how often. `Format` skips re-scanning the layout.

Two of them are not about speed at all, and hoisting them to go faster will disappoint you:

- **`Hasher`** exists so you can `update()` a stream you do not hold in memory. It replaces one call
  with three, and it has no expensive configuration to win that back.
- **`Matcher`** exists so a pattern can be bound to an object. `String.prototype.indexOf` runs the
  same SIMD kernel, so it is neither faster nor slower — use whichever reads better.

If you hash one buffer or test one version, use the free function.

## Reentrancy

Most of these are safe to use from anywhere, including concurrently, because their compiled state is
read-only once constructed: `Matcher`, `Range`, `Prefix`, `Format`.

Those that hold **mutable scratch across calls** are not, and they say so:

- `Hasher` accumulates into a state block. A reentrant `update()` from inside an argument's own
  `toString()` is well defined — the inner call completes first — because the native absorb runs no
  JavaScript. Verified to eight levels of nesting.
- `serialize()` allocates its own buffer per call, so a codec that re-enters it is safe
  `TypeError` rather than corrupting it. Use a second instance if you need to nest.

# matcher

`import { Matcher, MultiMatcher } from "dyna:matcher";`

Compiled search patterns. **Offsets are UTF-16 code units**, like `String.prototype`; the scan runs
over UTF-8 internally and translates on the way out, which is free for ASCII text (the byte and
code-unit lengths are equal iff every character is below U+0080, so the check is O(1)).

### `class Matcher`

One pattern, bound for ergonomics.

| Member | Signature | Description |
|---|---|---|
| `new Matcher(pattern, opts?)` | `(string, {algo?})` | `algo` is `"kmp"` (default) or `"bmh"`; it is validated and reported back, and does not change the answer. |
| `.firstIn(text)` | `(string) → number` | First offset, or `-1`. |
| `.allIn(text)` | `(string) → number[]` | Every offset, ascending, **counting overlaps**: `new Matcher("aa").allIn("aaaa")` is `[0,1,2]`. |
| `.countIn(text)` | `(string) → number` | |
| `.test(text)` | `(string) → boolean` | |
| `.replaceAllIn(text, repl)` | `(string, string) → string` | **Non-overlapping**, left to right — overlapping matches cannot all be replaced. |
| `.length` / `.algo` | getters | |

An empty pattern is at offset 0 and nowhere else: `firstIn` is 0, `test` is true, and `countIn` and
`allIn` report nothing rather than a zero-width match at every position.

**Matcher carries no precomputed table**, and that is deliberate: the search underneath is a SIMD
kernel, and a table cannot beat it, so building one would cost construction and buy nothing.

`String.prototype.indexOf` runs that same kernel, so **`Matcher` is neither faster nor slower — it
is free.** Use it when binding a pattern to an object reads better than repeating the string; use
`indexOf` when it does not. A pure-ASCII pattern is searched **directly in the engine's own string
bytes**, because no Latin-1 byte at or above 0x80 can equal an ASCII pattern byte.

### `class MultiMatcher`

Many patterns, compiled into an **Aho-Corasick automaton** and found in a **single pass**.

| Member | Signature | Description |
|---|---|---|
| `new MultiMatcher(patterns)` | `(string[])` | 1–65536 patterns, none empty. |
| `.firstIn(text)` | `(string) → {index, at} \| null` | `index` is the position in the pattern array. |
| `.allIn(text)` | `(string) → {index, at}[]` | Every hit of every pattern, in the order they end. |
| `.countIn(text)` | `(string) → number` | |
| `.test(text)` | `(string) → boolean` | |
| `.size` / `.states` | getters | Pattern count and automaton size. |

**Its cost is flat in the number of patterns, and that is the whole proposition.** The automaton
makes one pass whatever N is; N separate `indexOf` calls make N passes. But `indexOf` is a SIMD
kernel and the automaton is a scalar state machine, so one scalar pass is worth roughly thirty
vectorised ones — which is why the automaton only wins once N is large.

**Use it above roughly 36 patterns.** Below that, loop over `indexOf`.

It is also the only form that reports what a loop of independent searches gets right only by
accident: with `["he","she","his","hers"]` over `"ushers"` it finds `she@1`, `he@2` *and* `hers@2` —
the `he@2` hit exists because `she` fails back into `he`, which is what the suffix links are for.

```js
const log = ["ok", "ERROR disk full", "panic: nil deref"];
const mm = new MultiMatcher(["ERROR", "FATAL", "panic:"]);
for (const line of log) if (mm.test(line)) print(line);   // the last two
```

### Approximate matching

`import { Levenshtein, DiceCoefficient } from "dyna:matcher";`

Free functions, not compiled capabilities: neither has configuration worth
parsing once. They measure in **code points**, so an astral character counts 1,
not the 2 UTF-16 units it occupies.

| Function | Signature | Description |
|---|---|---|
| `Levenshtein(a, b, opts?)` | `(string, string, {max?}) → number` | Exact edit distance (insert/delete/substitute, each cost 1). |
| `DiceCoefficient(a, b)` | `(string, string) → number` | Sørensen–Dice over character bigrams, in `[0, 1]`. |
| `DiffLines(a, b)` | `(string, string) → hunk[]` | Minimal line diff. A line carries its own trailing newline. |
| `DiffWords(a, b)` | `(string, string) → hunk[]` | Word diff; whitespace runs are their own tokens. |
| `DiffChars(a, b)` | `(string, string) → hunk[]` | Code-point diff; an astral character is never split. |

**`max` is a bound, not a truncation.** The answer is exact while it is `<= max`
and is `max + 1` once it exceeds it, so `d <= max` is always a correct "within
max" test. Supplying it lets the search band the matrix, which is what makes a
bounded query on long strings cheap.

Below 64 code points on the shorter side the distance runs Myers' bit-parallel
algorithm — one machine word of state per element of the longer side. Past that
it is a two-row DP, banded by `max` when given. Either side is capped at 16 MiB.

`DiceCoefficient` reproduces the `string-similarity` package including its two
surprises, which are contract and not accident: **ASCII whitespace is stripped
before comparing**, and a side shorter than two characters scores 0 unless the
two are exactly equal. The bigram intersection is a multiset, so `"aa"` against
`"aaaa"` scores 0.5, not 1.

A **hunk** is `{ op: -1 | 0 | 1, text }` — delete, common, insert — and hunks
come back in source order with no two adjacent hunks sharing an op. Two
properties always hold, and they are what the test suite checks:

- concatenating every hunk with `op !== 1` reproduces `a`
- concatenating every hunk with `op !== -1` reproduces `b`

The script is **minimal**: it keeps exactly `LCS(a, b)` tokens common. The
algorithm is Myers O(ND) with the linear-space refinement over interned tokens,
run on an explicit stack rather than by recursion, with the shared prefix and
suffix trimmed first. Identical input returns one common hunk without
tokenising at all (measured 3.56x on an unchanged 2000-line file).

```js
Levenshtein("kitten", "sitting");             // 3
Levenshtein("kitten", "sitting", { max: 2 }); // 3 -> exceeds max, so max + 1
DiceCoefficient("healed", "sealed");          // 0.8
DiceCoefficient("french", "quebec");          // 0

for (const h of DiffWords("the quick brown fox", "the quick red fox"))
    print((h.op === 0 ? "  " : h.op < 0 ? "- " : "+ ") + h.text);
// "  the quick " / "- brown" / "+ red" / "   fox"
```

---

### HTTP message codecs

`import { ContentTypeParse, ContentTypeFormat, Negotiate, NegotiateToken, RangeParse, CookieParse, CookieSerialize, ETagMatch } from "dyna:net";`

| Function | Signature | Description |
|---|---|---|
| `ContentTypeParse(header)` | `(string) → {type, subtype, parameters} \| null` | Malformed input is `null`, never a plausible wrong object. |
| `ContentTypeFormat(obj)` | `(object) → string` | Quotes any value that is not a bare token. |
| `Negotiate(header, offers)` | `(string, string[]) → string \| null` | Media types. Exact beats `type/*` beats `*/*`, then q, then offer order. |
| `NegotiateToken(header, offers)` | `(string, string[]) → string \| null` | Language / encoding / charset. `en` matches `en-GB` by prefix. |
| `RangeParse(header, size)` | `(string, number) → [{start,end}] \| "unsatisfiable" \| null` | Ranges are **inclusive**. |
| `CookieParse(header)` | `(string) → object` | The `Cookie` request header. |
| `CookieSerialize(name, value, opts?)` | `(string, string, object) → string` | A `Set-Cookie` value. |
| `ETagMatch(header, etag)` | `(string, string) → boolean` | `If-None-Match`; weak comparison, `*` matches anything. |

The parser and the serializer share **one** `tchar` table, so a value the parser
would reject as a token is one the serializer quotes — a serializer that quotes
a different set than the parser accepts is a header-smuggling bug, not a
cosmetic difference. A comma or semicolon inside a quoted parameter does not
split a list.

`RangeParse` distinguishes three outcomes on purpose: an array of satisfiable
ranges, the string `"unsatisfiable"` (a 416), and `null` for "there was no byte
range to honour". An end past the resource is clamped rather than rejected.

`CookieSerialize` **refuses** a name that is not a token and a value carrying a
delimiter, rather than escaping them — escaping would let a caller append an
attribute or a second cookie. `CookieParse` writes names with define semantics,
so a cookie called `__proto__` arriving from the network cannot reach the
prototype.

```js
ContentTypeParse("text/html; charset=utf-8");   // { type:"text", subtype:"html", parameters:{charset:"utf-8"} }
ContentTypeFormat({ type:"m", subtype:"f", parameters:{ b:"a b" } });  // 'm/f; b="a b"'
Negotiate("text/html;q=0.1, application/json", ["application/json","text/html"]);
RangeParse("bytes=-500", 1000);                 // [{ start: 500, end: 999 }]
CookieSerialize("sid", "x", { httpOnly: true, secure: true });
ETagMatch('W/"abc"', '"abc"');                  // true -- weak comparison
```

# bytes

### Legacy charsets

`import { decode, encode, encodingExists, encodings } from "dyna:bytes";`

| Function | Signature | Description |
|---|---|---|
| `decode(bytes, label)` | `(Uint8Array, string) → string` | Decode a single-byte charset. An undefined byte becomes `U+FFFD`. |
| `encode(text, label)` | `(string, string) → Uint8Array` | Encode. A code point the charset cannot express becomes `?`. |
| `encodingExists(label)` | `(string) → boolean` | Whether **this build** can handle the label. |
| `encodings()` | `() → string[]` | Every label this build supports. |

Labels are matched ASCII-case-insensitively with surrounding space ignored, and
the common aliases (`latin1`, `cp1252`, `ascii`, …) resolve. Tables are
generated by `tools/gen-charsets.py` and committed, so the build never reaches
the network.

**The multi-byte CJK families are not built.** `shift_jis`, `euc-jp`, `gbk`,
`gb18030`, `big5` and `euc-kr` are absent, `encodingExists` returns `false` for
them, and `decode` refuses by name — a caller can find that out without a throw
in the middle of a request. Single-byte charsets and UTF-8 are always present.

```js
decode(new Uint8Array([0x80]), "windows-1252");   // "\u20AC" — EURO SIGN
decode(new Uint8Array([0x80]), "iso-8859-1");     // "\u0080" — a C1 control
encode("\u20AC", "windows-1252");                 // Uint8Array [0x80]
encode("\u4F60", "iso-8859-1");                   // Uint8Array [0x3F] — '?'
encodingExists("shift_jis");                      // false, honestly
```


`import { bytesOf, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, fill, /* read*, write* */ toUtf8, fromUtf8 } from "dyna:bytes";`

Byte-buffer operations, plus fixed-width integer/float accessors in both byte orders.
`indexOf`/`contains`/`count` use the SIMD substring engine.

**This module's buffer argument is stricter than the `bytes` shorthand above.** It takes a
**byte-addressed view** — `Uint8Array`, `Int8Array`, `Uint8ClampedArray`, or a `DataView` — or a plain
`ArrayBuffer` (the whole buffer). A view is always read through its own `byteOffset`/`byteLength`
window, never the whole backing buffer. A **string is not accepted** (use `fromUtf8` first), and
neither is a **wider view** such as `Uint16Array` or `Float64Array`: with a multi-byte element type
an `offset` argument would be ambiguous between an element index and a byte offset, so the module
throws `TypeError` instead of guessing. Pass such a view through `bytesOf` to state the
reinterpretation explicitly.


## Bytes

`import { Bytes } from "dyna:bytes";`

A **value handle** over one contiguous buffer. It owns a `Uint8Array` and caches the two
predicates downstream code keeps recomputing.

| Member | Signature | Description |
|---|---|---|
| `new Bytes(data)` | `(string \| bytes) → Bytes` | A string is encoded as UTF-8. A buffer is **copied**. |
| `.length` | `number` (getter) | |
| `.isAscii` | `boolean` (getter) | Every byte below `0x80`. |
| `.isValidUtf8` | `boolean` (getter) | Well-formed UTF-8, rejecting overlong, surrogate and out-of-range forms. |
| `.array` | `Uint8Array` (getter) | The owned view. |
| `.slice(start?, end?)` | `(number?, number?) → Bytes` | A **view**, sharing the buffer. Bounds clamp; negatives count from the end. |
| `.compare(other)` · `.equals(other)` | `(bytes) → number \| boolean` | |
| `.indexOf(needle, from?)` · `.lastIndexOf` · `.includes` · `.count` | | `needle` is a byte value or a buffer. |
| `.indexOfAny(set)` | `(bytes) → number` | The first position holding any byte of `set`. |
| `.fill(value, start?, end?)` | | Writes in place. |
| `.toUtf8()` · `.toString()` | `() → string` | |
| `.read*(offset)` · `.write*(offset, value)` | | All 36 fixed-width accessors — every width, signed and unsigned, LE and BE, including the BigInt 64-bit pairs. |
| `Bytes.alloc(n)` | `(number) → Bytes` | Zeroed. |
| `Bytes.concat(list)` | `(bytes[]) → Bytes` | One allocation, sized in a first pass. |
| `Bytes.isBytes(v)` | `(any) → boolean` | |

**The constructor copies and `.slice()` views, deliberately.** If the constructor aliased, a later
write through the original `Uint8Array` would leave `isAscii` and `isValidUtf8` describing bytes
that are no longer there — cached flags that have quietly become lies. A slice is the opposite case
and shares on purpose: writing through it *is* visible in the owner, which is what makes it a view.

A slice holds a **strong** reference to its owner's buffer, so a small view keeps a large buffer
alive. That is the correct trade — a view into freed memory would be far worse — but it means a
long-lived slice of a short-lived buffer retains all of it.

**Measured:** using a handle is free — `compare` 1.04×, `slice` 0.95× against the same operations on
a raw `Uint8Array`, with zero bytes retained per call. Constructing one is **~1.7×** a raw copy of
the same buffer, which is the cost of the extra object plus the scan.

`.slice()` is O(1) in the slice length. It gets there by inheriting: a slice of an ASCII buffer is
ASCII, and an ASCII buffer is valid UTF-8, so no scan is needed at all. When the owner is not ASCII
the slice does scan, because a slice can cut a multi-byte sequence in half and `isValidUtf8` cannot
be inherited.

`Bytes` is accepted anywhere this module takes a buffer, so the free functions above work on a
handle without duplication.

## Text

`import { Text } from "dyna:bytes";`

An interpretation of a string, paired with `Bytes` the way `Path` is paired with `File`: one is
storage, the other is a reading of it.

| Member | Signature | Description |
|---|---|---|
| `new Text(s)` | `(any) → Text` | Coerced to a string once. |
| `.value` · `.toString()` · `.toJSON()` | `string` | |
| `.isWide` | `boolean` (getter) | Any code unit above `U+00FF`. |
| `.isValidUtf8()` · `.isValidUtf16()` | `() → boolean` | |
| `.countUtf8()` · `.countUtf16()` | `() → number` | Code points, not bytes. |
| `.toUtf8()` · `.latin1ToUtf8()` · `.utf8ToLatin1()` | | |
| `.toBytes()` | `() → Bytes` | UTF-8. |

`isWide` is the engine's own summary bit: it decides whether a byte-wise kernel can run at all, so
a Latin-1 string — `"héllo"`, where `é` is `U+00E9` — is **not** wide.

### `bytesOf(view)`

| Parameter | Type | Description |
|---|---|---|
| `view` | `ArrayBuffer` \| any `TypedArray` \| `DataView` | The value to reinterpret as bytes. |

**Returns** `Uint8Array` — **aliasing** exactly the bytes `view` spans (`byteOffset`, `byteLength`).

The one function in the library that does **not** copy: writes through the result are visible through
the original view and vice versa. It replaces the hand-rolled
`new Uint8Array(v.buffer, v.byteOffset, v.byteLength)`, whose usual bug is dropping the
offset/length pair and silently covering the whole buffer instead of the view's own range. **Throws**
`TypeError` if `view` is not a buffer or view, or if it is detached/out of bounds.

<!-- check:skip -->
```js
const f = new Float64Array([1.5, -2.5]);
readDoubleLE(f, 0);                  // TypeError — Float64Array is not a byte view
const b = bytesOf(f);                // Uint8Array(16), aliasing f
readDoubleLE(b, 0);                  // 1.5
writeDoubleLE(b, 0, 42.25);
f[0];                                // 42.25 — the alias writes through
```

### `compare(a, b)` · `equal(a, b)`

| Parameter | Type | Description |
|---|---|---|
| `a`, `b` | `bytes` | The buffers to compare. |

**Returns** `compare`: `-1 | 0 | 1` (lexicographic by byte value, shorter-is-less on a common prefix). `equal`: `boolean`.

### `indexOf(buf, sub)` · `lastIndexOf(buf, sub)` · `contains(buf, sub)` · `count(buf, sub)`

Substring search within a byte buffer.

| Parameter | Type | Description |
|---|---|---|
| `buf` | `bytes` | The haystack. |
| `sub` | `bytes` | The needle. |

**Returns** `indexOf`/`lastIndexOf`: `number` (first/last start offset, or `-1`; `indexOf` is SIMD). `contains`: `boolean`. `count`: `number` (non-overlapping occurrences).

### `concat(buffers)`

| Parameter | Type | Description |
|---|---|---|
| `buffers` | `bytes[]` | An array of buffers to join, in order. |

**Returns** `Uint8Array` — a new buffer containing every input byte concatenated.

### `copy(dst, src, dstStart?, srcStart?, srcEnd?)`

Copies a range of `src` into `dst`.

| Parameter | Type | Description |
|---|---|---|
| `dst` | `Uint8Array` | The destination (written in place). |
| `src` | `bytes` | The source. |
| `dstStart` | `number?` | Destination offset. Default `0`. |
| `srcStart` | `number?` | Source start. Default `0`. |
| `srcEnd` | `number?` | Source end (exclusive). Default `src.length`. |

**Returns** `number` — the number of bytes copied (bounded by the space in `dst`).

```js
const d = new Uint8Array(6);
copy(d, new Uint8Array([1,2,3,4]), 1, 0, 3);   // returns 3; d = [0,1,2,3,0,0]
```

### `fill(buf, value, start?, end?)`

Fills a range of `buf` with a byte.

| Parameter | Type | Description |
|---|---|---|
| `buf` | `Uint8Array` | The buffer (written in place). |
| `value` | `number` | The byte value (low 8 bits). |
| `start` | `number?` | Start offset. Default `0`. |
| `end` | `number?` | End offset (exclusive). Default `buf.length`. |

**Returns** `Uint8Array` — `buf`.

### Fixed-width accessors — `read*` / `write*`

Read or write an integer/float at a byte offset in a specified width and byte order. The 8‑bit
forms have no byte order.

**Read** `readTYPE(buf, offset)` → the value. **Write** `writeTYPE(buf, offset, value)` → `number`
(the offset just past the written bytes).

| Family | `TYPE` values | Value type |
|---|---|---|
| 8‑bit | `Uint8`, `Int8` | `number` |
| 16‑bit | `Uint16LE/BE`, `Int16LE/BE` | `number` |
| 32‑bit | `Uint32LE/BE`, `Int32LE/BE` | `number` |
| 64‑bit | `BigUint64LE/BE`, `BigInt64LE/BE` | `BigInt` |
| float | `FloatLE/BE` (32‑bit), `DoubleLE/BE` (64‑bit) | `number` |

| Parameter | Type | Description |
|---|---|---|
| `buf` | `Uint8Array` | The buffer. `write*` mutates it in place. |
| `offset` | `number` | The byte offset. |
| `value` | `number \| BigInt` | (`write*`) the value; `BigInt` for the 64‑bit integer forms. |

**Throws** `RangeError` if `[offset, offset+width)` is out of bounds.

```js
const b = new Uint8Array(8);
writeUint32LE(b, 0, 0xdeadbeef);   // returns 4
readUint32LE(b, 0).toString(16);   // "deadbeef"
```

### Text conversions

> Hex and base64 are **not here** — they live in [`dyna:encoding`](#encoding), which is the
> single owner of every binary-to-text codec. `dyna:bytes` used to carry `toHex`/`fromHex`/
> `toBase64`/`fromBase64` as a third set of names over the same shared SIMD kernels; use
> `HexEncode`/`HexDecode`/`Base64Encode`/`Base64Decode` instead. They accept the same byte
> views (`DataView` included) plus a string.

- `toUtf8(buf) → string` / `fromUtf8(s: string) → Uint8Array` — decode/encode UTF‑8. This *is* here:
  it is the byte↔string boundary of a byte module, and nothing else duplicates it.

```js
import { toUtf8, fromUtf8 } from "dyna:bytes";
import { Base64Decode } from "dyna:encoding";

toUtf8(fromUtf8("hi"));               // "hi"        — round trip
toUtf8(Base64Decode("aGk="));         // "hi"        — decode, then read as text
```

---

## Text kernels

Throughput-oriented SIMD kernels for reading bytes AS text. Inputs are `bytes`; multi-byte
results are fresh `Uint8Array`s.

`count` and `indexOfAny` are methods on [`Bytes`](#bytes-1) rather than free functions: they are
byte scans, and the handle is where a byte scan belongs.

### `count(data, ch)`

Counts occurrences of a single byte.

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | The buffer. |
| `ch` | `string \| number` | The byte to count: a string's first byte, or a number (low 8 bits). |

**Returns** `number`.

### `Bytes.prototype.indexOfAny(chars)`

A method on [`Bytes`](#bytes-1), not a free function.

| Parameter | Type | Description |
|---|---|---|
| `chars` | `bytes` | The set of bytes to look for. |

**Returns** `number` — the first index at which any byte of `chars` occurs, or `-1`.

```js
import { Bytes } from "dyna:bytes";
print(new Bytes("hello world").indexOfAny(new Bytes("aeiou")));   // 1
```

### `isValidUtf8(data)` · `countUtf8(data)`

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | The buffer to inspect. |

**Returns** `isValidUtf8`: `boolean`. `countUtf8`: `number` (Unicode code-point count of well-formed UTF‑8).

### `latin1ToUtf8(data)` · `utf8ToLatin1(data)`

Transcode between Latin‑1 (ISO‑8859‑1) and UTF‑8.

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | Latin‑1 bytes (`latin1ToUtf8`) or UTF‑8 bytes (`utf8ToLatin1`). |

**Returns** `Uint8Array`. **Throws** (`utf8ToLatin1`) `RangeError` if a code point exceeds U+00FF.

### `utf8ToUtf16(data)` · `utf16ToUtf8(u16bytes)`

Lossless transcode between UTF‑8 and UTF‑16LE. **Strict**: malformed input is rejected, never
replaced with U+FFFD.

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | (`utf8ToUtf16`) UTF‑8 input. |
| `u16bytes` | `bytes` | (`utf16ToUtf8`) UTF‑16LE bytes (little-endian `uint16` units). |

**Returns** `Uint8Array` — UTF‑16LE bytes, or UTF‑8 bytes, respectively.
**Throws** `RangeError` — `utf8ToUtf16` on malformed UTF‑8; `utf16ToUtf8` on a lone/misordered surrogate, a high surrogate at end-of-buffer, or an odd byte length.

```js
const u16 = utf8ToUtf16("😀");     // 4 bytes (a surrogate pair)
utf16ToUtf8(u16);                  // the original UTF-8 bytes
```

### `isValidUtf16(u16bytes)` · `countUtf16(u16bytes)`

| Parameter | Type | Description |
|---|---|---|
| `u16bytes` | `bytes` | UTF‑16LE bytes. |

**Returns** `isValidUtf16`: `boolean` (well-formed, all surrogates paired). `countUtf16`: `number` (code-point count).

---

# encoding

### JSON5 and canonical JSON

`import { JSON5Parse, JSON5Stringify, StableStringify } from "dyna:encoding";`

| Function | Signature | Description |
|---|---|---|
| `JSON5Parse(text)` | `(string) → value` | The JSON5 dialect: comments, trailing commas, unquoted keys, single quotes, hex, `+`/`-Infinity`, `NaN`, line continuations. |
| `JSON5Stringify(value, {indent}?)` | `(value, object) → string` | JSON5 output; prints `NaN`/`Infinity`, which JSON cannot. |
| `StableStringify(value, {}?)` | `(value) → string` | RFC 8785 (JCS) canonical form. |

Valid JSON is valid JSON5, and parses identically. Nesting is capped at **256**
and the cap is checked **before** descending, so a nest bomb is a `RangeError`
rather than a stack overflow. Keys are written with define semantics, so a
document containing `__proto__` produces an own property and cannot retarget a
prototype.

`StableStringify` is for hashing and etags: keys sort by **UTF-16 code unit**
(RFC 8785's rule — *not* code-point order, which disagrees for astral
characters), numbers take the ECMAScript shortest round-trip form, `-0`
canonicalises to `0`, there is no whitespace at any `indent`, and a non-finite
number is **rejected** because it has no canonical form. A cycle throws; a
repeated non-cyclic node does not.

```js
JSON5Parse("{a: 1, /* note */ b: [2,]}");   // { a: 1, b: [2] }
JSON5Parse("0x10");                          // 16
StableStringify({ b: 1, a: 2 });             // '{"a":2,"b":1}'
StableStringify({ a: 1 }, { indent: 4 });    // '{"a":1}' — canonical means one form
```


### JSONPath

`import { JSONPath } from "dyna:encoding";`

A **compiled query** over plain JSON-shaped values: RFC 9535, the standardised
JSONPath. The expression is parsed once by the constructor and reused, which is
the whole reason it is a class rather than a function.

| Member | Signature | Description |
|---|---|---|
| `new JSONPath(expression)` | `(string) → JSONPath` | Compiles. Throws `SyntaxError` with an offset on a bad expression. |
| `.all(value)` | `(value) → array` | Every selected node, in document order. |
| `.first(value)` | `(value) → value` | The first, or `undefined` when nothing matched. |
| `.paths(value)` | `(value) → array` | The **normalized path** of each result, e.g. `$['store']['book'][0]`. |

| Syntax | Meaning |
|---|---|
| `$` | the document root |
| `.name`, `['name']`, `["name"]` | a member |
| `[3]`, `[-1]` | an element, counting from the end when negative |
| `[*]`, `.*` | every member value or element |
| `[1:5:2]`, `[::-1]` | a slice; the bounds follow `Array.prototype.slice`, and a step may be negative |
| `[0,2]`, `['a','b']` | a union — results appear in the order **written**, not sorted |
| `..` | a descendant segment: the node and all its descendants, in preorder |
| `[?<expr>]` | a filter over the children of the node |

A filter expression is `@.field` (existence), a comparison of two *comparables*
with `==` `!=` `<` `<=` `>` `>=`, and those combined with `&&`, `\|\|`, `!` and
parentheses. A comparable is a literal (number, string, `true`, `false`, `null`)
or a **singular query** — `@` or `$` followed by name and index steps only.
`$` inside a filter is the whole document, `@` is the node being tested.
Ordering is defined for two numbers or two strings; anything else compares
`false`. A missing member is *Nothing*, which equals only Nothing.

**A query never runs code.** An accessor property is skipped, not invoked, and
inherited properties are not visible — a name selector reads own data
properties, and a wildcard or descendant segment iterates own **enumerable**
ones, the same set `Object.keys` returns. That is what makes it safe to run an
expression over a document you did not write.

Limits, each a `RangeError` or `SyntaxError` rather than an unbounded walk: the
expression is at most **4096** bytes, filter nesting at most **32**, and one
evaluation visits at most **2²⁰** nodes.

**The accepted grammar is exactly the table above.** RFC 9535's function
extensions — `length()`, `count()`, `match()`, `search()`, `value()` — are
outside it and raise a `SyntaxError`; a filter operand is a singular query; and
`==` between two objects or two arrays is `false`, where the RFC specifies deep
equality. Script expressions are outside RFC 9535 itself, in every spelling, and
that hole is the reason to take the standardised grammar.

```js
const doc = { store: { book: [
    { title: "Sayings of the Century", price: 8.95 },
    { title: "Sword of Honour", price: 12.99 },
    { title: "Moby Dick", price: 8.99, isbn: "0-553-21311-3" } ] } };

const q = new JSONPath("$.store.book[?@.price < 10].title");
q.all(doc);      // [ "Sayings of the Century", "Moby Dick" ]
q.paths(doc);    // [ "$['store']['book'][0]['title']", "$['store']['book'][2]['title']" ]
new JSONPath("$..isbn").first(doc);     // "0-553-21311-3"
new JSONPath("$..*").all(doc).length;   // 12 -- every node except the root
```


`import { HexEncode, HexDecode, Base64Encode, Base64Decode, Base64URLEncode, Base64URLDecode, Base32Encode, Base32Decode, Base32HexEncode, Base32HexDecode, Base85Encode, Base85Decode, PutUvarint, Uvarint, PutVarint, Varint } from "dyna:encoding";`

Binary↔text codecs and LEB128 variable-length integers. Hex and base64 run on the SIMD kernels.

### Hex — `HexEncode(data)` / `HexDecode(s)`

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | (encode) the input bytes. |
| `s` | `string` | (decode) a hex string. |

**Returns** `HexEncode`: `string` (lowercase). `HexDecode`: `Uint8Array`.
**Throws** (`HexDecode`) `SyntaxError` on an odd-length string or a non-hex character.

### Base64 family

`Base64Encode/Base64Decode` (standard `+/`, padded) and `Base64URLEncode/Base64URLDecode`
(URL-safe `-_`, unpadded).

| Parameter | Type | Description |
|---|---|---|
| `data` | `bytes` | (encode) the input. |
| `s` | `string` | (decode) the encoded text. |

**Returns** encode: `string`; decode: `Uint8Array`. **Throws** `SyntaxError` on invalid input (decode).

### Base32 family

`Base32Encode/Base32Decode` (RFC 4648 standard alphabet) and `Base32HexEncode/Base32HexDecode`
(extended-hex alphabet). Same parameter/return/throw shape as the base64 family.

### Ascii85 — `Base85Encode(data)` / `Base85Decode(s)`

Same shape: `Base85Encode(data: bytes) → string`, `Base85Decode(s: string) → Uint8Array`.

```js
Base85Encode(HexDecode("deadbeef"));   // "hQ=N\\"
```

### Variable-length integers — `PutUvarint` / `Uvarint` / `PutVarint` / `Varint`

LEB128 encoding. The `u`-forms are unsigned; the signed forms use zig-zag.

### `PutUvarint(value)` · `PutVarint(value)`

| Parameter | Type | Description |
|---|---|---|
| `value` | `number \| BigInt` | The integer to encode. `PutUvarint` requires a non-negative value. |

**Returns** `Uint8Array` — the encoded bytes (1–10). **Throws** `RangeError` if `PutUvarint` is given a negative value, or the value is not a safe integer/BigInt.

### `Uvarint(buf)` · `Varint(buf)`

| Parameter | Type | Description |
|---|---|---|
| `buf` | `bytes` | A buffer whose prefix holds a var-int. |

**Returns** `[value: number | BigInt, bytesRead: number]`. **Throws** on truncated input or overflow past 64 bits.

```js
const enc = PutUvarint(300);   // Uint8Array(2) [ 0xac, 0x02 ]
Uvarint(enc);                  // [300, 2]
```

---

### QR Code

`QREncode(text, opts?)` → `{version, size, modules}` — `modules` is a
`Uint8Array` of `size × size`, one byte per module, 1 = dark, row-major.
`QRToString(text, opts?)` → the same symbol as terminal half-blocks, with the
two-module quiet zone **without which no scanner locks on**.

`opts` is `{ecc: "L"|"M"|"Q"|"H", version: 1..40, mask: 0..7}`. `ecc` defaults
to `"M"`; `version` and `mask` are chosen for you unless pinned. Byte mode
only, so the payload is the UTF-8 bytes of `text` — at most 2953 of them, and
more is a `RangeError` rather than a truncated payload.

Verified module-for-module against an independent implementation across all
four levels, all eight masks and versions 1 to 40.

```js
QREncode("dynascript").size;                    // 21 -- version 1
QREncode("dynascript", { ecc: "H" }).version;   // a bigger symbol: more parity
QREncode("hi", { mask: 0 }).modules[0];         // 1 -- the finder's top-left
```

### Base58 and BaseX — `Base58Encode(data)` / `Base58Decode(s)` / `Base58CheckEncode(data)` / `Base58CheckDecode(s)` / `BaseXEncode(data, alphabet)` / `BaseXDecode(s, alphabet)`

**These are DIVISION codecs, not bit-packing ones.** The radix does not divide
256, so encoding is repeated divmod over the whole number and costs **O(n²)**.
That is correct and fine for the 32-byte inputs they exist for; input is capped
at **4096 bytes** in both directions, and the cap is the defence rather than a
tidiness rule.

Base58 uses the Bitcoin alphabet, which omits `0`, `O`, `I` and `l`. Each
**leading zero byte** becomes one leading `1` — that is what makes the format
length-preserving for addresses. `Base58Check` appends the first four bytes of
the **double** SHA-256 as a checksum, so a single mistyped character is
rejected rather than decoded into a different value.

`BaseX` takes any alphabet of 2–255 characters. **A repeated character is
refused**: the second occurrence would be unreachable, so a value would encode
one way and read back as another.

```js
Base58Encode(HexDecode("516b6fcd0f"));            // "ABnLTmg"
Base58Encode(new Uint8Array([0, 0, 1]));          // "112" -- one '1' per zero byte
Base58CheckEncode(HexDecode("00010966776006953D5567439E5E39F86A0D273BEE"));
// "16UwLL9Risc3QfPqBUvKofHmBQ7wMtjvM"
BaseXEncode(HexDecode("ff00"), "01");             // "1111111100000000"
```

# xml

`import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";`

One scanner behind three front ends: a document tree, a streaming parser, and a
serializer. **No DTD is processed, ever** — a `<!DOCTYPE` is scanned past with a
bracket counter and its contents are never read — so an external entity has
nothing to declare it and XXE is *unrepresentable* rather than disabled.

| Function | Signature | Description |
|---|---|---|
| `XMLParse(text, options?)` | `(string, object) → element` | The document's root element. |
| `XMLStringify(node, options?)` | `(element, object) → string` | Back to XML; `indent` is 0–16. |
| `XMLToObject(node)` | `(element) → object` | The collapsed, **lossy** convenience shape. |
| `new SAXParser(handlers)` | `(object) → SAXParser` | Streaming: `.write(chunk)`, `.end()`. |

### The tree

An element is a plain object — `{ name, attrs, children }` — with text as
strings in `children`, so mixed content and element order survive:

```js
XMLParse('<a id="1">hi<b/>there</a>');
// { name: "a", attrs: { id: "1" }, children: [ "hi", {name:"b",…}, "there" ] }
```

`options.trim` (default `true`) drops whitespace-only text nodes, which is what
makes an indented document read as data rather than as text interleaved with
elements. `options.entities` is `"strict"` (default) or `"keep"`.

Attributes are written with **define** semantics, so a document containing an
attribute or element called `__proto__` produces an own property and cannot
retarget a prototype.

### Entities

The five predefined entities (`&lt; &gt; &amp; &apos; &quot;`) and numeric
character references. **Every other entity is an error**, because no DTD is
read and therefore none can exist — an entity bomb fails on its first reference
rather than on an expansion counter. A character reference that names a
surrogate, NUL, or a value above U+10FFFF is refused. Under
`entities: "keep"` an unknown reference passes through as literal text; it is
never expanded. Output is therefore bounded by input × 4.

### Bounds, all defaults, none optional

| Bound | Value |
|---|---|
| Nesting depth | 256 |
| Attributes per element | 1024 |
| One name, attribute value or text run | 16 MiB |
| Whole-document input (`XMLParse`) | 256 MiB |
| DTD | not read |

### Streaming

```js
const names = [];
const p = new SAXParser({
    onOpen(name, attrs) { names.push(name); },
    onClose(name) {}, onText(t) {}, onCData(t) {},
    onComment(t) {}, onPI(target, data) {}
});
p.write("<a><b");        // a string, a Uint8Array, any byte view, or a buffer
p.write("/></a>");       // a chunk may split any token
p.end();
names;                   // [ "a", "b" ]
```

Every handler is optional. A chunk may split any token: the parser keeps only
the unconsumed tail, so the event stream is identical whatever the chunk sizes
are, and the test proves that by feeding the same document at every chunk size
from one byte up. A handler that throws stops the parse and the exception
reaches the caller. Calling `write()` **from inside a handler** is a
`TypeError` — the carry buffer is not reentrant.

### `XMLToObject` — lossy on purpose

Element order and mixed content do not survive it. An element with only text
becomes that string; attributes become `@name` keys; text beside elements
becomes `#text`; a repeated child name becomes an array.

```js
XMLToObject(XMLParse("<a><b>1</b><b>2</b></a>"));   // { a: { b: ["1", "2"] } }
XMLToObject(XMLParse('<a id="7">x</a>'));           // { a: { "@id": "7", "#text": "x" } }
```

Namespace prefixes are preserved verbatim in `name`, and `xmlns` attributes are
ordinary attributes; the tree is the shape to walk when a document's prefixes
matter. Building a document is `XMLStringify` over a plain object — a builder
class would be a second spelling of the same thing.

```js
const doc = XMLParse("<rss><channel><title>News</title></channel></rss>");
doc.children[0].children[0].children[0];        // "News"
XMLStringify(doc, { indent: 2 });               // re-serialised, re-escaped
```

# yaml

`import { Parse, ParseAll, Stringify } from "dyna:yaml";`

The **YAML 1.2 core schema**, block and flow, and nothing else. Everything
outside the subset is refused **by name**, because a config parser that quietly
drops an anchor returns a document its author did not write.

| Function | Signature | Description |
|---|---|---|
| `Parse(text)` | `(string) → value` | One document. More than one is a `SyntaxError` naming `ParseAll`. |
| `ParseAll(text)` | `(string) → array` | Every `---`-separated document. |
| `Stringify(value, {indent}?)` | `(value, object) → string` | Block style; `indent` is 1–10, default 2. |

### What is here

Block mappings and sequences by indentation, the compact `- key: value` form,
flow collections (`[1, 2]`, `{a: 1}`), single- and double-quoted scalars with
the JSON escapes, literal (`|`) and folded (`>`) block scalars with the `-` and
`+` chomping indicators, comments, and multiple documents.

**Scalars resolve by the 1.2 core rules**, which is what ends the Norway
problem: `no`, `yes`, `on`, `off`, `y` and `n` are **strings**, `~` and `null`
are null, `true`/`false` are booleans, and `12:30:00` is a string rather than a
sexagesimal number. `1.20` is the number `1.2`; quote it to keep the text.

Keys are written with **define** semantics, so a document containing
`__proto__` produces an own property and cannot retarget a prototype. Nesting is
capped at 128 and input at 64 MiB. A tab in the indentation is an error.

### What is refused, and why by name

| Construct | Refusal |
|---|---|
| `&anchor` / `*alias` | there are no anchors, so nothing can reference one |
| `!tag` / `!!tag` | the core schema is the only schema; no constructor runs |
| `<<` merge key | it needs an anchor, which does not exist here |
| `%YAML` / `%TAG` | no directives |
| `? explicit key` | keys are scalars |

Refusing is the point. Anchors and tags are YAML's amplification and
object-injection surface; a parser that ignored them would return a document
missing values, and nothing downstream could tell. If you need them, the file
needs a full YAML implementation, and this one says so instead of guessing.

`Stringify` quotes any scalar that would read back as something else — including
`yes`, `no`, `on` and `off`, which this parser reads as strings but a YAML 1.1
reader does not. A round trip that only agrees with itself is not enough.

```js
Parse("name: app\nports:\n  - 80\n  - 443\nenv:\n  DEBUG: 'true'\n");
// { name: "app", ports: [80, 443], env: { DEBUG: "true" } }

Parse("country: no").country;              // "no" -- a string, per YAML 1.2
Parse("script: |\n  line one\n  line two\n").script;   // "line one\nline two\n"
ParseAll("a: 1\n---\nb: 2").length;        // 2
Stringify({ a: [1, 2], b: "yes" });        // 'a:\n  - 1\n  - 2\nb: "yes"\n'
```

# decimal

`import { Decimal, Money } from "dyna:decimal";`

Exact decimal arithmetic, and a separate integral type for money. The default
context is **IEEE 754-2008 decimal128** — 34 significant digits, half-even —
which is a standard rather than a house rule, and the same one Python's
`decimal`, Java's `BigDecimal` and SQL `NUMERIC` speak.

```js
new Decimal("0.1").add(new Decimal("0.2")).toString();   // "0.3"
0.1 + 0.2;                                               // 0.30000000000000004
```

### `Decimal`

`new Decimal(value)` takes a string, a finite number, or another `Decimal`. A
**number arrives through its own shortest round-trip text**, so `new
Decimal(0.1)` is `0.1` — not the double's 55-digit binary expansion, and not a
silent reinterpretation. `NaN` and `Infinity` have no decimal value and are
refused, as is any text outside `[+-]digits[.digits][eE[+-]digits]`.

| Method | Description |
|---|---|
| `add` `sub` `mul` | **Exact**, always. Rounding a sum that fits is how a ledger loses a cent. |
| `div(x, opts?)` | Rounded to `opts.precision` (default 34) by `opts.rounding` (default `halfEven`). |
| `mod(x)` | The remainder of truncated division; it takes the dividend's sign. |
| `pow(n, opts?)` | Integer exponent, \|n\| ≤ 10000. A negative exponent divides. |
| `abs()` `neg()` | |
| `cmp(x)` → −1/0/1, `equals(x)` | By **value**: `1.50` equals `1.5`. |
| `round(places, mode?)` | A negative `places` rounds to tens, hundreds, … |
| `toFixed(places, mode?)` | Exactly that many places, zero-padded. |
| `toString()` `toJSON()` | Plain notation, never exponential. |
| `toNumber()` | The one place a Decimal may become approximate. |
| `isZero()` `sign()` `digits()` | |

The eight rounding modes are `up`, `down`, `ceil`, `floor`, `halfUp`,
`halfDown`, `halfEven`, `halfOdd`. **`parse(format(x))` is exactly `x`** —
decimal round-trips are exact, unlike floating point, so the test asserts
equality rather than a tolerance.

There is no `Decimal.config`: a process-global default would change the result
of code that never asked, so precision and rounding are arguments where they
matter. Grouping, compact and byte formatting for ordinary numbers are already
`Number.prototype.format`, `abbr`, `metric` and `bytes` — this module does not
ship a second spelling of them.

### `Money`

`new Money(minorUnits, currency, {minorDigits}?)`. The amount is an **integer
count of the smallest unit** — `1999` is `$19.99` — because money is integer
arithmetic with a unit, and most money bugs come from using a float-shaped type
for it. A fractional amount is refused. The currency is a 3-letter code, and
its minor-unit count comes from the ISO register (JPY 0, BHD 3, most 2).

| Method | Description |
|---|---|
| `add` `sub` `cmp` `equals` | The operand must be a `Money` of the **same currency** — USD plus EUR is a missing exchange rate, not arithmetic. |
| `mul(n)` | Integer only; use `allocate` to split. |
| `allocate(weights)` | Split into shares that sum **exactly** to the original. |
| `amount()` `currency()` | The minor-unit count and the code. |
| `toString()` `format()` `toDecimal()` | `"19.99"`, `"$19.99"`, a `Decimal`. |

`allocate` is the operation that justifies the type: the remainder goes one
minor unit at a time to the earliest shares, so nothing is created and nothing
is lost. `new Money(100, "USD").allocate([1,1,1])` is 34, 33, 33 cents.

```js
const price = new Money(1999, "USD");
price.add(new Money(50, "USD")).format();      // "$20.49"
price.allocate([1, 1, 1]).map((m) => m.amount());   // [ 667, 666, 666 ]
new Decimal("1").div(new Decimal("3"), { precision: 5 }).toString();   // "0.33333"
new Decimal("2.5").round(0).toString();        // "2" -- half-even
```

# serialize

`import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode, CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";`

One graph walker, two wire formats, a value hash and a deep clone. (Container
persistence for `dyna:structures` is a different thing and lives there.)

| Function | Signature | Description |
|---|---|---|
| `MsgPackEncode(value)` / `MsgPackDecode(bytes)` | `(value) → Uint8Array` / `(bytes) → value` | MessagePack. |
| `CBOREncode(value)` / `CBORDecode(bytes)` | | RFC 8949. |
| `CBORCanonical(value)` | `(value) → Uint8Array` | CBOR with keys in the RFC's canonical order. |
| `ValueHash(value)` | `(value) → string` | 16 hex digits: xxh64 over the canonical encoding. |
| `structuredClone(value)` | `(value) → value` | A deep clone that **preserves cycles**. |

Both codecs carry `null`, booleans, numbers (integers in their shortest form,
everything else as a double), strings, byte views, arrays and objects.
`CBORDecode` also reads half- and single-precision floats and passes tagged
values through as their content, though it never writes either.

**A cycle is refused**, by name, because neither wire format can express one —
use `structuredClone` when you need the graph. A value that merely appears
twice is not a cycle: only the ancestor chain is checked, so ordinary shared
data encodes fine.

Decoding is the untrusted side, and the defence is that **a declared length is
checked against the bytes that remain before anything is allocated** — a
four-byte count is otherwise a four-gigabyte allocation. Nesting is capped at
256, indefinite-length CBOR items are refused because every length here is
declared, and trailing bytes after a complete value are an error rather than
being ignored. Map keys are written with define semantics, so `__proto__` in a
document becomes an own property.

`ValueHash` is object-hash's job with a **defined** canonical form rather than a
house convention: keys sort by length then bytes (RFC 8949 §4.2.1), so
`{a:1,b:2}` and `{b:2,a:1}` hash identically while `[1,2]` and `[2,1]` do not.

`structuredClone` is the missing global's semantics for plain data: plain
objects, arrays and byte views, with **cycles preserved** and shared identity
inside the graph kept shared. A function cannot be cloned.

```js
MsgPackDecode(MsgPackEncode({ id: 7, tags: ["a", "b"] }));   // the same value
CBOREncode(1000);                                            // 1903e8, per the RFC
ValueHash({ a: 1, b: 2 }) === ValueHash({ b: 2, a: 1 });     // true

const a = { name: "a" };
a.self = a;
const c = structuredClone(a);
c.self === c;                                                // true, and c !== a
```

# html

`import { HTMLParse, HTMLStringify, Selector, Sanitizer } from "dyna:html";`

A lenient HTML parser, compiled CSS selectors, and a sanitizer. The node shape
is `dyna:xml`'s on purpose — `{ name, attrs, children }`, text as strings — so
one tree walker serves both markup languages.

| Member | Signature | Description |
|---|---|---|
| `HTMLParse(text)` | `(string) → array` | The top-level nodes. Tag and attribute names lower-case. |
| `HTMLStringify(node)` | `(node\|array) → string` | Back to HTML, re-escaped. |
| `new Selector(css)` | `(string) → Selector` | Compiled once, reused. |
| `.all(doc)` `.first(doc)` `.matches(node)` | | Document order. |
| `new Sanitizer({allow, protocols})` | `(object) → Sanitizer` | Compiled policy. |
| `.clean(html)` | `(string) → string` | The safe subset, as text. |

### What the parser does

Void elements (`img`, `br`, `input`, …) open no scope. An open `<p>` is closed
by the next block-level element and an `<li>` by the next `<li>`, which is why
real pages parse at all. **An unmatched close tag is ignored** rather than
unwinding the document. `script`, `style`, `textarea` and `title` are
**raw text**: a `<` inside them is a character, and treating it as a tag is the
classic mXSS opening. Comments and doctypes are consumed, not stored.

**Browser-grade recovery for misnested formatting tags is not part of this
parser.** Input that relies on the adoption-agency algorithm parses as written,
not as a browser would repair it. The sanitizer's allow-list does not depend on
that recovery.

Named character references cover the working set rather than all 2231 HTML5
names; an unknown `&name;` stays literal, as a browser leaves it. A reference to
a surrogate or an out-of-range code point becomes U+FFFD.

### Selectors

Tag, `#id`, `.class`, `[attr]`, `[attr=v]`, `[attr^=v]`, `[attr$=v]`,
`[attr*=v]`, `[attr~=v]`, `:first-child`, `:last-child`, the descendant and
child (`>`) combinators, and comma-separated groups. `.class` matches a **word**
in the attribute, which is how a multi-class element works.

`+`, `~`, `:not()` and `:nth-child()` are outside this grammar and are a
`SyntaxError`, as is a trailing combinator — accepting `div >` silently would
make it mean `div`. `matches()` refuses a selector containing a combinator: it
sees one node with no ancestors, and answering `false` would be a lie.

### The sanitizer

`allow` maps a tag to the attributes permitted on it; `protocols` maps
`"tag.attr"` to the URL schemes permitted there. **There is no default policy** —
a default is a policy nobody read.

A disallowed element loses its **tag** and keeps its children, because dropping
the subtree silently deletes content the author wrote. A raw-text element is the
exception: its content goes with it, because that content *is* script. Text and
attribute values are re-escaped, so the output re-parses to the same thing, and
`clean()` is idempotent.

```js
const doc = HTMLParse('<div id="m"><p class="x">one</p><p>two</p></div>');
new Selector("div > p.x").all(doc).length;       // 1
new Selector("p").first(doc).children[0];        // "one"

const san = new Sanitizer({
    allow: { p: [], b: [], a: ["href"] },
    protocols: { "a.href": ["https", "mailto"] },
});
san.clean('<p onclick="x()">hi <script>alert(1)</script><b>b</b></p>');
// "<p>hi <b>b</b></p>"
san.clean('<a href="javascript:alert(1)">x</a>');   // "<a>x</a>"
```

### Markdown

`MarkdownToHTML(text, options?)` — CommonMark's core plus the GFM pieces people
actually use, rendered through this module's escaper.

**Raw HTML in the source is escaped, not passed through.** A renderer that emits
its input verbatim is an XSS hole with a nice API, so `<script>` in a document
becomes text. `{ allowRawHTML: true }` is the explicit opt-in, and it means the
caller has taken responsibility — pipe the result through `Sanitizer` if the
source is untrusted.

**A link's scheme is checked**: `[x](javascript:alert(1))` renders with an empty
`href`, because it is the same hole as a raw `<a>`. `http`, `https`, `mailto`,
`ftp` and relative references are kept.

Blocks: ATX and setext headings, paragraphs, thematic breaks, bullet and
ordered lists, block quotes (whose content is parsed as blocks), fenced code
with an info string, indented code, and GFM tables. Inline: emphasis, strong,
strikethrough, code spans, links with titles, images, autolinks and backslash
escapes.

Reference-style links (`[t][ref]`), footnotes, task lists and HTML blocks are
outside this renderer, and nesting is capped at 64.

```js
MarkdownToHTML("# Title\n\nA *para* with [a link](https://e.com).");
// "<h1>Title</h1>\n<p>A <em>para</em> with <a href=\"https://e.com\">a link</a>.</p>\n"
MarkdownToHTML("<script>alert(1)</script>");
// "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n"
```

### `class Template`

A Mustache-shaped template, compiled once and rendered many times. It lives in
this module because the thing it must get right is **escaping**, and a second
escaper is a second thing to keep correct.

**`new Template(source, {escape}?)`** · **`.render(data)` → string**

| Tag | Meaning |
|---|---|
| `{{name}}` | The value, **escaped**. |
| `{{{name}}}` or `{{&name}}` | The value, raw. |
| `{{#name}}…{{/name}}` | Section: renders once for a truthy value, once **per element** for an array (with `{{.}}` as the item), and pushes an object as a scope. |
| `{{^name}}…{{/name}}` | Inverted: renders only when the value is falsy. |
| `{{! note }}` | A comment. |
| `{{a.b.c}}` | A dotted path. |

**An empty array is falsy**, which is Mustache's rule and the one a plain
truthiness test gets wrong. Inside a section the outer scopes stay visible,
innermost first. A missing name renders as nothing.

`{{name}}` escapes `& < > " '` — all five, because **a template cannot know
whether its value lands in text, a double-quoted attribute or a single-quoted
one**. That is a different contract from `HTMLStringify`, which knows.

**A template is data.** A function in the data is refused rather than called,
and an accessor property is not read — a template that runs code is an
evaluator with a nicer syntax. `{{> partial}}` and `{{=<% %>=}}` are refused
too: the first needs a loader, the second changes the grammar mid-file.

```js
const t = new Template("Hi {{name}}!{{#tags}} #{{.}}{{/tags}}");
t.render({ name: "<b>", tags: ["a", "b"] });   // "Hi &lt;b&gt;! #a #b"
new Template("{{{raw}}}").render({ raw: "<b>" });   // "<b>"
```

# hash

`import { MD5, SHA1, SHA224, SHA256, SHA384, SHA512, /* + *Hex */ CRC32, CRC32C, XXHash64, XXHash32, Hasher } from "dyna:hash";`

One-way reduction of bytes to a fixed-size tag, with **no secret involved**. The question that puts a
function here is *"would I use this as a checksum, a cache key, or a content id?"* — which is why
`MD5` and `SHA1` live here rather than in `dyna:crypto`: they are fine as content ids and are not
security primitives.

Every data argument is a string (hashed as its UTF-8 bytes), an `ArrayBuffer`, or any
`TypedArray`/`DataView`. A string and the equivalent `Uint8Array` hash identically.

| Function | Signature | Description |
|---|---|---|
| `MD5` `SHA1` `SHA224` `SHA256` `SHA384` `SHA512` | `(data) → Uint8Array` | Digest bytes. |
| `MD5Hex` … `SHA512Hex` | `(data) → string` | Lowercase hex. |
| `CRC32(data)` / `CRC32C(data)` | `(data) → number` | IEEE 802.3 and Castagnoli. |
| `XXHash64(data, seed?)` | `(data, number?) → string` | **16 hex characters**, not a number: a JS number carries 53 exact bits and this value has 64, so returning one would collide in ways the algorithm does not. |
| `XXHash32(data, seed?)` | `(data, number?) → number` | Fits a JS number exactly. |

### `class Hasher`

Streaming digest. `new Hasher(algorithm)`, then `update(x)` any number of times and `digest()` or
`digestHex()`; the digest **does not consume** the state, and `reset()` starts a fresh message.

**`Hasher` is not the fast path.** It replaces one call (`SHA256Hex(x)`) with three, and it has no
expensive configuration to hoist to win that back. It exists so you can hash a stream you do not
hold in memory. For one buffer, use the free function.

---

### SHA-3, Keccak and SHAKE

`SHA3_224` `SHA3_256` `SHA3_384` `SHA3_512` `Keccak256` — `(data) → Uint8Array`,
each with a `…Hex` twin returning lowercase hex.

`SHAKE128(data, length = 32)` and `SHAKE256(data, length = 32)` are extendable
output functions: a shorter squeeze is a **prefix** of a longer one, and length
runs from 1 to 1 MiB.

**`Keccak256` is not `SHA3_256`.** They share the permutation and differ by one
padding byte — FIPS 202 appends `0x06`, original Keccak appends `0x01` — so a
mix-up produces a perfectly good digest of the wrong function. `Keccak256` is
the one Ethereum uses; `Keccak256Hex("")` is
`c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470`.

```js
SHA3_256Hex("abc");   // "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"
Keccak256Hex("");     // "c5d24601…" -- a different function, not a different spelling
SHAKE128Hex("", 16) === SHAKE128Hex("", 32).slice(0, 32);   // true
```

### BLAKE3, BLAKE2 and Murmur3

`BLAKE3(data, length = 32)` — `(data, number?) → Uint8Array`, with `BLAKE3Hex`.
BLAKE3 is a **Merkle tree of 1 KiB chunks**, not a serial hash, and it is an
extendable output function: length runs from 1 to 1 MiB and a short digest is a
prefix of a long one.

`BLAKE2b(data, length = 64)` and `BLAKE2s(data, length = 32)` — RFC 7693, with
`…Hex` twins. BLAKE2b caps at 64 bytes and BLAKE2s at 32; asking for more is a
`RangeError` rather than a silently truncated digest.

`Murmur3_128(data, seed = 0)` — `(data, number?) → Uint8Array` (16 bytes), with
`Murmur3_128Hex`. **Not cryptographic**: it is fast and well-distributed, which
is what a hash table or a sketch wants and is not what an attacker-facing tag
wants. Use it for bucketing; use SHA-256 or BLAKE3 for anything else.

```js
BLAKE3Hex("abc");        // "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"
BLAKE2sHex("abc");       // "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"
Murmur3_128Hex("abc");   // "6778ad3f3f3f96b4522dca264174a23b"
BLAKE3Hex("abc", 16) === BLAKE3Hex("abc", 32).slice(0, 32);   // true -- a prefix
```

# crypto

`import { HMAC, HMACHex, Hmac, TimingSafeEqual, HKDF, PBKDF2, RandomBytes } from "dyna:crypto";`

Everything that depends on a **secret** or on **constant-time execution**. The question that puts a
function here is *"does an attacker learn something if this leaks timing or key handling?"*

| Function | Signature | Description |
|---|---|---|
| `HMAC(algo, key, data)` | `(string, key, data) → Uint8Array` | RFC 2104. |
| `HMACHex(algo, key, data)` | `(string, key, data) → string` | |
| `TimingSafeEqual(a, b)` | `(bytes, bytes) → boolean` | Compares in time that depends only on the length. `false` for different lengths — a MAC's length is public; **where** two equal-length values first differ is not. |
| `HKDF(opts)` | `({hash?, key, salt?, info?, length?}) → Uint8Array` | RFC 5869 extract-then-expand. `hash` defaults to `"sha256"`, `length` to 32. |
| `PBKDF2(opts)` | `({hash?, password, salt?, iterations?, length?}) → Uint8Array` | RFC 8018. |
| `RandomBytes(n?)` | `(number?) → Uint8Array` | OS entropy. **Not** `dyna:random`, which is seeded and reproducible — the opposite of what this is for. |

`HKDF` and `PBKDF2` take an **options object**, not positional arguments, because the order of a key
and a salt is exactly the thing a caller gets wrong silently: swapping them derives a perfectly
good-looking key that is the wrong one.

### `class Hmac`

`new Hmac(algorithm, key)` derives the block-sized key schedule once and reuses it.

| Member | Signature | Description |
|---|---|---|
| `.sign(msg)` / `.signHex(msg)` | `(data) → Uint8Array \| string` | A complete MAC; the object is immediately reusable. |
| `.verify(msg, tag)` | `(data, bytes \| hex) → boolean` | Constant-time. |
| `.update(msg)` | `(data) → this` | Streaming. |
| `.digest()` / `.digestHex()` | `() → Uint8Array \| string` | Finalise a streamed message **and reset**. |
| `.algorithm` / `.digestSize` | getters | |
| `.close()` | `() → void` | Zeroes the key schedule now rather than at collection. |

**Use `verify`, not `signHex(msg) === tag`.** String equality exits at the first differing character
and so publishes how much of the MAC an attacker has guessed.

**Crossover depends on the key length, which is the honest answer.** With a key shorter than the
hash block the schedule is only a pad, so it costs almost nothing to redo and the class crosses over
at **N=100** for a 1.6% win. With a key longer than the block the free function must **hash the key
on every call**, and the class crosses over at **N=2**, reaching **1.94×**. The configuration a
capability hoists has to be expensive for it to pay, and here you can choose whether it is.


# cli

`import { Command, StyleText, Styles, IsTTY, Columns, ColorDepth } from "dyna:cli";`

### `class Command`

A compiled capability: the **spec** is the configuration, read once, and
`parse()` takes the data. Every builder method returns `this`.

| Member | Signature | Description |
|---|---|---|
| `new Command(name)` | `(string)` | |
| `.describe(text)` | `(string) → this` | |
| `.option(flags, desc?, opts?)` | `(string, string, {type,required,variadic,default}) → this` | `"-o, --out <path>"`. A `--long` name is required — it is the key the result is reported under. |
| `.argument(spec, desc?)` | `(string, string) → this` | `"<required>"`, `"[optional]"`, `"<rest...>"`. |
| `.command(sub)` | `(Command) → this` | |
| `.allowUnknown(on=true)` | `(boolean) → this` | |
| `.parse(argv)` | `(string[]) → {options, arguments, command, result?}` | |
| `.help()` | `() → string` | |
| `.name` | getter | |

Accepted forms: `--long`, `--long=value`, `-o value`, `-ovalue`, `-abc`
bundling, `--no-flag` negation, and `--` after which everything is positional
however much it looks like a flag. A `variadic` option collects repeats into an
array; a non-variadic one takes the last value.

**Types are declared, never guessed.** yargs-style implicit coercion is
declined: `--x=3` is the string `"3"` unless the option declared
`type: "number"`. A guessed type is how a CLI surprises the script that calls
it.

An unknown option is an **error** by default. `allowUnknown()` collects it as a
positional instead — opt-in, because silently swallowing a typo'd flag is how a
script quietly does the wrong thing.

### Styling and the terminal

| Function | Signature | Description |
|---|---|---|
| `StyleText(style, text)` | `(string\|string[], string) → string` | Node's `util.styleText` signature. An array composes and closes in reverse. |
| `Styles()` | `() → string[]` | Every style name this build knows. |
| `IsTTY(fd=1)` | `(number) → boolean` | |
| `Columns()` | `() → number` | `COLUMNS`, else 80. |
| `ColorDepth(fd=1)` | `(number) → 0\|4\|8\|24` | Bits of colour. `NO_COLOR` wins over everything. |

`StyleText` matches `util.styleText` deliberately rather than chalk's chaining
proxy — a third spelling of the same idea is the synonym the conventions
forbid. An unknown style is **refused**, not ignored: a silently dropped style
is a bug nobody sees.

Styled text carries no width: `StyleText("red", "hi").displayWidth()` is 2, so
it composes with the width math in `String.prototype`.

```js
const cmd = new Command("mytool").describe("does a thing")
  .option("-v, --verbose", "chatty", { type: "boolean" })
  .option("-o, --out <path>", "output file", { type: "string", required: true })
  .option("-n, --count <n>", "how many", { type: "number", default: 1 });

cmd.parse(["-v", "--out=f.txt", "-n5", "--", "--not-a-flag"]);
// { options: { verbose: true, out: "f.txt", count: 5 },
//   arguments: ["--not-a-flag"], command: null }

print(cmd.help());
if (ColorDepth() > 0) print(StyleText(["red", "bold"], "error"));
```

# validate

`import { IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii } from "dyna:validate";`

Format validators with a check digit or a real grammar. Each takes a string and
returns a boolean; a non-string is a `TypeError` and an input over 4 KiB is a
`RangeError` — a validator answers a question about one field, not a document.

| Function | Checks |
|---|---|
| `IsEmail(text)` | The practical grammar: one unquoted local part (≤64), a dotted domain, a letters-only TLD of at least two characters. |
| `IsIBAN(text)` | The **mod-97 check digit**, plus the country's registered length. Spaces and lowercase are tolerated. |
| `IsCreditCard(text)` | The **Luhn check digit**, 12–19 digits. Spaces and dashes are tolerated. |
| `IsAlpha` / `IsAlphanumeric` / `IsAscii` | Character classes. An empty string satisfies none of them. |

`IsEmail` matches what mail systems accept, not RFC 5322 — the RFC admits
comments, quoted strings and nested folding that no mail system round-trips, so
matching it exactly would accept addresses that bounce.

`IsIBAN` and `IsCreditCard` verify the **check digit**, so a single mistyped
character or a transposed pair is rejected rather than merely well-shaped. A
correct-looking IBAN of the wrong length for its country is invalid.

### Deliberately absent

These are answered by the module that already owns them; a second spelling would
be a synonym to keep correct in two places:

| you want | use |
|---|---|
| `IsUUID` | `validate()` from `dyna:uuid` |
| `IsIP` | `parseAddr()` from `dyna:net` |
| `IsURL` | `new URL()` from `dyna:url`, which throws on invalid |
| `IsBase64`, `IsHexadecimal` | the codecs in `dyna:encoding` |

```js
IsEmail("first.last@example.co.uk");   // true
IsEmail("a@b.c");                      // false -- a TLD needs two characters
IsIBAN("GB82 WEST 1234 5698 7654 32"); // true
IsIBAN("GB82WEST12345698765433");      // false -- the check digit fails
IsCreditCard("4242 4242 4242 4242");   // true
```

# config

`import { INI, Env, FrontMatter } from "dyna:config";`

Three configuration grammars. Each is a namespace object rather than a free
function, because a bare `parse` across three grammars is ambiguous.

Every key is written with define semantics, so a file containing `__proto__` —
as a key **or** as a section name — produces an own property and cannot reach
`Object.prototype`.

### `INI.parse(text)`

**Returns** `object`. `[section]` headers (dotted names nest, capped at 16
deep), `key = value`, `;` and `#` comments, CRLF, and the npm-ini conventions:
`key[] = v` appends to an array, and a bare key is `true`. A quoted value keeps
its padding and expands `\n`, `\t`, `\r`; an unquoted value is trimmed and
literal. Only the **first** `=` splits, so a URL with a query string survives.
A malformed line is skipped rather than aborting the parse.

### `Env.parse(text)`

**Returns** `object` of string values. dotenv's grammar: `KEY=value`, an
optional `export ` prefix, `#` comments, and single or double quotes. Escapes
expand **only** inside double quotes, which is dotenv's actual behaviour. This
function is pure — it does not read or mutate the process environment.

### `FrontMatter.split(text)`

**Returns** `{ data, body, lang }`. The fence must be the **first** line:
`---` is `"yaml"`, `+++` is `"toml"`, `;;;` is `"json"`. `data` is the raw
text between the fences — splitting is not parsing — and `body` is everything
after. An **unclosed** fence is not front matter: `data` is `null` and the whole
input is `body`, rather than silently swallowing the document.

```js
INI.parse("[db]\nhost = localhost\nport = 5432\n");   // { db: { host: "localhost", port: "5432" } }
INI.parse("debug\n");                                  // { debug: true }
Env.parse("export PORT=8080 # http\n");                // { PORT: "8080" }
FrontMatter.split("---\ntitle: hi\n---\nbody\n");      // { data: "title: hi", body: "body\n", lang: "yaml" }
```

# log

`import { Logger, Debug } from "dyna:log";`

Leveled, structured logging. `Logger` is a compiled capability: the level, name,
base fields and timestamp mode are parsed once in the constructor and the
per-call path only appends. Lines are JSON, one per line, written to stderr.

### `new Logger(options?)`

| Option | Default | Meaning |
|---|---|---|
| `level` | `"info"` | `trace` `debug` `info` `warn` `error` `fatal` `silent` |
| `name` | — | Emitted as `"name"` on every line. |
| `base` | — | Fields on every line, **pre-serialized once**. |
| `timestamp` | `"epochMs"` | `"epochMs"`, `"iso"` (RFC 3339), or `false` to omit. |

| Member | Signature | Description |
|---|---|---|
| `.trace/.debug/.info/.warn/.error/.fatal` | `(fields?, msg?) → undefined` | Object-first, pino's shape. An `Error` first argument becomes `{type, message, stack}`. |
| `.child(fields)` | `(object) → Logger` | Bound fields, serialized once, not per line. |
| `.enabled(level)` | `(string) → boolean` | Gate an expensive payload explicitly. |
| `.level` | getter/setter | Changeable at runtime, so a service can turn debug on without a restart. |

A call below the level does **no work** — it does not serialize its fields, so
`log.debug(expensive, "…")` costs a compare when debug is off. Fields are
emitted by a purpose-built writer, not by `JSON.stringify`: a **cycle** becomes
`"[Circular]"` and nesting past 8 becomes `"[deep]"` — two different answers,
because a self-reference and a genuinely deep object are different facts. A line
is truncated at 64 KiB.

**Every line is a single `write(2)`.** Nothing is buffered, so no line is lost
to a buffered tail when a process dies, and two writers appending to one
descriptor interleave cleanly rather than mid-line — which buffered stdio does
not guarantee.

### `Debug(namespace)`

**Returns** `function` — the `debug()` shape. Whether it prints is decided
**once**, when `Debug()` is called, by matching `namespace` against the `DEBUG`
environment variable (comma-separated, `*` matches any tail). A namespace that
is off costs a branch.

```js
const log = new Logger({ level: "info", name: "api", base: { pid: 7 } });
log.info({ userId: 42 }, "logged in");
log.error(new Error("boom"), "request failed");
const child = log.child({ requestId: "r1" });
child.warn("slow query");
if (log.enabled("debug")) log.debug({ plan: expensivePlan() }, "query plan");

const d = Debug("app:db");     // prints only when DEBUG matches
d("connected");
```

# url

`import { URL, formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";`

RFC 3986 parsing with the WHATWG component names, and the
`application/x-www-form-urlencoded` codec.

### `new URL(input, base?)`

Getters: `href protocol username password host hostname port pathname search
hash origin`, plus `toString()` and `toJSON()` (both `href`). `protocol` keeps
its colon, `search` its `?`, `hash` its `#`; an absent component reads as `""`.

The scheme is lowercased. A **default port is dropped** (`http` 80, `https` 443,
`ws` 80, `wss` 443, `ftp` 21), which is what makes two spellings of one origin
compare equal. A scheme with no authority (`mailto:`, `data:`) has an opaque
path and `origin` `"null"`. IPv6 literals keep their brackets.

Relative resolution follows RFC 3986 section 5.2, including the abnormal cases:
`..` past the root is dropped rather than allowed to escape above it, an empty
or fragment-only reference inherits the base's query, and `//host` takes its own
authority without inheriting the base's path.

**Refusals**, all `TypeError` unless noted: a relative reference with no base, an
invalid base, a port above 65535 or non-numeric, an unterminated IPv6 literal,
and an input over 64 KiB (`RangeError`). A `URL` is **immutable**: components
are read, and a changed URL is a new one built from a string.

### The form codec

| Function | Signature | Description |
|---|---|---|
| `formDecode(text)` | `(string) → object` | Last value per key. Tolerates a leading `?`. |
| `formEncode(obj)` | `(object) → string` | Own enumerable keys; `undefined` values omitted. |
| `encodeURIComponentStrict(text)` | `(string) → string` | Also escapes `!'()~`, which `encodeURIComponent` leaves alone. |

`+` decodes to a space and a space encodes to `+`. A **malformed escape stays
literal** (`%zz` decodes to `%zz`) rather than being dropped — losing bytes
silently turns a bad request into a different one. Keys are written with define
semantics, so `__proto__=x` produces an own property: the qs prototype-pollution
class dies at the decoder.

```js
const u = new URL("/api?q=1", "https://example.com:443/base/");
u.href;                                  // "https://example.com/api?q=1"
u.origin;                                // "https://example.com"
new URL("../g", "http://a/b/c/d").href;  // "http://a/b/g"
formDecode("a=hello+world&b=%E4%BD%A0"); // { a: "hello world", b: "\u4F60" }
formEncode({ a: "x&y" });                // "a=x%26y"
```

# uuid

`import { v4, v7, v3, v5, parse, validate, version, variant, bytes, fromBytes, NanoID, NanoIDAlphabet, ULID, ULIDTime, NIL, MAX, NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500 } from "dyna:uuid";`

RFC 9562 universally-unique identifiers. Returned UUIDs are canonical lowercase `8-4-4-4-12`.

### Compact identifiers: `NanoID` and `ULID`

Neither is a UUID; both answer the same question with a different trade-off, so
they share this module rather than opening a second one.

| Function | Signature | Description |
|---|---|---|
| `NanoID(size = 21)` | `(number) → string` | URL-safe 64-symbol alphabet; 21 characters carry 126 bits. |
| `NanoIDAlphabet(alphabet, size)` | `(string, number) → string` | Custom **ASCII** alphabet, 2–256 symbols. |
| `ULID(atMillis?)` | `(number) → string` | 26 Crockford base32 chars: 48-bit ms timestamp then 80 random bits. |
| `ULIDTime(ulid)` | `(string) → number` | The millisecond timestamp back out. Validates all 26 symbols. |

Both draw from the OS CSPRNG and map bytes to symbols by **rejection sampling**,
not modulo — a modulo would silently over-produce the first `256 % n` symbols
whenever the alphabet is not a power of two.

A `ULID` is **lexicographically ordered by its timestamp**, which is the reason
to pick it over `v4()`; that only holds because Crockford base32 is sorted by
value. `ULIDTime` accepts either case (Crockford's rule) and rejects `I`, `L`,
`O` and `U`, which the alphabet excludes so a transcription cannot be ambiguous.

```js
NanoID();                    // "V1StGXR8_Z5jdHi6B-myT"  (21 chars)
NanoID(10);                  // 10 chars
NanoIDAlphabet("0123456789abcdef", 8);   // 8 hex characters
ULIDTime(ULID(1469918176385));           // 1469918176385
```

### `v4()`

**Returns** `string` — a random (version 4) UUID from the OS CSPRNG.

### `v7()`

**Returns** `string` — a time-ordered (version 7) UUID: a 48‑bit big-endian Unix‑millisecond
timestamp in the high bits, the rest random. Successive calls are non-decreasing, so v7 ids sort by
creation time — a good database primary key.

### `v3(namespace, name)` · `v5(namespace, name)`

Deterministic name-based UUIDs (`v3` = MD5, `v5` = SHA‑1).

| Parameter | Type | Description |
|---|---|---|
| `namespace` | `string` | A UUID string — typically one of the `NAMESPACE_*` constants. |
| `name` | `string` | The name within the namespace. |

**Returns** `string` — the derived UUID (identical inputs always yield the same UUID).

```js
v5(NAMESPACE_DNS, "www.example.com");
// "2ed6657d-e927-568b-95e1-2665a8aea6a2"
```

### `parse(s)` · `validate(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | A candidate UUID (canonical, `urn:uuid:` prefixed, braced, or 32 raw hex; case-insensitive). |

**Returns** `parse`: `string` — the canonical lowercase form; **throws** `SyntaxError` on a malformed value. `validate`: `boolean` — never throws (non-string → `false`).

### `version(s)` · `variant(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | A UUID string. |

**Returns** `number` — the version (1–8) or variant field.

### `bytes(s)` · `fromBytes(u8)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | (`bytes`) a UUID string. |
| `u8` | `Uint8Array` | (`fromBytes`) exactly 16 bytes. |

**Returns** `bytes`: `Uint8Array` (16 bytes). `fromBytes`: `string` (canonical). **Throws** `RangeError` if `u8` is not 16 bytes.

### Constants

`NIL` (all-zero UUID), `MAX` (all-ones), and the four RFC namespace UUIDs `NAMESPACE_DNS`,
`NAMESPACE_URL`, `NAMESPACE_OID`, `NAMESPACE_X500` — all `string`.

---

# random

`import { Random } from "dyna:random";`

A fast, **seedable** pseudo-random generator (reproducible, unlike `Math.random`).

### `class Random`

**`new Random(seed)`**

| Parameter | Type | Description |
|---|---|---|
| `seed` | `number \| BigInt` | The seed. Equal seeds (as `number` or `BigInt`) produce identical streams. |

**Methods**

| Member | Signature | Description |
|---|---|---|
| `.nextU64()` | `() → BigInt` | The next 64‑bit unsigned integer. |
| `.nextU53()` | `() → number` | The next 53‑bit integer (exactly representable). |
| `.nextFloat()` | `() → number` | A double in `[0, 1)`. |
| `.nextBounded(n)` | `(number) → number` | A uniform integer in `[0, n)` (unbiased). |
| `.fill(buf)` | `(Uint8Array) → Uint8Array` | Fill `buf` with random bytes; returns `buf`. |

`Random` is a **plain garbage-collected object** — no `.close()`, `.closed` or `[Symbol.dispose]`.
The whole native state is one 64‑bit integer, so there is nothing scarce to release early; drop the
reference and the engine reclaims it, exactly like a `Map`.

```js
const rng = new Random(42);
rng.nextFloat();       // 0.0839...  (deterministic for seed 42)
rng.nextBounded(6);    // a fair die index 0..5
```

# mathx

`import { /* real */ gamma, lgamma, erf, erfc, cbrt, hypot, copysign, nextafter, expm1, log1p, log2, logb, scalbn, ilogb, modf, frexp, ldexp, remainder, fmod, isInf, isNaN, signbit, trunc, round, roundToEven, /* MATLAB */ mod, rem, fix, sign, nthroot, nextpow2, pow2, deg2rad, rad2deg, eps, realmin, realmax, flintmax, beta, betaln, gammaln, psi, erfinv, erfcinv, erfcx, expint, factor, primes, nchoosek, rat, linspace, logspace, cumsum, cumprod, diff, /* MATLAB tier B */ gammainc, gammaincinv, betainc, betaincinv, besselj, bessely, besseli, besselk, besseliScaled, besselkScaled, besselh, ellipke, ellipj, legendre, legendreP, polygamma, airy, /* int */ gcd, lcm, factorial, isPrime, abs, bitLen, popcount, /* bit ops */ bits, /* + constants */ } from "dyna:mathx";`

The mathematics `Math` omits: special functions, IEEE‑754 helpers, and exact integer routines. All
real-valued functions take and return `number` unless a tuple is noted.

### Special & elementary functions

| Function | Signature | Description |
|---|---|---|
| `gamma(x)` | `(number) → number` | The gamma function Γ(x). |
| `lgamma(x)` | `(number) → [value, sign]` | log\|Γ(x)\| and the sign of Γ(x) (`±1`). |
| `erf(x)` / `erfc(x)` | `(number) → number` | Error function and its complement. |
| `cbrt(x)` | `(number) → number` | Real cube root. |
| `hypot(x, y)` | `(number, number) → number` | √(x²+y²), computed without overflow. |
| `expm1(x)` / `log1p(x)` | `(number) → number` | eˣ−1 and ln(1+x), accurate near 0. |
| `log2(x)` / `logb(x)` | `(number) → number` | Base‑2 log; the unbiased exponent. |


### Special functions with argument regimes

These are the functions whose accuracy depends on **where** in the argument plane you evaluate them.
Each switches algorithm at a documented threshold, chosen so the two methods overlap — at the
switch point both are within the stated bound, so there is no cliff hiding at the seam. The
worst-case relative error is measured, not asserted, and is stated per regime rather than as a
single number for the function.

| Function | Signature | Description |
|---|---|---|
| `gammainc(x, a, tail?)` | `(number, number, string?) → number` | Regularised incomplete gamma. MATLAB's argument order: the variable first. `tail` is `"lower"` (default) or `"upper"`. |
| `gammaincinv(p, a)` | `(number, number) → number` | The `x` with `gammainc(x, a) === p`. |
| `betainc(x, a, b)` | `(number, number, number) → number` | Regularised incomplete beta `I_x(a,b)`. |
| `betaincinv(p, a, b)` | `(number, number, number) → number` | Its inverse in `x`. |
| `besselj(n, x)` · `bessely(n, x)` | `(int, number) → number` | Bessel functions of the first and second kind. **Integer order only** — a fractional order throws. |
| `besseli(nu, x)` · `besselk(nu, x)` | `(number, number) → number` | Modified Bessel functions. Real order. |
| `besseliScaled(nu, x)` · `besselkScaled(nu, x)` | `(number, number) → number` | `I·e^-x` and `K·e^x`. |
| `besselh(n, x, kind?)` | `(int, number, 1\|2) → [re, im]` | Hankel: `J ± iY`. `kind` defaults to 1. |
| `ellipke(m)` | `(number) → [K, E]` | Complete elliptic integrals, in MATLAB's **parameter** convention `m = k²`. Both, because they share one AGM iteration. |
| `ellipj(u, m)` | `(number, number) → {sn, cn, dn}` | Jacobi elliptic functions, `0 ≤ m ≤ 1`. |
| `legendre(n, x)` | `(int, number) → number[]` | The whole `m = 0..n` column, as MATLAB returns it. |
| `legendreP(n, m, x)` | `(int, int, number) → number` | One associated Legendre value, **with** the Condon–Shortley phase. |
| `polygamma(n, x)` | `(int, number) → number` | The n-th derivative of the digamma. `polygamma(0, x)` is `psi(x)` — the same implementation, not a second one. |
| `airy(x)` | `(number) → {ai, aip, bi, bip}` | All four Airy values, because one evaluation produces them all. |

**Measured worst-case relative error.** The oracle is a set of identities — Wronskians, three-term
recurrences, reflection and duplication rules, closed forms at integer parameters — rather than a
table of digits, because an identity cannot be misremembered in a way that accidentally passes.

| Function | Regime | Worst relative error |
|---|---|---|
| `gammainc` | against the closed forms `P(1,x)` and `P(1/2,x)` | 5.7e-16 |
| `gammainc` upper tail | far tail, where `1 − P` has no relative accuracy at all | 2.9e-14 |
| `gammaincinv` | round trip | 5.8e-14 |
| `betainc` | vs the binomial-tail closed form at integer parameters | 6.5e-15 |
| `betaincinv` | round trip | 1.2e-14 |
| `besseli` / `besselk` | `x < 0.5` (series) | 2.3e-15 |
| `besseli` / `besselk` | `0.5 ≤ x < 20` (quadrature) | 1.8e-15 |
| `besseli` / `besselk` | `x ≥ 20` (asymptotic) | 1.4e-14 |
| `ellipke` | Legendre's relation, which constrains K and E jointly | 9.9e-16 |
| `ellipj` | both defining identities | 2.2e-16 |
| `legendre` | closed forms and the degree recurrence | 9.4e-15 |
| `polygamma` | the exact recurrence | 4.7e-15 |
| `airy` | `x ≥ 0.1` (via Bessel) | 6.1e-15 |
| `airy` | `−7 ≤ x < 0.1` (Maclaurin) | **4.7e-12** |
| `airy` | `x < −7` (oscillatory asymptotic) | 1.8e-14 |

**Airy's weak regime is the series, not the transition band** — which is the opposite of what one
would guess. Approaching `x = −7` the Maclaurin series cancels terms of size `e^ζ` against a result
of order 1, and by the switch point that has cost about four digits; the asymptotic side has
already reached 1e-14 by then. The switch sits where the two curves cross.

`besseli(nu, x)` overflows past `x ≈ 713` and `besselk(nu, x)` underflows to zero past `x ≈ 745`,
exactly as a double must. The scaled forms are finite there and still satisfy the Wronskian, so any
computation that needs the pair far out should use them.

### IEEE‑754 manipulation

| Function | Signature | Description |
|---|---|---|
| `copysign(x, y)` | `(number, number) → number` | \|x\| with the sign of y. |
| `nextafter(x, y)` | `(number, number) → number` | The next representable double after x toward y. |
| `scalbn(x, n)` / `ldexp(x, n)` | `(number, number) → number` | x·2ⁿ. |
| `ilogb(x)` | `(number) → number` | Unbiased exponent. Returns the platform `FP_ILOGB0` for 0 and `FP_ILOGBNAN` for NaN (`-2147483648` / `2147483647` here), not a throw. |
| `modf(x)` | `(number) → [intPart, fracPart]` | Split into integer and fractional parts. |
| `frexp(x)` | `(number) → [frac, exp]` | x = frac·2^exp with frac in [0.5, 1). |
| `remainder(x, y)` / `fmod(x, y)` | `(number, number) → number` | IEEE remainder; C `fmod`. |
| `trunc(x)` / `round(x)` / `roundToEven(x)` | `(number) → number` | Toward zero; away-from-zero (ties); banker's rounding. |
| `isInf(x, sign?)` / `isNaN(x)` / `signbit(x)` | `→ boolean` | Classification. `isInf`'s optional `sign` (`>0`, `<0`) tests a specific infinity. |

### Exact integer routines

| Function | Signature | Description |
|---|---|---|
| `gcd(a, b)` | `(number\|BigInt, number\|BigInt) → BigInt` | Greatest common divisor. |
| `lcm(a, b)` | `(number\|BigInt, number\|BigInt) → BigInt` | Least common multiple. |
| `factorial(n)` | `(number) → BigInt` | n! computed exactly (arbitrary precision). **Throws `RangeError` for n > 10000**, a deliberate bound on worst-case work. |
| `isPrime(n)` | `(number\|BigInt) → boolean` | Deterministic Miller–Rabin, exact for all uint64. |
| `abs(n)` | `(BigInt) → BigInt` | Absolute value. |
| `bitLen(n)` | `(BigInt) → number` | Number of bits in \|n\|. |
| `popcount(n)` | `(BigInt) → number` | Number of set bits. |

```js
factorial(25);          // 15511210043330985984000000n
isPrime(1000000007n);   // true
gcd(462n, 1071n);       // 21n
```

### MATLAB-parity elementary math

| Function | Signature | Description |
|---|---|---|
| `mod(x, y)` | `(number, number) → number` | **Floored** modulo — MATLAB's. `mod(-1, 3)` is `2`. |
| `rem(x, y)` | `(number, number) → number` | **Truncated** remainder — C's `fmod`. `rem(-1, 3)` is `-1`. |
| `fix(x)` | `(number) → number` | Round toward zero (alias of `trunc`). |
| `sign(x)` | `(number) → number` | `-1`, `0`, `1`; preserves `-0` and `NaN`. |
| `nthroot(x, n)` | `(number, number) → number` | Real n-th root; defined for a negative `x` with odd `n`, where `x ** (1/n)` is `NaN`. |
| `nextpow2(x)` | `(number) → number` | Smallest `p` with `2**p >= abs(x)`. |
| `pow2(x)` | `(number) → number` | `2**x`. |
| `deg2rad(x)` / `rad2deg(x)` | `(number) → number` | Angle conversion. |
| `eps(x?)` | `(number?) → number` | Distance to the next representable double away from zero; `eps()` is `eps(1)`. Correct for subnormals. |
| `realmin()` / `realmax()` / `flintmax()` | `() → number` | Smallest normal, largest finite, `2**53`. |

**Sign convention.** `mod` takes the sign of the divisor, `rem` the sign of the dividend. They agree
for like signs and differ for mixed ones.

### Special functions

| Function | Signature | Description |
|---|---|---|
| `beta(a, b)` / `betaln(a, b)` | `(number, number) → number` | Beta function and its log. `betaln` stays finite where `beta` overflows. |
| `gammaln(x)` | `(number) → number` | `log|Γ(x)|`; the scalar form of `lgamma`. |
| `psi(x)` | `(number) → number` | Digamma. `NaN` at a non-positive integer pole. Accurate to ~2e-14. |
| `erfinv(y)` / `erfcinv(y)` | `(number) → number` | Inverse error function and its complement. `±Infinity` at `±1`, `NaN` outside `[-1, 1]`. |
| `erfcx(x)` | `(number) → number` | `exp(x²)·erfc(x)`; stays finite where `erfc` underflows to `0`. |
| `expint(x)` | `(number) → number` | E₁(x) for `x > 0`; `Infinity` at `0`, `NaN` for `x < 0`. |

### Discrete math

| Function | Signature | Description |
|---|---|---|
| `factor(n)` | `(number) → number[]` | Ascending prime factors with multiplicity. **Throws** `RangeError` unless `n` is an integer in `[1, 2⁵³]`. |
| `primes(n)` | `(number) → number[]` | Every prime `≤ n`, by sieve. **Throws** `RangeError` past 5e7. |
| `idivide(a, b, mode?)` | `(number, number, string?) → number` | Integer division with an explicit rounding mode: `"fix"` (toward zero, the default and C's `/`), `"floor"`, `"ceil"`, `"round"`. Division by zero is IEEE, not a throw. |
| `perms(v)` | `(number[]) → number[][]` | Every permutation, in MATLAB's **reverse lexicographic** order. At most 8 elements. |
| `nchoosek(n, k)` | `(number, number) → number` | Binomial coefficient, computed multiplicatively so it stays exact past `2³²`. `0` when `k > n`. |
| `rat(x, tol?)` | `(number, number?) → [num, den]` | Rational approximation by continued fractions; `tol` defaults to `1e-6`. Denominator is positive. |

### Vectors

| Function | Signature | Description |
|---|---|---|
| `linspace(a, b, n?)` | `(number, number, number?) → number[]` | `n` points inclusive of both ends (default 100). The final point is **exactly** `b`, not accumulated. |
| `logspace(a, b, n?)` | `(number, number, number?) → number[]` | `10**linspace(a, b, n)`. |
| `cumsum(v)` / `cumprod(v)` | `(number[] \| TypedArray) → number[]` | Running sum / product. |
| `diff(v)` | `(number[] \| TypedArray) → number[]` | Consecutive differences; length `n-1`. |

### `bits` — fixed-width bit operations

`import { bits } from "dyna:mathx";`

Fixed-width bit manipulation at widths 8, 16, 32 and 64, as one namespace object. **The 8/16/32
forms use `number`; the 64-bit forms take and return `BigInt`.** The counting functions always
return a `number`.

| Group | Members |
|---|---|
| Counting | `leadingZeros{8,16,32,64}` `trailingZeros{8,16,32,64}` `onesCount{8,16,32,64}` `len{8,16,32,64}` |
| Bit order | `reverse{8,16,32,64}` `reverseBytes{16,32,64}` `rotateLeft{8,16,32,64}` |
| Multi-precision | `add{32,64}` `sub{32,64}` `mul{32,64}` `div{32,64}` `rem{32,64}` |
| Constant | `uintSize` (`64`) |

`leadingZeros(0)` and `trailingZeros(0)` are the width; `len(0)` is `0`. `rotateLeft` reduces its
count modulo the width, so a negative count rotates right. `div`/`rem` **throw** `RangeError` on a
zero divisor, and `div` also on a quotient that would not fit the width.

```js
bits.onesCount32(255);      // 8
bits.rotateLeft8(1, -1);    // 128
bits.mul64(0xffffffffffffffffn, 2n);   // [1n, 18446744073709551614n]
```

### Constants

`E`, `Pi`, `Phi`, `Sqrt2`, `SqrtE`, `SqrtPi`, `Ln2`, `Log2E`, `Ln10`, `Log10E` (numbers);
`MaxInt32`, `MinInt32`, `MaxSafeInteger` (numbers); `MaxInt64` (BigInt).

---

### `class Expression`

A compiled arithmetic expression: shunting-yard to an RPN program over doubles,
evaluated with no `eval` and no scope.

**`new Expression(text)`** · **`.eval(vars?)` → number** · **`.variables()` → string[]**

An identifier is either a variable or one of the functions below; nothing
reaches an object, a prototype or a scope, and that is the reason to use this
for a config-driven formula rather than an evaluator.

`+ - * / % ^`, unary `-`, parentheses, and a comma between a two-argument
function's arguments. **`^` is right-associative and unary minus binds below
it**, so `2^3^2` is 512 and `-x^2` is `-(x^2)` — the two places a naive
shunting yard silently computes something else.

One argument: `sin cos tan asin acos atan sinh cosh tanh sqrt cbrt exp log
log2 log10 abs floor ceil round trunc sign expm1 log1p`. Two: `atan2 pow hypot
min max fmod`. Constants `pi` and `e`.

`.eval(vars)` reads each variable as an **own data property** — a getter is
refused, because running code is the one thing this exists to avoid — and a
missing or non-numeric value names itself in the error. Source is capped at
4096 bytes and nesting at 256.

```js
const area = new Expression("pi * r^2");
area.variables();              // [ "r" ]
area.eval({ r: 2 });           // 12.566370614359172
new Expression("2^3^2").eval();   // 512, not 64
new Expression("-x^2").eval({ x: 3 });   // -9, not 9
```

# structures

`import { Heap, List, BitSet, UnionFind, Deque, Fenwick, SegTree, RingBuffer, BloomFilter, Trie,
LRU, SortedSet, SortedMap, Multiset, Multimap, BiMap, Table, RangeSet, RangeMap, IntervalTree,
MinMaxHeap, CountMinSketch, HyperLogLog, Graph } from "dyna:structures";`

Data structures JavaScript has **no builtin** for. `Array`, `Map`, `Set` and the TypedArrays are
engine intrinsics (already native C) and are deliberately not reimplemented here — this module ships
only what the language lacks.

**Iteration.** `BitSet`, `Deque`, `List`, `RingBuffer`, `SortedSet`, `Trie`, `Multiset`,
`Multimap`, `BiMap`, `Table`, `RangeSet` and `RangeMap` implement `[Symbol.iterator]`, so they
spread, destructure, feed `Array.from`, and compose with the whole `Iterator` helper surface.
Iteration is a **snapshot**: the loop sees the container as it was at entry, so mutating during a
`for...of` is well defined and costs an O(n) copy per loop.

`BloomFilter`, `CountMinSketch` and `HyperLogLog` cannot enumerate their members — that is what
makes them sketches. `Heap` and `MinMaxHeap` have no non-destructive order, `UnionFind` is a range
of integers you query with `find()`, `Fenwick` and `SegTree` expose folds rather than slots, and
`LRU` and `SortedMap` have no element projection to snapshot. Those are not iterable.

**These are plain garbage-collected objects — exactly like `Map` and `Set`.** There is **no**
`.close()`, `.closed`, or `[Symbol.dispose]`: nothing to manage. Create one, use it, and the GC
reclaims its native backing when it becomes unreachable — including any values a container holds and
even reference cycles through it (the value-holding classes implement `gc_mark`, so the cycle
collector traces them like a native `Map` would).

### `class Heap`

A binary heap / priority queue, ordered by a comparator or by natural numeric order.

**`new Heap([compare])`**

| Parameter | Type | Description |
|---|---|---|
| `compare` | `(a, b) => number` | Optional. Returns `<0` if `a` should come out before `b`. `(a,b)=>a-b` is a min-heap; `(a,b)=>b-a` a max-heap. Omit it for a numeric min-heap compared in C, which is several times faster because no JavaScript runs per comparison. |

**Throws** `TypeError` if `compare` is given and is not a function, and — for a
heap built without one — if a pushed value is not a number. Such a value is
refused rather than coerced: `valueOf` is never called.

| Member | Signature | Description |
|---|---|---|
| `.push(value)` | `(any) → void` | Insert a value. |
| `.pop()` | `() → any` | Remove and return the highest-priority element (`undefined` if empty). |
| `.peek()` | `() → any` | Return, without removing, the highest-priority element. |
| `.size` | `number` (getter) | The element count. |

The comparator is invoked during `push`/`pop`; it may run arbitrary JavaScript safely (a comparator
that reenters or mutates the heap mid-operation is rejected with a clean throw, never corruption).

```js
const pq = new Heap();                                // numeric min-heap, native compare
[5,1,4,2,8].forEach(v => pq.push(v));
const out = []; while (pq.size) out.push(pq.pop());   // [1,2,4,5,8]

const maxq = new Heap((a, b) => b - a);               // any other order needs a comparator
```

### `class List`

A doubly-linked list, i.e. a deque with O(1) operations at both ends.

**`new List()`**

| Member | Signature | Description |
|---|---|---|
| `.pushFront(value)` / `.pushBack(value)` | `(any) → void` | Insert at the front/back. |
| `.popFront()` / `.popBack()` | `() → any` | Remove and return from the front/back (`undefined` if empty). |
| `.front()` / `.back()` | `() → any` | Peek at the front/back. |
| `.toArray()` | `() → any[]` | A front-to-back snapshot array. |
| `[Symbol.iterator]()` | | Iterate front to back (`for...of`). |
| `.length` | `number` (getter) | Element count. |

### `class BitSet`

Dynamic bit set backed by 64-bit words; word-parallel set algebra and popcount.

| Member | Signature | Description |
|---|---|---|
| `new BitSet(nbits?)` | `(number?)` | Empty set; optional initial bit capacity. |
| `.set(i)` / `.clear(i)` / `.flip(i)` | `(number) → this` | Set / clear / toggle bit `i` (grows on demand). |
| `.get(i)` | `(number) → boolean` | Whether bit `i` is set. |
| `.nextSet(from)` | `(number) → number` | Index of the first set bit `≥ from`, or `-1`. |
| `.and(o)` / `.or(o)` / `.xor(o)` | `(BitSet) → this` | In-place word-parallel set algebra with another `BitSet`. |
| `.count` | `number` (getter) | Number of set bits (popcount). |
| `.toArray()` | `() → number[]` | Ascending indices of the set bits. |

### `class UnionFind`

Disjoint-set forest over elements `0..n-1` (path halving + union by rank).

| Member | Signature | Description |
|---|---|---|
| `new UnionFind(n)` | `(number)` | `n` singleton sets. |
| `.find(x)` | `(number) → number` | Representative of `x`'s set. |
| `.union(x, y)` | `(number, number) → boolean` | Merge; `true` if it joined two distinct sets. |
| `.connected(x, y)` | `(number, number) → boolean` | Whether `x` and `y` share a set. |
| `.count` | `number` (getter) | Number of disjoint components. |
| `.size` | `number` (getter) | Element count `n`. |

### `class Deque`

Double-ended queue — **O(1) at both ends** (unlike `Array`, whose `shift`/`unshift` are O(n)).

| Member | Signature | Description |
|---|---|---|
| `new Deque()` | | Empty deque. |
| `.pushBack(v)` / `.pushFront(v)` | `(any) → number` | Append / prepend; returns the new length. |
| `.popBack()` / `.popFront()` | `() → any` | Remove and return an end element, or `undefined` if empty. |
| `.peekBack()` / `.peekFront()` | `() → any` | Inspect an end element without removing. |
| `.get(i)` | `(number) → any` | Element `i` from the front, or `undefined`. |
| `.length` | `number` (getter) | Element count. |
| `.toArray()` | `() → any[]` | Fresh Array, front→back. |

### `class Fenwick`

Binary Indexed Tree over a fixed vector of doubles: O(log n) point update + prefix/range sum.

| Member | Signature | Description |
|---|---|---|
| `new Fenwick(n)` | `(number)` | `n` zeroed slots (positions `0..n-1`). |
| `.update(i, delta)` | `(number, number) → this` | Add `delta` at position `i`. |
| `.prefixSum(i)` | `(number) → number` | Sum of positions `[0..i]` inclusive. |
| `.rangeQuery(lo, hi)` | `(number, number) → number` | Sum of `[lo..hi]` inclusive (`0` if `lo > hi`). |
| `.size` | `number` (getter) | Slot count `n`. |

### `class SegTree`

Segment tree over a fixed vector of doubles: O(log n) point update + range fold.

| Member | Signature | Description |
|---|---|---|
| `new SegTree(n, op?)` | `(number, "sum"\|"min"\|"max")` | `n` identity slots; `op` defaults to `"sum"`. |
| `.update(i, value)` | `(number, number) → this` | Assign position `i`. |
| `.rangeQuery(lo, hi)` | `(number, number) → number` | Fold `op` over `[lo..hi]` (identity if `lo > hi`). |
| `.size` | `number` (getter) | Slot count `n`. |

### `class RingBuffer`

Fixed-capacity circular buffer; `push` overwrites the oldest element when full.

| Member | Signature | Description |
|---|---|---|
| `new RingBuffer(capacity)` | `(number)` | Fixed capacity (`> 0`). |
| `.push(v)` | `(any) → number` | Append; evicts the oldest if full. Returns the live count. |
| `.get(i)` | `(number) → any` | Element `i` from oldest, or `undefined`. |
| `.toArray()` | `() → any[]` | Oldest→newest. |
| `.length` / `.capacity` / `.full` | getters | Count / capacity / whether at capacity. |

### `class BloomFilter`

Probabilistic set membership over string keys. **No false negatives**; false positives bounded by `(bits, hashes)`.

| Member | Signature | Description |
|---|---|---|
| `new BloomFilter(bits, hashes?)` | `(number, number?)` | Bit array size; `hashes` defaults to 3 (capped at 32). |
| `.add(key)` | `(string) → this` | Insert a key. |
| `.mayContain(key)` | `(string) → boolean` | `false` = definitely absent; `true` = possibly present. |
| `.bits` / `.hashes` | getters | Parameters. |

### `class Trie`

Set of string keys with prefix queries (teardown/queries are iterative — no C-stack recursion).

| Member | Signature | Description |
|---|---|---|
| `new Trie()` | | Empty trie. |
| `.insert(key)` / `.has(key)` / `.delete(key)` | `(string) → this`/`boolean`/`boolean` | Membership. |
| `.keysWithPrefix(prefix)` | `(string) → string[]` | All stored keys starting with `prefix` (order unspecified). |
| `.longestPrefix(str)` | `(string) → string` | Longest stored key that prefixes `str`, or `""`. |
| `.size` | `number` (getter) | Key count. |

### `class LRU`

Capacity-bounded string→value cache with least-recently-used eviction.

| Member | Signature | Description |
|---|---|---|
| `new LRU(capacity, opts?)` | `(number, {ttlMs?, onEvict?})` | Max entries (`> 0`). `ttlMs` expires every entry; `onEvict(key, value)` fires when the cache drops one. |
| `.get(key)` | `(string) → any` | Value (marks MRU), or `undefined`. |
| `.put(key, value)` / `.set(...)` | `(string, any) → this` | Insert/update; evicts the LRU entry past capacity. |
| `.setWithTTL(key, value, ms)` | `(string, any, number) → this` | Insert/update with an expiry for this entry only. |
| `.has(key)` | `(string) → boolean` | Membership (does not change recency). An expired entry is absent. |
| `.delete(key)` | `(string) → boolean` | Remove. |
| `.purgeExpired()` | `() → number` | Drop expired entries, returning how many. |
| `.size` / `.capacity` | getters | Entry count / max. |
| `.stats` | getter | `{hits, misses, evictions, expired, size, capacity}`. |

Expiry is **lazy**: an expired entry is never returned, whether or not anything
else touches the cache, and the clock is monotonic so an NTP step backwards
cannot resurrect one. There is no background sweep, so a key written once and
never read again holds its memory until `purgeExpired()` — that call is about
memory, not correctness.

`evictions` and `expired` count different things: dropped for **capacity**
versus dropped for **age**. Conflating them hides which one to tune.

`onEvict` runs *after* the entry is already out of the cache, so it always sees
a consistent structure — and modifying that cache from inside the callback is
refused rather than silently corrupting the walk it interrupted.

```js
const c = new LRU(2, { ttlMs: 60000 });
c.put("a", 1); c.put("b", 2); c.put("c", 3);   // "a" is evicted
c.has("a");                                     // false
c.stats.evictions;                              // 1
c.setWithTTL("d", 4, 50);                       // this entry only, 50 ms
```

### `class SortedSet` / `class SortedMap`

Ordered collections of numeric keys (skiplist-backed): O(log n) add/lookup + floor/ceil/range.

| `SortedSet` | `SortedMap` | Description |
|---|---|---|
| `.add(x) → this` | `.set(k, v) → this` | Insert (map updates value); `NaN` key throws. |
| `.has(x)` | `.get(k)` / `.has(k)` | Lookup. |
| `.delete(x)` | `.delete(k)` | Remove; returns whether present. |
| `.first()` / `.last()` | `.firstKey()` / `.lastKey()` | Extremes, or `undefined`. |
| `.floor(x)` / `.ceil(x)` | `.floorKey(x)` / `.ceilKey(x)` | Largest `≤ x` / smallest `≥ x`. |
| `.rangeQuery(lo, hi) → number[]` | `.rangeQuery(lo, hi) → [key,val][]` | Keys/entries in `[lo, hi]`, ascending. |
| `.toArray()` | `.keys()` | Ascending keys. |
| `.size` | `.size` | Element count. |

### `class BTree`

An ordered map on numeric keys, B+ shaped: values live in the leaves and the leaves are linked, so
a range scan and the iterator walk memory in order rather than descending per element. Same surface
as `SortedMap`, and measurably faster on every operation that touches more than one level — a
skiplist chases one dependent pointer per level, a B+tree reads a whole node of keys per level.

Measured at 100,000 scattered keys: `get` 1.94x, `set` 2.12x, `floorKey` 1.93x, building from empty
2.35x, a full ordered scan 1.64x. A lookup for a key beyond the largest is parity — a skiplist
rejects that at its top level in one step.

**`new BTree()`**

| Member | Signature | Description |
|---|---|---|
| `.set(k, v)` | `→ this` | Insert or replace. A `NaN` key throws — it has no position in an ordered structure. |
| `.get(k)` | `→ any` | `undefined` if absent. |
| `.has(k)` | `→ boolean` | |
| `.delete(k)` | `→ boolean` | Whether the key was present. |
| `.firstKey()` / `.lastKey()` | `→ number` | Extremes, or `undefined` when empty. |
| `.floorKey(x)` / `.ceilKey(x)` | `→ number` | Largest `≤ x` / smallest `≥ x`, or `undefined`. |
| `.rangeQuery(lo, hi)` | `→ [key, val][]` | Entries in `[lo, hi]`, both ends inclusive, ascending. |
| `.keys()` | `→ number[]` | Ascending keys. |
| `.size` | `number` (getter) | Entry count. |

`[Symbol.iterator]` yields `[key, value]` in ascending key order.

```js
const rows = [[101, "ann"], [150, "bo"], [220, "cy"]];
const idx = new BTree();
for (const [id, name] of rows) idx.set(id, name);
idx.rangeQuery(100, 200);      // [[101, "ann"], [150, "bo"]]
idx.floorKey(150);             // 150 -- the largest id at or below 150
idx.ceilKey(151);              // 220
```

Deletion removes the key and leaves the node under-full rather than rebalancing, so a
delete-heavy workload keeps the space its peak size demanded and never touches the parent chain.

## Byte-string keys

`Multiset`, `Multimap`, `BiMap`, `Table`, `CountMinSketch` and `HyperLogLog` take **byte-string**
keys, like `Trie`, `LRU` and `BloomFilter` before them. A key is compared byte for byte and never
interpreted, so an embedded NUL is an ordinary key byte. A non-string argument is converted with
`String()`, so `1` and `"1"` are the same key.

### `class Multiset`

Guava's `Multiset` / Commons' `Bag`: a set that counts. `size` is the number of **distinct**
elements; `totalSize` is the sum of the counts.

**`new Multiset()`**

| Member | Signature | Description |
|---|---|---|
| `.add(key, n = 1)` | `→ number` | Adds `n`; returns the **new** count. `RangeError` if `n < 0`. |
| `.remove(key, n = 1)` | `→ number` | Subtracts `n`, clamped at 0; returns the new count. A count that reaches 0 removes the key. |
| `.count(key)` | `→ number` | `0` for an absent key. |
| `.has(key)` | `→ boolean` | `count > 0`. |
| `.setCount(key, n)` | `→ this` | Sets the count outright; `0` removes. |
| `.delete(key)` | `→ boolean` | Removes every copy; returns whether it was present. |
| `.clear()` | `→ undefined` | |
| `.elementSet()` | `→ string[]` | Distinct keys. |
| `.entrySet()` | `→ [key, count][]` | What `[Symbol.iterator]` yields. |
| `.size` / `.totalSize` | `number` | Distinct keys / total copies. |

```js
const text = "a b a c a b";
const words = new Multiset();
for (const w of text.split(/\s+/)) words.add(w);
words.entrySet().sort((a, b) => b[1] - a[1]).slice(0, 10);   // top ten
```

### `class Multimap`

Guava's `Multimap`, list flavour: one key, many values, **duplicates kept**, insertion order
preserved. Values are arbitrary JS values. A key whose last value is removed disappears.

**`new Multimap()`**

| Member | Signature | Description |
|---|---|---|
| `.put(key, value)` | `→ this` | Appends. |
| `.get(key)` | `→ any[]` | A fresh array; `[]` for an absent key. |
| `.count(key)` | `→ number` | Values under `key`. |
| `.removeAt(key, i)` | `→ any` | Removes and returns the `i`th value, preserving the order of the rest; `undefined` if out of range. |
| `.delete(key)` | `→ number` | Removes the key; returns how many values went. |
| `.keys()` | `→ string[]` | Distinct keys. |
| `.entries()` | `→ [key, value][]` | **One pair per value.** What `[Symbol.iterator]` yields. |
| `.size` / `.keyCount` | `number` | Total values / distinct keys. |

### `class BiMap`

Guava's `BiMap`: a string↔string map unique in **both** directions, with an O(1) inverse lookup.

**`new BiMap()`**

| Member | Signature | Description |
|---|---|---|
| `.set(key, value)` | `→ this` | **`TypeError` if `value` is already bound to a different key.** Rebinding an existing key is fine and frees its old value. Setting a pair that already holds is a no-op. |
| `.forceSet(key, value)` | `→ this` | Guava's `forcePut`: evicts the pair that owned `value`. |
| `.get(key)` / `.keyOf(value)` | `→ string?` | The two directions; `undefined` if absent. |
| `.has(key)` / `.hasValue(value)` | `→ boolean` | |
| `.delete(key)` / `.deleteValue(value)` | `→ boolean` | |
| `.entries()` / `.inverseEntries()` | `→ [string, string][]` | `entries()` is what `[Symbol.iterator]` yields. |
| `.clear()` | `→ undefined` | |
| `.size` | `number` | |

The refusal is the point: `set` throwing is what makes the inverse a function rather than a
best-effort index. Use `forceSet` when you mean to rebind.

### `class Table`

Guava's `Table`: a sparse `(row, column) → value` store. The key is a **length-prefixed**
`row‖column`, so `("ab","c")` and `("a","bc")` are different cells.

**`new Table()`**

| Member | Signature | Description |
|---|---|---|
| `.put(row, col, value)` | `→ this` | Insert or replace. |
| `.get(row, col)` | `→ any` | `undefined` if absent. |
| `.has(row, col)` | `→ boolean` | |
| `.delete(row, col)` | `→ boolean` | |
| `.row(r)` | `→ [col, value][]` | |
| `.column(c)` | `→ [row, value][]` | |
| `.cells()` | `→ [row, col, value][]` | What `[Symbol.iterator]` yields. |
| `.size` | `number` | Occupied cells. |

`get`/`put`/`delete` are O(1). `row()` and `column()` are **O(size)** scans: the storage is a sparse
cell list, and two extra indices would be paid for by every write to serve a projection most callers
never ask for.

### `class RangeSet`

A **coalescing** set of half-open numeric intervals `[lo, hi)`. Adjacent and overlapping spans merge,
so the state is always the minimal list of disjoint spans covering the same points.

**`new RangeSet()`**

| Member | Signature | Description |
|---|---|---|
| `.add(lo, hi)` / `.remove(lo, hi)` | `→ this` | An empty or inverted interval (`hi <= lo`) is a **no-op**, not an error. |
| `.contains(x)` | `→ boolean` | `lo` is in, `hi` is out. |
| `.encloses(lo, hi)` | `→ boolean` | Is all of `[lo, hi)` covered? |
| `.intersects(lo, hi)` | `→ boolean` | Any overlap at all? |
| `.ranges()` | `→ [lo, hi][]` | Ascending, disjoint, non-adjacent. What `[Symbol.iterator]` yields. |
| `.complement(lo, hi)` | `→ [lo, hi][]` | The gaps within `[lo, hi)`. |
| `.clear()` | `→ undefined` | |
| `.size` / `.measure` | `number` | Number of spans / total covered length. |

`±Infinity` are legitimate bounds. **`NaN` throws `RangeError`** — a NaN bound makes every
comparison false and would silently store an interval containing nothing.

### `class RangeMap`

Guava's `RangeMap`: `[lo, hi) → value`, kept disjoint but **not** coalesced. The newest `put` wins;
overlapping parts of existing spans are trimmed, and a span the new range falls strictly inside is
split in two around it.

**`new RangeMap()`**

| Member | Signature | Description |
|---|---|---|
| `.put(lo, hi, value)` | `→ this` | |
| `.get(x)` | `→ any` | `undefined` outside every span. |
| `.remove(lo, hi)` | `→ this` | |
| `.entries()` | `→ [lo, hi, value][]` | Ascending. What `[Symbol.iterator]` yields. |
| `.size` | `number` | Spans. |

```js
const tiers = new RangeMap();
tiers.put(0, 100, "free").put(100, 1000, "pro").put(1000, Infinity, "enterprise");
tiers.get(250);                                   // "pro"
tiers.put(500, 600, "promo");                     // splits "pro" into two spans
```

Equal adjacent values stay separate spans — `RangeMap` deliberately is not a `RangeSet`.

### `class IntervalTree`

Closed intervals `[lo, hi]` with a payload, answering **which intervals overlap** — the question a
`RangeSet` destroys by merging. Overlapping intervals are kept as distinct entries.

**`new IntervalTree()`**

| Member | Signature | Description |
|---|---|---|
| `.insert(lo, hi, value)` | `→ this` | `hi < lo` is clamped to `[lo, lo]`. |
| `.overlapping(lo, hi)` | `→ [lo, hi, value][]` | Every interval intersecting `[lo, hi]`, endpoints included. |
| `.at(x)` | `→ [lo, hi, value][]` | The degenerate query `[x, x]`. |
| `.size` | `number` | |

The index is a sorted array plus a max-endpoint segment tree, rebuilt lazily on the first query
after a mutation — so a bulk load pays one sort, not one per insert, and a query prunes whole
subtrees whose maximum endpoint is below the query.

### `class MinMaxHeap`

A double-ended priority queue: O(log n) at **both** ends, which `Heap` cannot do. Priority is a
**number**, not a comparator — that keeps the sift loop entirely inside C (a JS comparator is ~97%
of `Heap`'s cost) and makes the class reentrancy-safe by construction.

**`new MinMaxHeap()`**

| Member | Signature | Description |
|---|---|---|
| `.push(priority, value = priority)` | `→ this` | `RangeError` if `priority` is `NaN`. |
| `.popMin()` / `.popMax()` | `→ any` | Removes and returns the payload; `undefined` when empty. |
| `.peekMin()` / `.peekMax()` | `→ any` | Without removing. |
| `.size` | `number` | |

```js
const samples = [{ latencyMs: 4 }, { latencyMs: 91 }, { latencyMs: 12 }];
const window = new MinMaxHeap();
for (const s of samples) {
  window.push(s.latencyMs, s);
  if (window.size > 1000) window.popMax();      // keep the fastest 1000
}
```

### `class CountMinSketch`

Frequency estimation in fixed space. It **never under-counts**: the answer is exact or too high, and
the error is bounded by `totalCount / width` with probability `1 - 2^-depth`.

**`new CountMinSketch(width, depth = 5)`** — `RangeError` if either is `0`, if `depth > 64`, or if
`width * depth` exceeds 2^28 counters.

| Member | Signature | Description |
|---|---|---|
| `.add(key, n = 1)` | `→ this` | `RangeError` if `n < 0`: a sketch cannot be decremented. |
| `.count(key)` | `→ number` | The minimum over the rows. |
| `.merge(other)` | `→ this` | Counter-wise sum; identical to having added both streams into one sketch. `TypeError` on a dimension mismatch or on merging a sketch into itself. |
| `.width` / `.depth` / `.totalCount` | `number` | `totalCount` is exact. |

### `class HyperLogLog`

Distinct-count estimation in `2^precision` bytes. The default `precision` of 14 uses 16 KiB for a
standard error of `1.04 / sqrt(2^14)` = **0.81%**, independent of the cardinality.

**`new HyperLogLog(precision = 14)`** — `RangeError` outside 4…18.

| Member | Signature | Description |
|---|---|---|
| `.add(key)` | `→ this` | Adding a key already seen is exactly idempotent. |
| `.count()` | `→ number` | The estimate. Small cardinalities use linear counting, where the raw estimator is badly biased. |
| `.merge(other)` | `→ this` | Register-wise max — the estimate of the **union**. `TypeError` on a precision mismatch. |
| `.precision` / `.registers` | `number` | |

Merging is why it is worth using: per-shard sketches combine into a global distinct count without
ever shipping the keys.

### `class Graph`

A native graph with traversal / shortest‑path / spanning‑tree algorithms **as methods** (one JS→C
transition + a C loop, not N interpreted steps). Nodes are integer ids `0..n-1` (`addNode` returns
the next id; `addEdge` auto‑grows to include its endpoints). Every result is a fresh JS
array/object.

| Member | Signature | Description |
|---|---|---|
| `new Graph({directed?, weighted?})` | | Empty graph (both default `false`). |
| `.addNode()` | `() → number` | Append an isolated node; returns its id. |
| `.addEdge(u, v, w?)` | `(number, number, number?) → this` | Add an edge (both directions if undirected); `w` defaults to 1 (ignored if unweighted). |
| `.neighbors(u)` / `.hasEdge(u, v)` | | Out‑neighbor ids / edge existence. |
| `.nodeCount` / `.edgeCount` | getters | Counts. |
| `.bfs(src)` / `.dfs(src)` | `(number) → number[]` | Visitation order from `src`. |
| `.dijkstra(src)` / `.dijkstra(src, dst)` | `→ number[]` / `→ number` | Shortest distances (Infinity if unreachable); throws on a negative weight. |
| `.bellmanFord(src)` | `(number) → number[]` | Distances; allows negatives, **throws on a negative cycle**. |
| `.topologicalSort()` | `() → number[]` | Directed only; throws on a cycle. |
| `.connectedComponents()` | `() → number[]` | `comp[i]` = component id (undirected). |
| `.floydWarshall()` | `() → number[][]` | All‑pairs distance matrix. |
| `.mst()` | `() → {weight, edges:[[u,v,w],…]}` | Minimum spanning forest (undirected; Kruskal). |
| `.aStar(src, dst, heuristic)` | `(number, number, fn) → {dist, path}` | A\* with an admissible `heuristic(node)`; `path` is `[]` if unreachable. |

```js
const g = new Graph({ directed: true, weighted: true });
const a = g.addNode(), b = g.addNode(), c = g.addNode();
g.addEdge(a, b, 2).addEdge(b, c, 3).addEdge(a, c, 10);
g.dijkstra(a);          // [0, 2, 5]
g.dijkstra(a, c);       // 5
```

## Capacity limits

`LRU` and `RingBuffer` allocate their whole capacity up front, so a capacity above **2^24**
(16 777 216) throws `RangeError`. Every other container grows on demand and has no fixed ceiling.

## Binary persistence — `serialize()` / `deserialize()`

Every container in this module encodes to a self-describing binary record and back. One envelope,
shared with `dyna:ml`'s model persistence, so there is one format to version:

```
"DYNS" | u16 version | u16 type_id | u32 flags | u64 payload_len | payload | u32 CRC32C
```

All integers are little-endian and doubles go out as their IEEE-754 bit pattern, so a record written
on one machine reads on another.

**Serialization is two ordinary methods on the containers themselves** — an instance `.serialize()`
and a static `.deserialize(bytes, arg?)`, on every class that has a codec.

| Member | Signature | Description |
|---|---|---|
| `container.serialize()` | `→ Uint8Array` | The record for this container. |
| `Class.deserialize(bytes, arg?)` | `→ container` | **Static, and TYPE-CHECKED**: a record of any other type is a `TypeError` naming both types. `bytes` is a `Uint8Array`, any TypedArray view, or an `ArrayBuffer`. |

```js
import { Trie, Deque } from "dyna:structures";

const words = new Trie();
words.insert("hello");
const bytes = words.serialize();            // Uint8Array
Trie.deserialize(bytes).has("hello");       // true

try { Deque.deserialize(bytes); }           // a Trie record, not a Deque
catch (e) { print(e.message); }             // "Deque.deserialize: these bytes
                                            //  are a Trie record, not a Deque"
```

**`deserialize` refuses another type's record**, which the older generic static could not: it built
whatever the record named, so asking for a `Trie` and receiving a `Deque` was a surprise rather than
an error.

**There is no `Serializer` class.** It was a compiled capability whose only reuse was its output
buffer, and that measured **1.50× on a 16-element container (0.1 µs absolute), 1.00× at 1k and 0.97×
at 64k** — so it bought nothing and cost an API. Removing the shared buffer also removed its
reentrancy hazard: a codec that calls back into JS used to have to be refused mid-encode, and now
simply works.

**A `Heap` record carries values, never an order.** A heap's order *is* its comparator, and a
function is not data, so the record cannot hold it. Pass it as the second argument to get that order
back; omit it for natural numeric order, exactly as the constructor works:

```js
const heap = new Heap((a, b) => b - a);          // a MAX heap
heap.push(3); heap.push(1);
Heap.deserialize(heap.serialize(), (a, b) => b - a).pop();   // 3
Heap.deserialize(heap.serialize()).pop();                    // 1 -- natural order
```

The two lines decode the same bytes and disagree, which is the point: the ordering was never in the
record. A non-function second argument throws `TypeError` naming the fix. This is the case the
per-class form states honestly: the old generic `decode(bytes, arg)` carried a second argument that
meant nothing for the other 22 types.

**What each record contains.** Numeric containers write their arrays, so the state is restored
exactly — a `UnionFind` keeps its forest rather than replaying unions. Value-holding containers write
their elements as one payload produced by the engine's own serialiser, which accepts the values
structured cloning accepts: primitives, plain objects, arrays, `Map`, `Set`, `Date`, typed arrays.
A function, a `Proxy` or a `Symbol` throws, and the next `serialize()` still works. Skiplist levels are
never written (they are RNG-derived); a `SortedSet`/`SortedMap` stores its sorted key stream and
rebuilds. A `Trie` stores its keys in sorted order, so the record is a function of the key set and
not of the insertion order. A `Graph` writes its adjacency node by node, omitting edge weights
entirely when the graph is unweighted; `edgeCount` is **recomputed** from the adjacency on read
rather than stored, so a record cannot claim an edge count it does not contain.

**Reading a record is reading untrusted input.** `decode` verifies the CRC before interpreting a
single payload byte, and every length that would drive an allocation is checked against the bytes
that actually remain. A corrupted or hostile record throws; it does not allocate on a forged count.
The element payload is read with bytecode and `SharedArrayBuffer` refused, so a record cannot carry
executable code.

**Reentrancy.** Each `serialize()` allocates its own buffer, so a codec that calls back into JS --
`List`, whose codec calls the object's own `toArray()` -- can re-enter serialization safely. The
older shared-buffer capability had to refuse that case; there is nothing left to refuse, and nesting
needs no workaround.

---

# sys

`import { env, getEnv, setEnv, args, cwd, chDir, platform, pid, hostName, homeDir, memoryUsage, cpuInfo, memInfo, loadAvg, uptime, diskUsage, Exec, Which } from "dyna:sys";`

Process and environment access. **Filesystem operations — metadata, directories, links, globbing
and temp files — are in [`dyna:file`](#file)**. On failure, functions throw an
`Error` whose `.code` (e.g. `"ENOENT"`) and `.errno` identify the OS error.

### Subprocesses

| Function | Signature | Description |
|---|---|---|
| `Exec(command, args?, options?)` | `(string, string[], object) → object` | Run a program to completion and capture its output. |
| `Which(name)` | `(string) → string \| null` | Resolve a program name against `PATH`. |

**There is no shell.** `Exec` takes an argv array and nothing else — no
`shell: true`, no command string. Command injection is therefore not something
you have to avoid; it is unrepresentable. A caller who genuinely wants a shell
writes `Exec("/bin/sh", ["-c", line])` and owns that decision on a line they
wrote.

`Exec` returns `{ code, signal, stdout, stderr, timedOut }`. A child killed by a
signal has `signal` set to its **name** and `code` **null** — a signalled child
and a nonzero exit mean different things, and a wrapper that conflates them
loses the difference. `stdout` and `stderr` are strings, or `Uint8Array` under
`encoding: "bytes"`.

| Option | Default | Description |
|---|---|---|
| `cwd` | inherited | Working directory. Not a directory ⇒ `Error`. |
| `env` | inherited | **Replaces** the environment. The child is resolved against this `PATH`. |
| `input` | none | A string or bytes written to the child's stdin. |
| `timeoutMs` | none | `SIGTERM`, then `SIGKILL` two seconds later. |
| `maxBuffer` | 8 MiB | Output past it is a `RangeError`, not a truncation. |
| `encoding` | `"utf8"` | Or `"bytes"`. |

Both pipes are drained **while** the child runs, so a program that writes more
than a pipe buffer holds cannot deadlock. The timeout signals the child's whole
process **group** — a program that forks and exits leaves a grandchild holding
the pipe open, and killing only the child waits for an EOF that never comes.

A command that cannot be resolved is an `Error` naming it, rather than the
exit code 127 a shell would report.

```js
Exec("git", ["rev-parse", "HEAD"]).stdout.trim();
Exec("echo", ["$(whoami)"]).stdout;             // "$(whoami)\n" -- no shell
Exec("sleep", ["60"], { timeoutMs: 100 });      // { timedOut: true, signal: "SIGTERM", ... }
Exec("cat", [], { input: "piped" }).stdout;     // "piped"
Which("git");                                   // "/usr/bin/git" or null
```

### Process & environment

| Function | Signature | Description |
|---|---|---|
| `env()` | `() → object` | All environment variables as an object. |
| `getEnv(name)` | `(string) → string \| undefined` | One variable. |
| `setEnv(name, value)` | `(string, string) → void` | Set a variable. |
| `args()` | `() → string[]` | The process argument vector. |
| `cwd()` / `chDir(path)` | | Get / set the working directory. |
| `platform()` | `() → string` | `"darwin"`, `"linux"`, or `"unknown"`. |
| `pid()` | `() → number` | The process id. |
| `hostName()` | `() → string` | The host name. |
| `homeDir()` | `() → string` | The current user's home directory. |
| `memoryUsage()` | `() → object` | Engine memory accounting plus the OS peak RSS. |
| `cpuInfo()` | `() → object` | `{model?, cores?, threads?, mhz?, features}` — the CPU and its detected vector ISAs. |
| `memInfo()` | `() → object` | `{total, free?, available?}` in bytes. |
| `loadAvg()` | `() → number[]` | The 1, 5 and 15 minute load averages. |
| `uptime()` | `() → number` | Seconds since boot. |
| `diskUsage(path)` | `(string) → object` | `{total, free, available}` in bytes for the filesystem holding `path`. |

### Machine facts

`cpuInfo().features` is the **same detection the SIMD dispatcher branches on**,
not a second copy of it. That matters more than it looks: the day feature
detection stops selecting a vector kernel, every benchmark simply gets slower
and nothing says why. This is the instrument that says why.

A fact the OS does not report is **absent**, never zero — a caller charting free
memory has to be able to tell "none left" from "this system did not say". Apple
Silicon publishes no `mhz`, so on that host the field is not there at all.

`free` and `available` differ and both are useful: `free` is untouched, while
`available` includes what the OS would reclaim under pressure, which is the one
that answers "can I allocate this?". For `diskUsage` the same pair means
reserved blocks — `available` is what a non-root caller may actually take.

```js
cpuInfo().features.includes("neon") || cpuInfo().features.includes("sse42");
memInfo().total > 0;                       // true
loadAvg().length;                          // 3
diskUsage("/").available <= diskUsage("/").free;   // true -- reserved blocks
```

### `memoryUsage()`

Answers "what did this cost in memory?", which a stopwatch cannot. Code that keeps its nanoseconds
while allocating an extra object per call has not got faster — it has moved the cost to the
collector.

| Field | Meaning |
|---|---|
| `mallocCount` / `mallocSize` | **Live** allocations and bytes. The delta across N operations is what those operations *retained* — the leak signal, once a collection has run. |
| `memoryUsedCount` / `memoryUsedSize` | The same for engine-tracked memory. |
| `objCount` / `objSize` | Live JS objects. |
| `strCount` / `strSize`, `propCount`, `shapeCount`, `arrayCount` | Live strings, properties, shapes, arrays. |
| `peakRss` | The process high-water mark, **in bytes on every platform**. The only field that reflects transient churn: the engine counters cannot see memory that was allocated and freed between two samples. |

Sizes include the allocator's per-block overhead, so they match what the process actually took.
Sample far enough apart that a collection has run in between — otherwise every in-flight object
counts and the delta reads as a leak:

<!-- check:skip -->
```js
import { memoryUsage } from "dyna:sys";
const before = memoryUsage();
for (let i = 0; i < 10000; i++) work();
const after = memoryUsage();
print((after.mallocSize - before.mallocSize) / 10000, "bytes retained per call");
```

`gc()` is not a global in an ordinary build; a collection happens on its own schedule, so take the
two samples far enough apart that one has run in between, or the delta counts objects that were
merely still in flight.

---

# file

`import { readFile, writeFile, FileReader, FileWriter, stat, lstat, exists, readDir, makeDir, remove, removeAll, rename, symlink, readLink, realPath, chmod, glob, tempDir, makeTempDir, makeTempFile } from "dyna:file";`

The filesystem module: buffered file content I/O (with per-OS fast paths — macOS
`F_RDAHEAD`/`F_PREALLOCATE`/`F_FULLFSYNC`; Linux `fadvise`/`fallocate`/io_uring) **plus all
filesystem operations** (metadata, directories, links, globbing, temp files), and the
[`Path`](#path) handle every one of them takes. Process and
environment access is in [`dyna:sys`](#sys). Filesystem functions throw an `Error` whose `.code`
(e.g. `"ENOENT"`) and `.errno` identify the OS error.

> **Every function and stream constructor here takes a [`Path`](#path), not a string**, and a string
> is **refused** — `TypeError: path must be a Path -- wrap it with new Path(...)`. Refusing beats
> coercing: a path assembled by string concatenation is where separator and traversal bugs live, and
> `new Path(...)` is the one place that normalisation happens. The exceptions are the two that take a
> *pattern* or an opaque link target rather than a path: `glob(pattern)`, and `symlink`'s `target`.
> `File` and `Glob` accept either, because their constructors build the `Path` for you.

### `readFile(path)` · `writeFile(path, data)`

One-shot whole-file helpers.

| Parameter | Type | Description |
|---|---|---|
| `path` | `Path` | The file. |
| `data` | `string \| bytes` | (`writeFile`) the content to write. |

**Returns** `readFile`: `string` (the file content). `writeFile`: `number` (bytes written). **Throws** on I/O error.

### `readFileAsync(path, options?)` · `writeFileAsync(path, data, options?)`

The same two operations returning a `Promise`, so a whole-file read does not
stop the event loop. `options.bytes` makes `readFileAsync` resolve to a
`Uint8Array` instead of a `string`; `options.append` is as `writeFile`. An I/O
error **rejects** with an `Error` carrying `.errno` and `.path` — it does not
throw synchronously.

**These pick one of two strategies per call, and the threshold is a loop-block
budget rather than a throughput crossover.** Offloading to a pool thread costs a
roughly fixed ~190–275 µs (a full loop turn: worker, wake fd, drain, settle), so
against a page-cache hit it never wins on throughput — 10.2× slower at 64 KiB,
2.95× at 1 MiB, 1.06× at 8 MiB. What it buys is the loop: **30 reads of 8 MiB
run synchronously fired a 1 ms timer zero times in 31 ms; the same reads through
`readFileAsync` fired it 29 times and took 33 ms.** So the gate asks whether the
synchronous read would stall the loop long enough to matter, and offloads at
**1 MiB**, where that cost reaches ~110 µs. Below it the call runs inline and
settles on the same turn — paying 190 µs to avoid a 20 µs stall is a tax.

`asyncStats()` reports `{inline, offloaded, readMin, writeMin}` so a caller (or
a test) can see which arm ran; a portfolio whose selection is invisible is one
that can silently stop choosing the fast arm.

Both need `dyna:net`, which owns the shared reactor. In a build without it they
are **not exported**, rather than exported and blocking.

### Copying, moving and sniffing

`copyFile` goes through the kernel where the kernel offers it — `fcopyfile` on
Darwin (which clones the extents outright on APFS) and `copy_file_range` on
Linux — and falls back to a buffered loop otherwise, because both of those
refuse in cases the loop handles. It **refuses an existing destination** by
default: a copy that silently replaces a file is the one nobody notices until
the file is gone. The source's permission bits carry over, so a copy of a
private key does not land world-readable.

`move` uses `rename(2)`, which is atomic but only **within one filesystem**.
Across one it fails `EXDEV` and falls back to copy-then-unlink, which is *not*
atomic. The unlink happens last: if it fails the data still exists at both
paths, which is recoverable, where unlinking first and then failing the write
is not.

`sniffType` reads **magic bytes, never the extension** — the extension is what
the sender claims and the bytes are what arrived. RIFF containers are told
apart at offset 8 (a WAV is not a WebP), SQLite's magic is 16 bytes, and tar's
sits at offset 257. With no magic match, a NUL byte in the first block means
`application/octet-stream` and anything else is `text/plain` — that last one is
a guess, and a caller that needs certainty should not ask a sniffer.

```js
import { Path, copyFile, sniffType } from "dyna:file";
sniffType(new Uint8Array([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]));  // "image/png"
sniffType(new Uint8Array([0x68, 0x00, 0x69]));   // "application/octet-stream" -- a NUL
```

### `class FileReader`

Buffered sequential reader.

**`new FileReader(path, options?)`**

| Parameter | Type | Description |
|---|---|---|
| `path` | `Path` | The file to open for reading. |
| `options` | `{ bufferSize? }?` | Read-buffer size in bytes. |

| Member | Signature | Description |
|---|---|---|
| `.read()` | `() → string \| null` | The next buffered chunk, or `null` at EOF. |
| `.readLine()` | `() → string \| null` | The next line **without** its newline, or `null` at EOF. Handles buffer refills. |
| `.readAll()` | `() → string` | The rest of the file. |
| `.close()` | `() → void` | Release. |

### `class FileWriter`

**A durable sync waits for the DEVICE, not the kernel, and that is milliseconds.**
On Darwin `.sync()` is `fcntl(F_FULLFSYNC)` — measured here at **4810 µs against
38.7 µs for a plain `fsync`, 135×** — and it runs on the JS thread, so nothing
else is served while it waits. `.syncAsync()` moves that wait to a pool thread:
measured over 12 syncs, `.sync()` let a 1 ms timer fire **zero** times while
`.syncAsync()` let it fire **47–48** times, **at the same total time** (56 ms vs
55–60 ms), because a ~200 µs hop against a 5 ms device wait is 4%.

**It offloads only when something is dirty**, and that gate is measured too: a
durable sync with nothing to commit costs **7.3 µs** — 660× less — so
offloading every call would put the hop in front of 7 µs of work and be 27×
slower. `asyncStats()` reports which arm ran.

Buffered sequential writer.

**`new FileWriter(path, options?)`**

| Parameter | Type | Description |
|---|---|---|
| `path` | `Path` | The file to open/create for writing. |
| `options` | `{ bufferSize?, preallocate? }?` | Write-buffer size; `preallocate` reserves a file extent (fewer syscalls, less fragmentation). |

| Member | Signature | Description |
|---|---|---|
| `.write(data)` | `(string \| bytes) → number` | Buffer bytes for writing; returns the count. |
| `.flush()` | `() → void` | Flush the buffer to the OS. |
| `.sync()` | `() → void` | Durable flush to disk (`F_FULLFSYNC` on macOS). **Blocks the event loop for milliseconds** — see below. |
| `.syncAsync()` | `() → Promise<void>` | The same durability, off the loop. Rejects with an `Error` carrying `.errno`. Needs `dyna:net`. |
| `.close()` | `() → void` | Flush and release. |

```js
const w = new FileWriter(new Path("/tmp/out"), { bufferSize: 1 << 16, preallocate: 1 << 20 });
try { for (let i = 0; i < 100000; i++) w.write(`row ${i}\n`); w.sync(); }
finally { w.close(); }
```

### `stat(path)` · `lstat(path)`

Return file metadata. `lstat` does not follow a final symlink.

| Parameter | Type | Description |
|---|---|---|
| `path` | `Path` | The target path. |

**Returns** an object: `{ size, mode, isDir, isFile, isSymlink, mtimeMs, atimeMs, ctimeMs, uid, gid, ino, nlink }`. `mode` is the full Unix `st_mode`; times are milliseconds (float). **Throws** on a missing path.

### `exists(path)`

**Returns** `boolean` — whether the path exists (uses `lstat`, so `true` even for a dangling symlink). Does **not** throw.

### `readDir(path)`

| Parameter | Type | Description |
|---|---|---|
| `path` | `Path` | A directory. |

**Returns** `{ name, isDir, isFile, isSymlink }[]` — entries sorted by name, excluding `.` and `..`. **Throws** if `path` is not a readable directory.

### `makeDir(path, options?)` · `remove(path)` · `removeAll(path)` · `rename(from, to)`

| Function | Parameters | Behavior |
|---|---|---|
| `makeDir(path, { recursive?, mode? })` | `string`, options | Create a directory. `recursive: true` creates parents. `mode` sets permissions. |
| `remove(path)` | `string` | Remove a file or empty directory. Throws if a directory is non-empty. |
| `removeAll(path)` | `string` | Recursively remove `path`. Symlink-safe (never deletes through a symlink out of the tree). A missing `path` is a **no-op** (no throw). |
| `rename(from, to)` | `Path, Path` | Rename within one filesystem. |
| `copyFile(from, to, opts?)` | `(Path, Path, {overwrite?}) → number` | Copy, returning bytes written. Refuses an existing destination unless `overwrite` is set. |
| `move(from, to)` | `(Path, Path) → void` | Rename, falling back to copy-then-unlink across filesystems. |
| `sniffType(pathOrBytes)` | `(Path \| bytes) → string` | The MIME type from the content's magic bytes. |

### `symlink(target, linkPath)` · `readLink(path)` · `realPath(path)` · `chmod(path, mode)`

| Function | Signature | Description |
|---|---|---|
| `symlink(target, linkPath)` | `(string, string) → void` | Create `linkPath` pointing at `target`. |
| `readLink(path)` | `(string) → string` | The target of a symlink. |
| `realPath(path)` | `(string) → string` | The canonicalized absolute path (resolving symlinks). |
| `chmod(path, mode)` | `(string, number) → void` | Set permission bits (e.g. `0o755`). |

### `glob(pattern, options?)`

Expand a shell-style glob against the filesystem.

| Parameter | Type | Description |
|---|---|---|
| `pattern` | `string` | Supports `*` (not crossing `/`), `**` (crossing directories), `?`, and `[...]` character classes (ranges and `[!...]` negation). |
| `options` | `{ cwd? }?` | `cwd` sets the base for a relative pattern. |

**Returns** `string[]` — matching paths, sorted and de-duplicated. Symlink cycles terminate (never infinite-loop).

```js
glob("src/**/*.js");     // every .js under src, at any depth
glob("data/file-[0-9].csv");
```

### Temp paths — `tempDir()` · `makeTempDir(prefix)` · `makeTempFile(prefix)`

| Function | Signature | Description |
|---|---|---|
| `tempDir()` | `() → string` | The OS temporary directory. |
| `makeTempDir(prefix)` | `(string) → string` | Atomically create and return a unique temp directory. |
| `makeTempFile(prefix)` | `(string) → string` | Create and return a unique temp file path. |

---

### File

`import { File } from "dyna:file";`

A value handle over **one path**: the shape almost every caller actually has. Every method is the
free function of the same name with this handle's path supplied, so there is one implementation of
`readFile` and one of `stat` — the class is a rebinding of the argument, not a second code path.

```js
const dir = new Path("/tmp");
const f = new File(dir.join("report.txt"));
f.writeText("hello");
f.append("\n");
if (f.exists()) print(f.stat().size);          // 6
```

| Member | Signature | Description |
|---|---|---|
| `new File(path)` | `(Path \| string) → File` | A string is accepted **here and nowhere else** — a constructor whose whole subject is the path is the one place it is unambiguous. |
| `.path` | `Path` (getter) | |
| `.readText()` · `.writeText(data, opts?)` · `.append(data)` | | `append` supplies `{append: true}` so the common case cannot be spelled wrong. |
| `.readBytes()` · `.writeBytes(data)` | `() → Uint8Array` | Binary. `readBytes` reads raw bytes — it does **not** decode to a string first, so `0xFF` survives instead of becoming U+FFFD. |
| `.stat()` · `.lstat()` · `.exists()` · `.realPath()` · `.chmod(mode)` | | |
| `.remove()` | | |
| `.copyTo(dest)` | `(Path) → File` | A real copy — writing through one does not affect the other. |
| `.moveTo(dest)` | `(Path) → File` | **Retargets this handle** to the new path, which is what moving a file you hold means. |
| `.reader(opts?)` · `.writer(opts?)` | | The buffered streams over this path. |
| `.toString()` · `.toJSON()` | `string` | The path. |

A `File` names a path, not an open descriptor. Removing the file and writing it again through the
same handle works.

### Glob

`import { Glob } from "dyna:file";`

A compiled pattern, matched against unbounded paths.

| Member | Signature | Description |
|---|---|---|
| `new Glob(pattern)` | `(string) → Glob` | |
| `.matches(path)` | `(Path) → boolean` | **Purely lexical** — never touches the disk, so it works on a path that does not exist. |
| `.expand(dir?)` | `(Path?) → Path[]` | Walks the filesystem. Equal to `glob(pattern, {cwd: dir})`. |
| `.filter(paths)` | `(Path[]) → Path[]` | `matches` over a list you already have. |
| `.pattern` · `.hasWildcard` | getters | `hasWildcard` is computed once, at construction. |

`*` never crosses `/`; `**` matches **zero or more** directories, so `**/*.js` finds `top.js` as
well as `sub/deep.js`. `?` and `[...]` classes are supported. Dotfiles are skipped unless the
pattern names them explicitly.

**Compiling a glob buys nothing, and that is worth saying.** It is a copy and one wildcard scan, so
unlike [`Range`](#semver) or [`Compressor`](#class-compressor) there is almost nothing to amortise.
It exists for the API — one pattern, three ways to apply it — not for speed.

### Path

`import { Path } from "dyna:file";`

A **value handle** over one POSIX path. Every filesystem entry point in this module takes a `Path`
and nothing else — a string throws a `TypeError`. Constructing one normalises the path
(collapsing `.`, `..` and repeated separators) and caches the result, so the normalisation happens
once per path rather than once per call.

```js
const dir  = new Path("/var", "log");
const file = dir.join("app.log");     // /var/log/app.log
file.toString();                      // "/var/log/app.log"
```

The separator is always `/`; this is the POSIX flavour and there is no win32 variant. No `Path`
method touches the filesystem.

### `new Path(...segments)`

| Parameter | Type | Description |
|---|---|---|
| `...segments` | `string \| Path` | One or more fragments. They are joined with `/` and the result is normalised. Empty strings are skipped. |

**Returns** `Path`. **Throws** `TypeError` with no arguments, or for a segment that is neither a
string nor a `Path`. `new Path(aPath)` shares the existing buffer instead of copying it.

```js
String(new Path("a", "b", "..", "c"));   // "a/c"
String(new Path("/x/", "y", "z"));       // "/x/y/z"
```

### Properties

| Property | Type | Description |
|---|---|---|
| `.dirname` | `Path` | The parent. `"."` when there is none, `"/"` at the root. |
| `.basename` | `string` | The final segment. |
| `.extname` | `string` | The extension of the final segment, including the `.`. A leading dot makes a dotfile, not an extension, so `new Path(".bashrc").extname` is `""`. |
| `.isAbsolute` | `boolean` | Whether the path starts at the root. |

All four are slices of the cached buffer, not scans.

Because a `Path` is normalised at construction, these are splits of the **normalised** value:
`new Path("a/..")` is `"."`, so its `.dirname` is `"."`. The free functions this class replaced were
purely lexical on the raw string and answered `"a"`.

### Methods

| Method | Returns | Description |
|---|---|---|
| `.join(...segments)` | `Path` | Appends the segments and normalises. |
| `.resolve(...segments)` | `Path` | Resolves right-to-left to an **absolute** path, anchoring at the rightmost segment that starts with `/`. There is no working directory here, so the fallback anchor is `/`. |
| `.relativeTo(other)` | `Path` | The path from `this` to `other`. Both sides are resolved first. Returns `"."` when they name the same place — a `Path` is never empty. |
| `.equals(other)` | `boolean` | Value equality. Both sides are normalised, so `"a//b/./c"` equals `"a/b/c"`. Anything that is not a `Path` is not equal. |
| `.basenameWithout(suffix)` | `string` | The final segment with a literal trailing `suffix` removed — unless removing it would empty the segment. |
| `.toString()` · `.toJSON()` | `string` | The normalised path. `JSON.stringify` and template interpolation both go through these. |

```js
String(new Path("/a/b/c").relativeTo(new Path("/a/d")));   // "../../d"
new Path("a//b/./c").equals(new Path("a/b/c"));            // true
new Path("/tmp/report.tar.gz").basenameWithout(".gz");     // "report.tar"
```

### Statics

| Static | Returns | Description |
|---|---|---|
| `Path.cwd()` | `Path` | The process working directory. |
| `Path.home()` | `Path` | `$HOME`, or `/`. |
| `Path.temp()` | `Path` | `$TMPDIR`, or `/tmp`. Agrees with `tempDir()`, trailing separator included. |
| `Path.isPath(v)` | `boolean` | Whether `v` is a `Path`. |
| `Path.sep` | `string` | `"/"`. |
| `Path.delimiter` | `string` | `":"`. |

The three OS-derived statics strip a trailing separator: it carries no information on a path that
names a directory, and macOS sets `TMPDIR` with one.

### Paths in, paths out

Anything that returns a path returns a `Path`: `realPath`, `glob`, `tempDir`, `makeTempDir`,
`makeTempFile`. Two functions deal in strings instead, because a symbolic link's target is not a
path this module resolves — it is opaque bytes stored inside the link, may be relative, and may name
something that does not exist: `symlink(target, linkPath)` takes the target as a `string`, and
`readLink(path)` returns one. Normalising it would silently rewrite the link.

# uring

`import { readFile, readFileSync, checksum } from "dyna:uring";`

High-queue-depth bulk file reads via Linux **io_uring**. Available only on Linux builds with
io_uring support; on other platforms use `dyna:file`.

| Function | Signature | Description |
|---|---|---|
| `readFile(path)` | `(string) → Uint8Array` | Whole-file read submitted through io_uring (many outstanding reads, minimal syscalls). |
| `readFileSync(path)` | `(string) → Uint8Array` | Synchronous whole-file read. |
| `checksum(path)` | `(string) → number` | Streamed CRC over the file at high queue depth. |

**Throws** on I/O error, or if the ring cannot be created (e.g. seccomp-restricted environment).

---

# net

Networking: the `App` server and HTTP client, TCP/UDP/IPC sockets, and IP address / CIDR handling.
One module, one `import`.

## HTTP — `App`, `HTTPClient`, `HTTPServer`, `HTTPServerAsync`

`import { App, HTTPClient, HTTPServerAsync, HTTPServer } from "dyna:net";`

An HTTP/1.1 client (`HTTPClient`), an application server (`App`), and two low-level static server
implementations (`HTTPServerAsync`, `HTTPServer`). **`App` is the server you build on** — it runs
your JavaScript handlers on a single-thread event-loop reactor (kqueue on macOS; epoll or io_uring
on Linux). `HTTPServerAsync` is the bare reactor: it serves fixed responses only and never enters
the JavaScript world.

### `class HTTPClient`

**`new HTTPClient()`**

| Member | Signature | Description |
|---|---|---|
| `.get(url)` | `(string) → Response` | Perform a GET. |
| `.post(url, body, headers?)` | `(string, string\|bytes, object?) → Response` | POST with a body and optional headers. |
| `.request(url, options)` | `(string, object) → Response` | A general request (`{ method, headers, body }`). |
| `.setTimeout(ms)` | `(number) → void` | Set the request timeout. |
| `.disconnect()` | `() → void` | Drop the underlying connection. |
| `.close()` | `() → void` | Release the client. |

**`Response`** = `{ status: number, headers: object, body: string }`.

### `class App`

The application server. You register **typed routes** — there is deliberately no raw request
handler — and `App` runs your handlers on the single-thread event-loop reactor. A resource: call
`.close()` when done.

**`new App(config)`**

| Field | Type | Description |
|---|---|---|
| `config.port` | `number` | The listen port. **Required in practice** — unlike `HTTPServerAsync`, `.port` reports this value, so `port: 0` is not resolved to an OS-assigned port. |
| `config.idleTimeoutMs` | `number` | Close a connection that has made no **protocol progress** for this long. Default **30000**; `0` disables. Progress means a request parsed, a WebSocket frame decoded, or upload body bytes written — *not* bytes merely arriving, so a client that dribbles one byte at a time to hold a connection open (slowloris, CWE-400) is closed, while a slow but genuinely progressing client is not. Swept once a second, so the effective deadline is `idleTimeoutMs` + up to 1s. |

| Member | Signature | Description |
|---|---|---|
| `.rpc(path, methods)` | `(string, object) → void` | Register a strict **JSON-RPC 2.0** endpoint. `methods` maps a method name to `(params) => result`. See below. |
| `.static(prefix, dir, opts?)` | `(string, string, object?) → void` | Serve files under `prefix` from `dir` via zero-copy `sendfile`. `opts`: `{ maxFileSize?: number, allow?: string[] }` where `allow` is a whitelist of `.ext`s or MIME types. `maxFileSize` defaults to 32 MiB and a larger file is refused. On a build with `CONFIG_IO_URING` that limit is also a memory and latency budget: that backend reads the file into memory rather than streaming it, so it allocates the file's size and pauses the loop for the read. |
| `.upload(path, opts, handler)` | `(string, object, function) → void` | Stream an upload to disk, then call `handler(savedPath, meta)`. `opts`: `{ dir: Path (required), maxFileSize?, allow? }` — `dir` is a [`Path`](#path), and a string is **refused**; `meta` is `{ size, contentType }`. |
| `.ws(path, handlers)` | `(string, object) → void` | Register an **RFC 6455** WebSocket endpoint. `handlers`: `{ open(ws), message(ws, data, isBinary), close(ws, code, reason) }`. |
| `.start()` | `() → void` | Bind, listen, and fold the reactor into this thread's event loop. |
| `.port` | `number` (getter) | The port actually bound. With `port: 0` this reports the ephemeral port the OS chose, but only after `.start()`. |
| `.close()` | `() → void` | Stop and release. Also `[Symbol.dispose]()`. |

**RPC contract.** Each method receives the request's JSON-RPC `params` as its single argument and
returns the `result`. Throwing produces a JSON-RPC error object (code `-32000`); an unknown method
yields `-32601`; a malformed request yields `-32600`/`-32700`. A method may return a value
synchronously *or* a `Promise` for a **single** request; a **batch** request requires synchronous
methods (an async handler in a batch is rejected).

**WebSocket connection (`ws`).** The object passed to the handlers has `.send(data)` — a `string`
is sent as a text frame, an `ArrayBuffer` as a binary frame — and `.close()`, which sends a close
frame and tears the connection down.

**Threading.** Handlers run on the JS thread, so they share your program's heap with no
cross-thread copy — but a blocking handler stalls the whole reactor (offload heavy CPU work to an
`os.Worker`), and a same-thread blocking `HTTPClient` cannot call the same process's `App`. Drive an
`App` from another process.

<!-- check:skip -->
```js
import { Path } from "dyna:file";

const app = new App({ port: 8080 });
app.rpc("/rpc", { add: ([a, b]) => a + b });
app.static("/assets", new Path("/var/www"), { allow: [".css", ".png"] });
app.start();
// curl -sd '{"jsonrpc":"2.0","id":1,"method":"add","params":[2,3]}' :8080/rpc
//   → {"jsonrpc":"2.0","result":5,"id":1}
```

### `class HTTPServerAsync`

A low-level reactor that maps paths to **fixed responses** — it never runs JavaScript, so it is not
a substitute for `App`. Useful for static content and for exercising the reactor's raw concurrency.

**`new HTTPServerAsync(config)`**

| Field | Type | Description |
|---|---|---|
| `config.port` | `number` | The listen port; `0` picks a free port (read it back from `.port`). |
| `config.routes` | `object` | Maps a path string to either a body `string` or `{ status, contentType, body }`. Unmatched paths return 404. |

| Member | Signature | Description |
|---|---|---|
| `.start()` | `() → void` | Begin listening/serving. |
| `.stop()` | `() → void` | Stop and release. |
| `.port` | `number` (getter) | The bound port. |

Routes are fixed at construction; there are no callbacks to run, so nothing user-supplied executes
per request. (For dynamic behavior, use `App`.)

<!-- check:skip -->
```js
const s = new HTTPServerAsync({ port: 0, routes: { "/": "hello\n" } });
s.start();
const c = new HTTPClient();
c.get(`http://127.0.0.1:${s.port}/`).status;   // 200
s.stop();
```

### `class HTTPServer`

A thread-pool server variant with the same `.start()`, `.stop()`, and `.port` surface. Prefer
`HTTPServerAsync` for high connection concurrency.

---

## TCP, UDP and IPC — `TCPServer`, `UDPSocket`

`import { TCPServer, UDPSocket } from "dyna:net";`

All three run **entirely on the JS thread** via the shared reactor — one reactor per JS thread, so a
server and a client (or several of each) coexist in one process. Nothing here is offloaded to the IO
pool: the reactor already knows when a socket is ready, and handing a ready socket to another thread
costs more than the read.

Bytes handed to a handler are a **`Uint8Array`**, and a **copy** rather than a view: the adapter
recycles its shared receive buffer as soon as the handler returns, so a handler may keep what it is
given. It is a typed array rather than a bare `ArrayBuffer` deliberately — an `ArrayBuffer` has no
`.length` and indexes to `undefined`, so a caller's loop would run zero times and report success.

### `class TCPServer`

| Member | Signature | Description |
|---|---|---|
| `new TCPServer({port, maxConnections?, idleTimeoutMs?})` | | TCP. `port: 0` binds an OS-assigned port, readable from `.port` after `start()`. |
| `new TCPServer({path, …})` | | **IPC**: an AF_UNIX stream socket at a filesystem path. Everything below is identical — only the address differs. |
| `.start(handlers)` | `(object) → void` | `handlers`: `{connect(conn), data(conn, bytes), close(conn)}`. |
| `.port` | getter | The bound port (0 for a `path` server). |
| `.close()` | | Releases this holder's reference to the shared reactor. **Mandatory**, not tidy — see below. |
| `TCPServer.connect({host, port, connectTimeoutMs?}, handlers)` | `→ TCPServer` | Async connect. `handlers.connect(conn, err)` fires with `err` non-null on failure — a refused connection reports the error rather than silence. |
| `TCPServer.connect({path}, handlers)` | `→ TCPServer` | The same over AF_UNIX. |

**`connectTimeoutMs` bounds a connect that will never complete.** Without it an unroutable address
is not an error but a wait — the OS gives up after about 75 seconds on Darwin, and the handle holds
itself alive for the whole time. On expiry `connect(conn, err)` fires with an error, exactly as a
refusal does, so a caller has one failure path rather than two. Default `0` (off).

**`maxConnections` and `idleTimeoutMs` bound what a peer can spend.** Each accepted connection costs
a descriptor, an allocation and a JS object; past `maxConnections` the accepted socket is closed
immediately, before any of that. `idleTimeoutMs` closes a connection that has made no **progress** —
which is the `data` handler being invoked and returning, *not* bytes arriving. That distinction is
the whole defence: an attacker who dribbles bytes forever without ever completing anything would
otherwise look permanently active.

**Both default to off, and that is deliberate.** On a raw transport an idle connection is legitimate
(a subscription, a game session, anything with heartbeats), unlike HTTP where a request must
complete — which is why `App` defaults `idleTimeoutMs` to 30 s and this does not. **A `TCPServer`
exposed to untrusted peers should set both**; one exposed only to known clients need not.

The `conn` handed to a handler has `.write(data)` and `.close()`. `data` may be a string, a
`Uint8Array` (or any typed-array view) or an `ArrayBuffer`; a string is sent as UTF-8, and a view is
sent as exactly its own bytes, not its whole backing buffer.

**Every handle must be closed.** A live `TCPServer` holds a reference that keeps the event loop
alive, so forgetting one is a program that never exits — including a handle created only to probe an
error path.

**IPC permissions come from the DIRECTORY.** A unix socket's own mode is not the access control:
binding unlinks any stale socket file first, so anyone who can unlink in that directory can take over
the endpoint. Put the socket somewhere only the intended peers can write.

### `class UDPSocket`

| Member | Signature | Description |
|---|---|---|
| `new UDPSocket({port, host?})` | | Binds immediately. `port: 0` takes an OS-assigned port. |
| `.start({message})` | `(object) → void` | `message(bytes, from)` where `from` is `{address, port}`. |
| `.send(data, host, port)` | `(string\|Uint8Array\|ArrayBuffer, string, number) → number` | Returns the bytes sent. `host` must be an IPv4 literal — no name resolution. |
| `.port` | getter | The bound port. |
| `.close()` | | As above. |

**A zero-length datagram is real data and is delivered.** Unlike a stream read, where `0` means the
peer closed, `0` here is an empty packet — `message` runs with an empty `bytes`.

---

## DNS — `DNSResolver`, `DNSServer`

`import { DNSResolver, DNSServer } from "dyna:net";`

An RFC 1035 codec in pure C with the sockets on the shared reactor. The parser is an untrusted-input
surface — a DNS message is whatever a peer sends, and name compression makes it a pointer-chasing
format — so it is iterative, a compression pointer must point **strictly backward** (which makes a
loop unrepresentable rather than merely detected), and the 255-octet name and 63-octet label caps are
enforced *while* decoding rather than after.

### `class DNSResolver`

| Member | Signature | Description |
|---|---|---|
| `new DNSResolver({server?, port?, timeoutMs?})` | | `server` defaults to `127.0.0.1`, `port` to 53, `timeoutMs` to 5000. |
| `.query(name, type, cb)` | `(string, number, function) → void` | `cb(err, records)`. `type` is the numeric RR type: 1 = A, 28 = AAAA, 5 = CNAME, 15 = MX, 16 = TXT, 2 = NS, 12 = PTR. |
| `.close()` | | Releases this holder's reference to the shared reactor. Mandatory. |

A record is `{name, type, ttl, address?}` — `address` is present for A and AAAA.

**Four independent defences against a forged answer**, because UDP has no return path a client can
trust: the socket is `connect()`ed so the kernel drops datagrams from any other source; the source
port is randomised by binding port 0; the transaction id comes from the OS entropy source, not a
PRNG; and the question in the reply must match the question that was asked, byte for byte. An answer
failing any of them is discarded and the query goes on to time out — **a timeout, not someone else's
address**, is the correct outcome of a spoofing attempt.

**A truncated (TC) answer is retried over TCP** once. The TCP length prefix is two octets and does
**not** count itself.

### `class DNSServer`

| Member | Signature | Description |
|---|---|---|
| `new DNSServer({port, host?})` | | `port: 0` takes an OS-assigned port, readable from `.port`. |
| `.start(fn)` | `(function) → void` | `fn(name, type)` returns an address string, or `null` for "no such name". |
| `.port` | getter | The bound port. |
| `.close()` | | As above. |

A name the handler does not know comes back as an **empty answer**, which is a different thing from
a timeout and from an error: the client sees zero records.

**Per-source rate limiting is on by default** — a token bucket of 20 queries a second across 64
source slots. An open resolver is an amplification weapon, and the bound is what stops one query
from a forged source address becoming a flood at someone else.

<!-- check:skip -->
```js
const r = new DNSResolver({ server: "1.1.1.1" });
r.query("example.com", 1, (err, recs) => {
  if (!err) print(recs[0].address);
  r.close();
});
```

### `class RateLimiter`

`import { RateLimiter } from "dyna:net";` — a token bucket over a **fixed**,
direct-mapped table.

| Member | Signature | Description |
|---|---|---|
| `new RateLimiter({tokensPerSec, burst?, slots?})` | | `burst` defaults to one second of traffic; `slots` (8 … 2^20, rounded up to a power of two) defaults to 1024. |
| `.allow(key, cost = 1)` | `(string, number?) → boolean` | Spend `cost` tokens if they are there. |
| `.tokens(key)` | `(string) → number` | Tokens currently available; does not spend. |
| `.reset(key?)` | `(string?) → void` | Clear one key, or all of them. |
| `.stats` | getter | `{allowed, denied, slots, live, tokensPerSec, burst}`. |

**The table never grows, and that is the point.** A limiter that allocates a slot
per key hands an attacker with forged keys the memory exhaustion the limiter
existed to prevent. The cost of a bound they cannot move is that two keys can
hash to the same bucket and share a budget — an approximation, chosen
deliberately. Raise `slots` to make collisions rarer; you cannot make the table
unbounded.

Refill is exact integer arithmetic: milli-tokens per millisecond is the same
number as tokens per second, so a bucket cannot drift.

`DNSServer` keeps its own 64-slot IPv4 bucket rather than using this one. Its
policy is fixed, its key is a `uint32` in network order, and it runs in a UDP
receive path where a string key would mean an allocation per packet — which is
exactly what a flood is trying to buy.

```js
const rl = new RateLimiter({ tokensPerSec: 10, burst: 3 });
rl.allow("1.2.3.4");            // true
rl.allow("1.2.3.4", 2);         // true -- the burst is now spent
rl.allow("1.2.3.4");            // false
rl.allow("5.6.7.8");            // true -- a different key, its own budget
```

### `Metrics`

`import { Metrics } from "dyna:net";` — counters, gauges and histograms with a
Prometheus text-format scrape.

| Member | Signature | Description |
|---|---|---|
| `Metrics.counter(name, value = 1, labels?)` | `(string, number?, object?) → void` | Add to a counter. A negative step throws — a counter that decreases is not a counter. |
| `Metrics.gauge(name, value, labels?)` | `(string, number, object?) → void` | Set the current value. |
| `Metrics.histogram(name, value, labels?)` | `(string, number, object?) → void` | Observe a value, in seconds. |
| `Metrics.scrape()` | `() → string` | The exposition body. |
| `Metrics.reset()` | `() → void` | Drop every series. For tests. |

**The registry is fixed at 256 series and says so rather than growing.** A
metric name or a label taken from a request would otherwise let a peer allocate
server memory without bound by varying one field, so a full registry is an
error and never a silent overwrite of another series.

The layout is the reason this is native. Each series is **exactly one 64-byte
cache line** and the array is aligned, so two counters bumped by two worker
threads never share a line — without that, unrelated counters ping-pong the
line between cores and the "free" increment costs more than the work it
measures. The name and label strings live in a separate array, written once at
registration and read only by `scrape()`. Increments are `memory_order_relaxed`:
telemetry orders nothing, and a barrier here would be the entire cost.

Histogram buckets are **cumulative**, as the format requires: `le="0.1"` counts
every observation at or below 0.1, not just those between the previous bound
and this one. Bounds are 0.005, 0.01, 0.05, 0.1, 0.5 and 1.0 seconds.

```js
Metrics.counter("reqs_total", 1, { code: "200" });
Metrics.gauge("queue_depth", 7);
Metrics.histogram("lat", 0.07);
Metrics.scrape().indexOf("# TYPE reqs_total counter") >= 0;   // true
```

---

## SQLite — `SQLite`

`import { SQLite } from "dyna:net";`

A binding to the system SQLite. It is **linked, not vendored**, so the version is whatever the build
found — read `.version` rather than assuming.

| Member | Signature | Description |
|---|---|---|
| `new SQLite(path, {readonly?, bigint?})` | | `path` may be `":memory:"`. Opened with `SQLITE_OPEN_NOMUTEX`: one connection belongs to one thread. |
| `.query(sql, params?)` | `(string, array?) → Array<object>` | Rows as plain objects keyed by column name. |
| `.exec(sql, params?)` | `(string, array?) → number` | For statements with no result set; returns the number of rows changed. |
| `.lastInsertRowId` | getter | |
| `.version` | getter | The library version actually linked. |
| `.close()` | | |

**Parameters are bound, never interpolated.** There is no code path that splices a value into the
statement text, so a value containing `'; DROP TABLE` is a string. A count mismatch between the
placeholders and the array is **refused** rather than filled with NULLs — silently binding a missing
parameter as NULL turns a caller's bug into a wrong answer.

**All five storage classes round-trip.** `INTEGER` and `REAL` are numbers, `TEXT` a string, `NULL`
`null`, and a `BLOB` is a **`Uint8Array`** — not a bare `ArrayBuffer`, which has no `.length` and no
indexing, so a caller's loop over one runs zero times and reports success. Binding is the mirror: a
typed array, `DataView` or `ArrayBuffer` binds as a `BLOB`, and **every other object is refused** by
name rather than stringified, because `{a:1}` stores cleanly as `"[object Object]"` and a
`Uint8Array` as `"104,105"`.

**An integer past 2^53 comes back as a string.** SQLite integers are 64-bit and a JavaScript number
is a double, so returning one as a number would round it; the exact digits are worth more than the
type. **`bigint: true`** returns every integer as a `BigInt` instead — exact *and* typed — and binds a
`BigInt` parameter losslessly to the full int64 range. It is opt-in because `JSON.stringify` throws
on a `BigInt`.

```js
const db = new SQLite(":memory:");
db.exec("CREATE TABLE t (name TEXT, score REAL)");
db.exec("INSERT INTO t VALUES (?, ?)", ["alice", 1.5]);
db.query("SELECT * FROM t WHERE name = ?", ["alice"]);   // [{name:"alice", score:1.5}]
db.close();
```

---

## Redis — `Redis`

`import { Redis } from "dyna:net";`

A RESP2/RESP3 client for Redis and Redis-compatible servers. It runs entirely on the JS thread
through the shared reactor — a request/response protocol on a ready socket is not work worth handing
to another thread. Every command returns a `Promise`.

The connection speaks **RESP3** when the server supports it. `HELLO 3` is sent before any of your
commands; a server that refuses it (Redis before 6.0) is downgraded to RESP2 and the client
authenticates with a separate `AUTH`. `.protocol` reports `2` until the handshake settles, so test
`.ready` first if the distinction matters.

**No TLS.** `tls: true` is refused by name at construction rather than downgraded to plaintext, and a
peer that answers with a TLS record is reported as *that* rather than as a protocol error. Terminate
TLS in front of the server, or use a plaintext endpoint.

### `class Redis`

**`new Redis(options?)`**

| Field | Type | Description |
|---|---|---|
| `options.host` | `string` | Default `"127.0.0.1"`. |
| `options.port` | `number` | Default **6379**. |
| `options.path` | `string` | Connect over **AF_UNIX** to this path instead of TCP. Takes precedence over `host`/`port`. |
| `options.username` | `string` | ACL user, sent as part of `HELLO 3 AUTH` or as `AUTH user pass` after a downgrade. |
| `options.password` | `string` | With no `username` the user is `"default"` — the pre-ACL `requirepass` case. |
| `options.db` | `number` | `0`–`255`; a non-zero value issues `SELECT` during the handshake. **Throws** `RangeError` outside the range. |
| `options.binary` | `boolean` | Bulk and verbatim replies as `Uint8Array` rather than `string`. Default `false`. |
| `options.bigint` | `boolean` | Integer and big-number replies as `BigInt` rather than `number`/`string`. Default `false`, because `JSON.stringify` throws on a `BigInt`. |
| `options.maxReplyBytes` | `number` | Cap on a **single** bulk payload, and the basis for the cap on an aggregate\'s element count. Default **67108864** (64 MiB). |
| `options.maxPending` | `number` | Commands in flight. Default **4096**. |
| `options.connectTimeoutMs` | `number` | Deadline for reaching a *usable* connection — the TCP connect **and** the handshake. Default **10000**; `0` disables. |
| `options.commandTimeoutMs` | `number` | Deadline for the command at the head of the queue. Default `0` — off. |
| `options.tls` | `boolean` | Refused; a truthy value **throws** `TypeError`. |

| Member | Signature | Description |
|---|---|---|
| `.command(name, ...args)` | `(string, ...(string\|number\|Uint8Array)) → Promise` | One command. Rejects with an `Error` if the server replies with one. |
| `.pipeline(commands)` | `(Array<Array>) → Promise<Array>` | N commands in one round trip, one promise for all the replies, in order. |
| `.on(event, handler)` | `(string, function) → this` | `"push"` (alias `"message"`) and `"error"`. One handler per event; a second call replaces the first. |
| `.protocol` | getter | `3` once `HELLO 3` was accepted, `2` otherwise. |
| `.ready` | getter | Whether the handshake has finished. |
| `.pending` | getter | Commands in flight, in both queues. |
| `.close()` | | Release. Also `.dispose()` and `[Symbol.dispose]()`. |

**Arguments are length-prefixed, always.** Every command goes out in the array-of-bulk-strings form,
so a `\r\n` inside a value is data and cannot begin a second command. The inline form — the one that
makes command injection possible in clients that use it — is never emitted. A `number` is sent as its
decimal text; a `Uint8Array` as its bytes, unchanged.

**A malformed reply destroys the connection.** Replies are matched to commands by position alone, so
a client that tries to recover its place after a parse error is a client that can return one key\'s
value for another. An unknown type byte, a length past `maxReplyBytes`, nesting past 32, or a reply
arriving with no command outstanding each close the socket, reject every outstanding promise with a
`CONNECTION` error and fire the `"error"` handler. So does a `commandTimeoutMs` expiry — a timed-out
command cannot be un-sent and its reply would land on the next one — and so does a `NOAUTH` reply
arriving after the connection was ready, since authentication was revoked underneath it.

**`maxPending` counts both queues.** Commands issued before the handshake completes are held, so
gating only on what is on the wire would leave the client unbounded exactly when it cannot drain.
Exceeding it **throws** synchronously. The handshake occupies up to two slots.

| RESP | JavaScript |
|---|---|
| simple `+`, bulk `$` | `string`, or `Uint8Array` for `$` under `binary` |
| verbatim `=` | as bulk, with the three-byte encoding hint and its colon removed — `INFO` gives the report, not `"txt:…"` |
| integer `:` | `number`, or a **`string`** past 2^53, where a `number` would round; `BigInt` under `bigint` |
| big number `(` | `string` — its purpose is to be outside the range a `number` holds; `BigInt` under `bigint`, which is arbitrary-precision and so loses nothing |
| double `,` | `number`; `inf`, `-inf`, `nan` arrive as `Infinity`, `-Infinity`, `NaN` |
| boolean `#` | `boolean` |
| null `_`, `$-1`, `*-1` | `null` |
| array `*`, set `~`, push `>` | `Array` |
| map `%` | plain `Object`. Keys are always strings, including under `binary`; a key of `__proto__` is a key, not a prototype |
| attribute `\|` | skipped — it decorates the reply that follows and is not a reply |
| error `-`, blob error `!` | an `Error` with `.code`, `.message` and `.redis === true` |

**Errors carry their class.** The first token of a Redis error is surfaced as `.code`, so a caller can
branch without matching message text: `ERR`, `WRONGTYPE`, `NOAUTH`, `WRONGPASS`, `NOPERM`, `LOADING`,
`BUSY`, `READONLY`, `MOVED`, `ASK`, `TRYAGAIN`, `CLUSTERDOWN`. A connection-fatal error carries
`.code === "CONNECTION"`. A rejected credential is reported **without the credential in it**.

**A pipeline resolves; its elements carry the errors.** `.command()` rejects on an error reply, but
`.pipeline()` resolves with the full array and puts each error in its own slot — one failing command
does not discard the other replies. Test an element with `instanceof Error`.

**A command that answers once per channel is counted as such.** `SUBSCRIBE`, `PSUBSCRIBE`,
`SSUBSCRIBE` and their `UN`-forms produce one reply **per channel named**, so `SUBSCRIBE a b` consumes
two replies and resolves with both confirmations; `.pipeline()` sums the same way. An `UNSUBSCRIBE`
with **no channel** is **refused**: it answers once per channel the *server* believes you hold, a
count the client cannot know, and guessing is what puts a positional protocol one step out of phase.

**Pub/sub.** A subscribe resolves from its own confirmation; the messages that follow reach the
`"push"` handler as `["message", channel, payload]` or `["pmessage", pattern, channel, payload]`.
Under RESP2 a delivery is an ordinary array and only that first element distinguishes it from a
reply, so the client tracks how many channels it holds; under RESP3 it is a typed push.

**RESP2 restricts a subscribed connection, and so does this client.** Once subscribed on RESP2 only
`PING`, `QUIT`, `RESET` and the subscribe/unsubscribe commands are legal; anything else is **refused
locally** with a `TypeError` naming the rule. That also keeps a stray command from being in flight
when a delivery arrives, which is the shape that desynchronises the queue. RESP3 has no such
restriction and none is applied.

**Every handle must be closed.** A live `Redis` holds a reference that keeps the event loop alive.

<!-- check:skip -->
```js
const r = new Redis({ host: "127.0.0.1", port: 6379 });

await r.command("SET", "k", "v");
await r.command("GET", "k");                       // "v"
await r.pipeline([["INCR", "n"], ["GET", "k"]]);   // [1, "v"]

try {
  await r.command("LPUSH", "k", "x");
} catch (e) {
  e.code;                                          // "WRONGTYPE"
}

r.on("push", ([kind, channel, payload]) => { /* ... */ });
await r.command("SUBSCRIBE", "news");

r.close();
```

---

## PostgreSQL — `PostgreSQL`

`import { PostgreSQL } from "dyna:net";`

A PostgreSQL frontend/backend protocol client. Same discipline as `Redis`: on the JS thread through
the shared reactor, one `Promise` per query, a strict FIFO that never resynchronises.

**Protocol version 3.0.** Version 3.2, new in PostgreSQL 18, changes nothing a client here wants —
its one visible difference is a longer cancel key — so requesting 3.0 gets identical behaviour from
every server back to 7.4. `NegotiateProtocolVersion` is still consumed if a pooler sends one, because
a client that skips it reads the next message at the wrong offset. The cancel key is sized **from its
own message**: 4 octets on 3.0, 32 on 3.2, up to 256 from a pooler.

**No TLS.** `tls: true` is refused by name. The authentication note below is the reason, not an
apology.

### `class PostgreSQL`

**`new PostgreSQL(options?)`**

| Field | Type | Description |
|---|---|---|
| `options.host` | `string` | Default `"127.0.0.1"`. |
| `options.port` | `number` | Default **5432**. |
| `options.path` | `string` | Connect over **AF_UNIX**. Takes precedence over `host`/`port`. |
| `options.user` | `string` | Default `"postgres"`. |
| `options.password` | `string` | Used for SCRAM-SHA-256. |
| `options.database` | `string` | Omitted, the server defaults it to the user name. |
| `options.applicationName` | `string` | Sent as `application_name`; shows in `pg_stat_activity`. |
| `options.raw` | `boolean` | Every column as its text, with no conversion. Default `false`. |
| `options.insecureAuth` | `boolean` | Permit cleartext and MD5. Default `false`, which **refuses** both. |
| `options.maxMessageBytes` | `number` | Cap on one backend message. Default **67108864** (64 MiB). |
| `options.maxPending` | `number` | Queries in flight. Default **1024**. |
| `options.connectTimeoutMs` | `number` | Deadline for reaching `ReadyForQuery` — connect, authentication and startup together. Default **10000**. |
| `options.queryTimeoutMs` | `number` | Deadline for the query at the head of the queue. Default `0` — off. |
| `options.tls` | `boolean` | Refused; a truthy value **throws** `TypeError`. |

| Member | Signature | Description |
|---|---|---|
| `.query(sql)` | `(string) → Promise<Result>` | The **Simple Query** protocol. |
| `.query(sql, params)` | `(string, Array) → Promise<Result>` | The **Extended Query** protocol — `Parse`/`Bind`/`Describe`/`Execute`/`Sync` — so each parameter is a *value*. At most 65535. |
| `.cancel()` | `() → void` | Ask the server to cancel what this connection is running. **Throws** before the server has sent a cancel key. |
| `.on(event, handler)` | `(string, function) → this` | `"notice"`, `"notification"`, `"error"`. |
| `.ready` | getter | Whether the handshake has finished. |
| `.pending` | getter | Queries in flight, in both queues. |
| `.backendPid` | getter | The backend\'s process ID, from `BackendKeyData`. |
| `.transactionStatus` | getter | `"I"` idle, `"T"` in a transaction block, `"E"` in a failed one. |
| `.parameters` | getter | The server\'s run-time parameters. A **live view**, not a snapshot: the server updates it mid-session. |
| `.close()` | | Sends `Terminate`, then releases. |

**`Result`** is `{ rows, fields, command, rowCount }`. A row is a plain object keyed by column name —
a column named `__proto__` is a key, not a prototype. A field is `{name, tableOid, column, typeOid,
format}`. `command` is the server\'s tag (`"SELECT 1"`, `"INSERT 0 3"`); `rowCount` is the number of
`DataRow` messages.

**Parameters are values, and the placeholders are the server\'s.** Write `$1`, `$2` — not `?`. They go
out length-prefixed in a `Bind` with no type OIDs, so the server infers each type from where it is
used. `null` and `undefined` become SQL `NULL` (a length of `-1`), which is not the empty string. **A
`Uint8Array`, any typed array, a `DataView` or an `ArrayBuffer` is bytea**, encoded as the `\x` hex
literal the server parses whatever `bytea_output` is set to. **Every other object is refused**:
stringifying one gives `[object Object]` or `104,105`, both of which store cleanly and are both wrong
— pass `JSON.stringify(v)` for json or an ISO string for a timestamp.

**Results come back in BINARY for the types that gain by it, and only those.**
Binary is not a blanket win: for a fixed-width or text-expanded type it removes
the server's formatting, the wire bytes and the client parse, but for a
**text-like** type it *costs* about 16%, because `textout` returns a pointer
where `textsend` makes an extra copy. So `bool`, `int2`, `int4`, `int8`,
`float4`, `float8`, `oid`, `uuid` and `bytea` are requested binary per column,
and `text`, `varchar`, `json`/`jsonb`, `numeric` and the timestamps stay text.

This needs the result column types *before* `Bind`, and they only arrive in a
`RowDescription` — so it is the statement cache that makes it free: the types
are learned on a statement's first execution and used from the second onward.
Decoding follows the format the **server** reported, not the one requested, and
every width is checked against the declared type. `textResults: true` forces the
old all-text path. **Measured with the decode dominating: ~1.07-1.09x on a
numeric-heavy row (six columns, survives reversing the A/B order), and parity on
a text-heavy one** — parity is the design working, since those columns were
never converted.

**Repeated queries are promoted to prepared statements, and it is a strategy rather than a switch.**
An *unnamed* statement is one round trip that leaves no server-side state — right for a one-shot
query. A *named* one lets the server keep the parse and the plan — right once the same SQL text has
been seen `prepareAfter` times (default **2**; promoting on the first sighting would pay for state a
one-shot query never uses). `statementCache` reports `{size, max, prepareAfter, preparedHits,
unnamed}` so a caller can see which arm ran.

| Option | Default | Description |
|---|---|---|
| `statementCacheSize` | `64` | Statements held per connection. **`0` disables it**, which PgBouncer in transaction mode requires — it cannot carry a named statement across a pooled connection. A full cache stays unnamed rather than thrashing. |
| `prepareAfter` | `2` | Sightings of one SQL text before promoting it. |

**Measured, pipelined at depth 300 so the round trip does not hide the server's work:** a trivial
`SELECT $1::int` goes **19–27 µs → 11 µs per query (1.7–2.4×)**, while a query that actually does
something — a `generate_series` with `md5`, a sort and a limit — moves only **398 → 386 µs (1.01–1.05×)**.
That ordering is the point and it is the opposite of the intuition: the cache saves parse and plan,
which is nearly all of a cheap query's cost and a rounding error on an expensive one. At depth 1 both
read as ~1.03× because 96% of that number is the round trip. Issuing 1500 *distinct* one-shot
statements costs about **1%** against a disabled cache, so the arm that cannot help does not tax.

**A schema change is handled, not merely survived.** PostgreSQL refuses a cached plan whose result
type changed (`0A000`), and the name may not exist at all (`26000`); any error on a named statement
evicts it, so the next call re-parses. Without that eviction the query fails identically for ever —
which is what the test injects to prove the check is load-bearing.

**Passing an array chooses the protocol, not its length.** `query(sql, [])` takes the extended path
just as `query(sql, [1])` does. The two paths differ in more than parameters: the simple one runs
several statements separated by semicolons and the extended one refuses them, so choosing by length
would mean an empty array silently left the parameterised path.

**One statement per query.** A second result set is **refused** rather than merged — merging gives
one `rows` array whose shape changes partway through, with `fields` describing only the last part.

**Type mapping, and where it deliberately stops.** Results are requested in **text** format:

| PostgreSQL | OID | JavaScript |
|---|---|---|
| `bool` | 16 | `boolean` |
| `int2`, `int4` | 21, 23 | `number` |
| `float4`, `float8` | 700, 701 | `number` |
| `int8` | 20 | `number`, or the **text** past 2^53 where a double rounds; `BigInt` under `bigint: true` |
| `bytea` | 17 | the `\x` hex **text**; a `Uint8Array` under `bytes: true` |
| `oid`, `uuid` | 26, 2950 | `number`, `string` |
| a `NULL` column | — | `null` |
| everything else | — | `string` |

Everything else means `numeric`, `timestamp`, `date`, `interval`, `json`, `uuid`, arrays and every
user type — and it is a decision, not a gap. Text is **exact**: `numeric '0.10'` is not the double
`0.1` and is not `"0.1"`, and a `timestamptz` carries an offset a `Date` cannot represent. Parse what
you need in the caller, where the intent is known. `raw: true` extends that to every column.

**`bigint: true` makes every `int8` a `BigInt`, not only the large ones** — a type that changes with
the value is worse than one that is always wide. Note `JSON.stringify` throws on a `BigInt`, which is
why this is opt-in rather than the default. **`bytes: true` decodes `bytea`** and falls back to the
text if the server is set to `bytea_output = escape`, so a half-decoded value is never returned.

**A framing failure destroys the connection.** A message length below 4 (which unchecked would make
the read loop rewind rather than advance), a length past `maxMessageBytes`, an unknown type byte, or a
row that contradicts its own length each close the socket and reject everything outstanding. So does a
`queryTimeoutMs` expiry, because a timed-out query\'s rows would land on the next one — use `.cancel()`
for the graceful form. COPY is not supported and is **named as such** rather than reported as a
framing fault.

**An error rejects with its SQLSTATE.** An `ErrorResponse` is held until the `ReadyForQuery` that ends
the query — the server keeps sending until then, and settling early leaves those messages for the next
query to misread — then rejects with an `Error` carrying `.code` (the SQLSTATE, five characters, never
localised — branch on this), `.message`, `.severity`, `.detail`, `.hint`, `.position` (1-based, **in
characters**), and `.schema`/`.table`/`.column`/`.constraint` for constraint violations. Unrecognised
field types are ignored, as the protocol requires.

**`.cancel()` goes out on a fresh connection**, because the busy one is not reading. It returns
immediately: the server processes the request and closes without answering, so there is nothing to
wait for and no way to tell from here whether it took effect. The cancelled query rejects through its
own promise.

**Notices and notifications are out of band.** A `RAISE NOTICE` reaches the `"notice"` handler and does
not affect the query it arrived during; a `NOTIFY` reaches `"notification"` as `{pid, channel,
payload}`. Both are dropped with no handler installed.

### Authentication without TLS

**SCRAM-SHA-256 only.** A server asking for cleartext or MD5 is **refused by name** unless
`insecureAuth` is set: on a plaintext link a cleartext password is handed to anything on the path, and
MD5 is a replayable scheme over a broken digest. Channel binding requires a TLS channel to bind to, so
the gs2 flag is a truthful `n` — the spelling that says "this client does not do channel binding" —
rather than the `y` a TLS-capable client sends.

**What SCRAM protects here.** The password never crosses the wire in any form, and a passive listener
gets nothing replayable, because the proof is bound to both nonces. The **server is authenticated
too**: only a party holding the key derived from the password can produce the `ServerSignature`, and
this client verifies it with a constant-time compare, requires the server\'s nonce to extend its own,
and **refuses a server that skips the final message** rather than treating the proof as optional. It
also bounds everything the server chooses — iteration count, salt length, message size — because those
are attacker-controlled work.

**What it does not protect.** It does not stop a **man in the middle**: PostgreSQL\'s own documentation
says a relay can pass the server\'s random value through and authenticate, and without channel binding
nothing detects that. A passive listener can mount an **offline dictionary attack** on the captured
exchange, bounded only by the iteration count. And everything after the handshake — every statement,
parameter and row — is **plaintext and modifiable in flight**. SCRAM protects the credential, not the
session.

**`SCRAM-SHA-256-PLUS` in the server\'s mechanism list is not usable information here.** A server
advertises it whenever it was built with SSL support, whether or not *this* connection is encrypted, so
it is not consulted. The corollary is worth stating: a man in the middle can **strip** it and this
client cannot tell, because detecting that downgrade is exactly what the channel-binding variant exists
for. That is the cost of having no TLS.

If any of this matters, terminate TLS in front of the server, or use `path` and keep the traffic off
the network entirely.

<!-- check:skip -->
```js
const db = new PostgreSQL({ host: "127.0.0.1", port: 5432,
                          user: "app", password: "…", database: "shop" });

const r = await db.query("SELECT id, name FROM users WHERE active");
r.rows[0].name;        // "ada"
r.command;             // "SELECT 2"

await db.query("INSERT INTO events(kind, at) VALUES ($1, now())", ["login"]);

try {
  await db.query("SELECT * FROM nope");
} catch (e) {
  e.code;              // "42P01"
  e.position;          // "15"
}

db.on("notification", ({ channel, payload }) => { /* ... */ });
await db.query("LISTEN jobs");

db.close();
```

---

## Addresses and CIDR — `Prefix` and the address functions

`import { parseAddr, parsePrefix, contains, masked, canonical, isValid, compareAddr, isLoopback, isPrivate, isMulticast, isUnspecified, isLinkLocalUnicast, isGlobalUnicast, isLinkLocalMulticast } from "dyna:net";`

IPv4/IPv6 address and CIDR-prefix parsing, comparison and classification.

### `parseAddr(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | An IPv4 dotted-quad, an IPv6 address (compressed or full), or an IPv4-mapped IPv6 address. |

**Returns** `{ is4: boolean, is6: boolean, bytes: Uint8Array, string: string }` — `bytes` is 4 or 16 bytes; `string` is the canonical form. **Throws** `SyntaxError` on an invalid address. Note: an IPv4-mapped IPv6 address (`"::ffff:1.2.3.4"`) parses as an **IPv6** value (`is4: false`, 16 bytes); the classifiers below unmap it first.

### `parsePrefix(s)`

| Parameter | Type | Description |
|---|---|---|
| `s` | `string` | A CIDR prefix, e.g. `"10.0.0.0/8"` or `"2001:db8::/32"`. |

**Returns** `{ addr, bits: number }`. **Throws** on an invalid prefix or out-of-range length.

### `contains(prefix, addr)`

| Parameter | Type | Description |
|---|---|---|
| `prefix` | `string` | A CIDR prefix. |
| `addr` | `string` | An address. |

**Returns** `boolean` — whether `addr` falls within `prefix` (comparing only the masked bits).

### `masked(prefix)` · `canonical(addr)` · `isValid(s)` · `compareAddr(a, b)`

| Function | Signature | Description |
|---|---|---|
| `masked(prefix)` | `(string) → string` | The network address with host bits zeroed. |
| `canonical(addr)` | `(string) → string` | The RFC 5952 canonical form (lowercase, `::`-compressed). |
| `isValid(s)` | `(string) → boolean` | Validity test (no throw). |
| `compareAddr(a, b)` | `(string, string) → -1\|0\|1` | Ordering of two addresses. |

### Classifiers

Each takes an address string and returns a `boolean`: `isLoopback`, `isPrivate`, `isMulticast`,
`isUnspecified`, `isLinkLocalUnicast`, `isGlobalUnicast`, `isLinkLocalMulticast`. All except
`isUnspecified` first unmap an IPv4-in-IPv6 address (so `isLoopback("::ffff:127.0.0.1")` is `true`).

```js
contains("10.0.0.0/8", "10.1.2.3");   // true
canonical("2001:0db8:0000:0000:0000:0000:0000:0001"); // "2001:db8::1"
isPrivate("192.168.1.1");             // true
```

---

### `class Prefix` — a compiled CIDR (parse once, test many)

`contains(prefix, addr)` re-parses the CIDR text on every call. An ACL, a firewall rule and a rate
limiter each ask the same question once per request against a prefix fixed at startup, so
`new Prefix` parses **and masks** once and `.contains()` is a masked compare.

Because masking happens at construction, `new Prefix("10.1.2.3/8")` and `new Prefix("10.0.0.0/8")`
are indistinguishable afterwards. Plain garbage-collected object, read-only, freely reusable.

| Member | Signature | Description |
|---|---|---|
| `new Prefix(cidr)` | `(string)` | Compile. **Throws** `TypeError` on a malformed CIDR. |
| `.contains(addr)` | `(string) → boolean` | Whether `addr` is in the prefix. An unparseable address, or one of the other family, is `false`. |
| `.overlaps(other)` | `(Prefix) → boolean` | Whether two prefixes intersect. **Throws** `TypeError` unless `other` is a `Prefix`. Different families never overlap. |
| `.masked` | `string` (getter) | The canonical network address. |
| `.bits` | `number` (getter) | The prefix length. |
| `.isIPv4` | `boolean` (getter) | Address family. |

**Crossover: N = 10**, reaching 1.33× at 1000 uses.

```js
const lan = new Prefix("10.0.0.0/8");
lan.contains("10.1.2.3");            // true
lan.overlaps(new Prefix("10.1.0.0/16"));  // true
```

---

# time

`import { Nanosecond, Microsecond, Millisecond, Second, Minute, Hour, durationString, parseDuration, now, nowUnixNano, nowMillis, monotonicNano, formatRFC3339, parseRFC3339, formatUnix, date, fromUnix, Format, DateParser } from "dyna:time";`

Nanosecond-precision durations, a monotonic clock, and RFC 3339 formatting.

### Duration constants

`Nanosecond`, `Microsecond`, `Millisecond`, `Second`, `Minute`, `Hour` — each the number of
nanoseconds in that unit (used to build durations, e.g. `90 * Number(Minute)`).

### `durationString(ns)` · `parseDuration(s)`

| Function | Signature | Description |
|---|---|---|
| `durationString(ns)` | `(number\|BigInt) → string` | Format a nanosecond duration, e.g. `"1h30m0s"`. |
| `parseDuration(s)` | `(string) → number\|BigInt` | Parse a duration string (`"1h30m"`, `"500ms"`, `"1.5s"`) to nanoseconds. |

### Clocks

| Function | Signature | Description |
|---|---|---|
| `now()` | `() → number` | Current Unix time in milliseconds. |
| `nowUnixNano()` | `() → BigInt` | Current Unix time in nanoseconds. |
| `nowMillis()` | `() → number` | Current Unix time in milliseconds. |
| `monotonicNano()` | `() → BigInt` | A monotonic clock reading (nanoseconds); never decreases — use it for elapsed-time and timeouts. |

### Formatting & construction

| Function | Signature | Description |
|---|---|---|
| `formatRFC3339(t)` | `→ string` | RFC 3339 / ISO‑8601 timestamp. |
| `parseRFC3339(s)` | `(string) → …` | Parse an RFC 3339 timestamp. |
| `formatUnix(sec, layout)` | `(number, string) → string` | Format a Unix-second time with a reference-time layout (see `Format` below). |
| `date(year, month, day)` | | Construct a time value from calendar fields. |
| `fromUnix(sec, nsec?)` | | Construct a time value from Unix seconds (+ optional nanoseconds). |

```js
durationString(90 * Number(Minute));    // "1h30m0s"
parseDuration("1h30m");                  // 5400000000000
formatRFC3339(fromUnix(0, 0));           // "1970-01-01T00:00:00Z"

const t0 = monotonicNano();
/* work */
const ms = Number(monotonicNano() - t0) / 1e6;
```

### `class Format` — a compiled layout

Scans a layout once and reuses it for unbounded timestamps, and adds the one thing the free functions
cannot do: **parsing an arbitrary layout**. `parseRFC3339` handles exactly one shape.

**`new Format(layout)`** — the layout is written by spelling out the **reference time**
`Mon Jan 2 15:04:05 2006`, the same convention `formatUnix` uses. The tokens are therefore `2006`
`01` `02` `15` `04` `05` `Jan` `Mon`. Everything else is a literal, including strings that merely
look like tokens (`"20"`, `"Ja"`, `"January"`).

| Member | Signature | Description |
|---|---|---|
| `.format(unixSec)` | `(number) → string` | Always UTC. |
| `.parse(str)` | `(string) → number` | Unix seconds. `SyntaxError` if the input does not match. |
| `.layout` | `string` (getter) | |

```js
const iso = new Format("2006-01-02T15:04:05Z");
iso.format(1735689600);            // "2025-01-01T00:00:00Z"
iso.parse("2025-01-01T00:00:00Z"); // 1735689600

const eu = new Format("02.01.2006");
eu.parse("15.03.2025");
```

**`parse` is strict**, which is what makes it round-trip: literals must match byte for byte, each
field is exactly as wide as `format` writes it, and trailing input is a mismatch. A field the layout
does not mention takes its value from `1970-01-01T00:00:00Z`, so `new Format("15:04").parse("12:30")`
is 45 000.

The year is read as **exactly four digits plus an optional sign**. `format` writes wider years when
it has to, so a year outside ±9999 formats correctly and does not parse back — the alternative, a
greedy year, cannot parse a layout with no separators at all (`"2006010215:04:05"`).

**Speed is the smaller reason to use it.** A compiled layout skips coercing and re-scanning the
layout string, but the per-token output still has to happen, so the win is modest and it takes a
handful of uses to appear. Read-only compiled state, so one instance is freely shared.

### `class PlainDate` and `class Duration`

A calendar date with **no time and no zone** — the type for a birthday, an
invoice date or a deadline, none of which have an instant.

| Member | Signature | Description |
|---|---|---|
| `new PlainDate(y, m, d)` | `(number, number, number)` | Proleptic Gregorian. An impossible date throws. |
| `parseDate(text)` / `dateFromEpochDay(n)` | `→ PlainDate` | ISO 8601 `YYYY-MM-DD`, or days since 1970-01-01. |
| `.year` `.month` `.day` `.dayOfWeek` `.dayOfYear` | getters | `dayOfWeek` is ISO: Monday is 1. |
| `.daysInMonth` `.daysInYear` `.inLeapYear` `.epochDay` | getters | |
| `.add(dur)` / `.subtract(dur)` | `(Duration) → PlainDate` | |
| `.until(other)` | `(PlainDate) → Duration` | |
| `.compare(other)` | `(PlainDate) → -1\|0\|1` | |
| `new Duration({years?, months?, weeks?, days?, hours?, minutes?, seconds?, milliseconds?})` | | `.months` `.days` `.years` `.sign` `.blank`, `toString()` as ISO 8601. |
| `new PlainTime(h, m?, s?, ms?)` | | Time of day, no date and no zone. `.hour` `.minute` `.second` `.millisecond` `.msSinceMidnight`, `.add`/`.subtract`/`.compare`/`.toString`. |
| `parseTime(text)` | `→ PlainTime` | `HH:MM[:SS[.mmm]]`. |
| `new PlainDateTime(y, m, d, h?, mi?, s?, ms?)` | | A date and a time, still no zone. `.toPlainDate()` `.toPlainTime()`, plus every getter of both. |

**31 February is refused, not rolled into March.** A date type that silently
normalises an impossible date hides the caller's bug.

**Month arithmetic clamps.** 31 January plus one month is the last day of
February — 29th or 28th — never 3 March. Months move first and days second,
and that order is part of the contract: adding `{months: 1, days: 1}` to
2024-01-30 gives 2024-03-01, while adding a day and then a month gives
2024-02-29. A `Duration` therefore keeps months and days **separate** and never
collapses one into the other, because a month is not a fixed number of days.

Years outside 0..9999 are written expanded with a sign and six digits, so
`+012345-06-07` can never be mistaken for a four-digit year.

`PlainTime` **wraps at midnight** — a time of day has nowhere to put the
overflow, and discarding it silently would make the result quietly wrong. It
refuses `24:00`, which is a legal instant only as the *end* of a day, and
refuses a duration in months, which cannot be converted to hours.

Hours, minutes and seconds fold into milliseconds exactly. **Days do not fold
into hours** — that would assume every day is 24 hours, which is the assumption
these types exist to avoid.

`PlainDateTime` exists as its own type rather than a pair because time
overflow **carries into the date**, where `PlainTime` wraps and loses the day.
That is the whole distinction, and it is the example below.

```js
new PlainDateTime(2024, 3, 1, 23, 30).add(new Duration({ hours: 2 })).toString();
                                                   // "2024-03-02T01:30:00" -- carries
new PlainTime(23, 30).add(new Duration({ hours: 2 })).toString();  // "01:30:00"
new Duration({ days: 2, hours: 3 }).toString();                    // "P2DT3H"
new PlainDate(2024, 2, 29).dayOfWeek;                          // 4 -- a Thursday
new PlainDate(2024, 1, 31).add(new Duration({ months: 1 })).toString();  // "2024-02-29"
new PlainDate(2024, 1, 31).until(new PlainDate(2024, 3, 30)).toString(); // "P1M30D"
```

### `class DateParser` — dates a human typed

`new DateParser(locale?, { now? })`. The **locale is the configuration**; one instance parses
unbounded strings. `parse(text)` returns unix seconds, or **`null`** when it recognises nothing —
this reads text a person typed, so "not a date" is an ordinary outcome, and a caller who wants an
error writes one where the failure means something.

| Member | Signature | Description |
|---|---|---|
| `new DateParser(locale?, opts?)` | `(string?, {now?}) → DateParser` | `locale` defaults to `en-US`. |
| `.parse(text)` | `(string) → number \| null` | Unix seconds. |
| `.locale` | `string` (getter) | |
| `.dayFirst` | `boolean` (getter) | Whether `03/04` reads as 3 April or 4 March. |

Locales: `en-US` `en-GB` `en` `fr` `de` `es`. **`en-US` is the only month-first one**, and that is
the whole reason the locale exists: `"03/04/2026"` is 4 March in `en-GB`, `fr`, `de` and `es`, and
3 April in `en-US`. Both readings are well-formed, so a parser that guessed would be silently wrong
for one of them. Month and weekday names come from the same table, so `fr` does not recognise
`"March"` and `en-US` does not recognise `"mars"`.

Forms it reads:

```
2026-07-28   2026/07/28   2026-07-28 14:30   2026-07-28T14:30:05   2026-07-28 2:30 pm
July 28, 2026   28 July 2026   Jul 28 26   Tuesday, 28 July 2026
03/04/2026 (locale order)     4 mars 2026     4. Marz 2026
now   today   tomorrow   yesterday
in 3 days   90 minutes ago   in 2 months   1 year ago
next monday   last friday
```

Two-digit years use the POSIX window — 69 and below are 2000s. Calendar months **clamp**: 31 January
plus one month is 28 February, never 3 March. `{ now }` fixes the instant relative words resolve
against, which is what makes them testable.

It ships because it *expresses* something no free function here could — there was no
natural-language date parser to be faster than. Construction is a table pointer and one allocation, so hoisting a parser out of a loop helps a
little and is not a step change. Rejecting a string that is not a date costs about as much as
parsing one, because it walks every alternative before giving up.



---

# semver

`import { parse, isValid, clean, compare, gt, gte, lt, lte, eq, neq, sort, major, minor, patch, prerelease, inc, satisfies, maxSatisfying, minSatisfying, coerce } from "dyna:semver";`

Semantic Versioning 2.0.0 with npm-style ranges.

### `parse(v)` · `isValid(v)` · `clean(v)`

| Parameter | Type | Description |
|---|---|---|
| `v` | `string` | A version string. |

**Returns** `parse`: `{ major, minor, patch, prerelease: (string|number)[], build: string[], version: string }`; **throws** `SyntaxError` on an invalid version. `isValid`: `boolean` (no throw). `clean`: `string | null` — a normalized version (strips a leading `v`/whitespace), or `null` if not coercible to strict form.

### Comparison

| Function | Signature | Description |
|---|---|---|
| `compare(a, b)` | `(string, string) → -1\|0\|1` | SemVer precedence. A version with a prerelease is **lower** than the release; build metadata is ignored. |
| `gt` `gte` `lt` `lte` `eq` `neq` | `(string, string) → boolean` | The obvious comparisons. |
| `sort(versions)` | `(string[]) → string[]` | A new array sorted ascending by precedence. |

Precedence follows the spec §11 chain, e.g.
`1.0.0-alpha < 1.0.0-alpha.1 < 1.0.0-alpha.beta < 1.0.0-beta < 1.0.0-beta.2 < 1.0.0-beta.11 < 1.0.0-rc.1 < 1.0.0`.

### Field accessors & increment

| Function | Signature | Description |
|---|---|---|
| `major(v)` / `minor(v)` / `patch(v)` | `(string) → number` | The numeric fields. |
| `prerelease(v)` | `(string) → (string\|number)[]` | The prerelease identifiers. |
| `inc(v, release)` | `(string, string) → string` | Increment. `release` ∈ `"major" \| "minor" \| "patch" \| "premajor" \| "preminor" \| "prepatch" \| "prerelease"`. |

### Ranges

| Function | Signature | Description |
|---|---|---|
| `satisfies(version, range)` | `(string, string) → boolean` | Whether `version` satisfies the npm `range`. |
| `maxSatisfying(versions, range)` | `(string[], string) → string \| null` | The highest satisfying version, or `null`. |
| `minSatisfying(versions, range)` | `(string[], string) → string \| null` | The lowest satisfying version. |
| `coerce(s)` | `(string) → string \| null` | Best-effort extract a valid version from loose text. |

`range` supports the full npm grammar: exact versions; comparators `>` `>=` `<` `<=` `=`; caret
`^1.2.3` (with the special 0.x rules); tilde `~1.2.3`; hyphen `1.2.3 - 2.3.4`; x-ranges `1.x` /
`1.2.*` / `*`; conjunction (space) and disjunction (`||`). A prerelease version only satisfies a
range whose comparator set carries a prerelease at the same `major.minor.patch`.

```js
satisfies("1.2.9", "^1.2.3");   // true
satisfies("0.3.0", "^0.2.3");   // false  (^0.2.3 := >=0.2.3 <0.3.0)
inc("1.2.3", "minor");          // "1.3.0"
maxSatisfying(["1.0.0","1.2.0","1.9.0","2.0.0"], "^1.2.0");   // "1.9.0"
```

---

### `class Range` — a compiled range (parse once, test many)

`satisfies(v, range)` re-parses the range string on every call. A dependency solver asks the same
question across thousands of versions against a range that never changes, so `new Range` parses once
and each `.test()` is a comparison against the compiled comparator sets — the same relationship
`new RegExp` has to a literal match.

A plain garbage-collected object: no `.close()`. The compiled state is read-only, so one instance is
freely reusable and safe to share.

| Member | Signature | Description |
|---|---|---|
| `new Range(range)` | `(string)` | Compile. **Throws** `TypeError` on a malformed range — once, at construction, rather than on every use. |
| `.test(version)` | `(string) → boolean` | Whether `version` satisfies the range. An unparseable version is `false`, not a throw. |
| `.maxSatisfying(list)` | `(string[]) → string \| null` | Highest satisfying element, or `null`. |
| `.minSatisfying(list)` | `(string[]) → string \| null` | Lowest satisfying element, or `null`. |
| `.filter(list)` | `(string[]) → string[]` | Every satisfying element, input order preserved. |
| `.source` | `string` (getter) | The range as given. |
| `.setCount` | `number` (getter) | Number of `||`-separated comparator sets it compiled to. |

An unparseable entry in a list **throws** `TypeError` — unlike `.test()`, because a list is validated
up front. An empty range string is valid and means "any version".

**Crossover: N = 10.** One instance beats N free `satisfies()` calls from ten uses onward, reaching
5.9× at 1000. Below that the parse is not amortised; a single check should use `satisfies`.

```js
const r = new Range(">=1.2.3 <2.0.0 || ^3.0.0");
r.test("1.5.0");                     // true
r.filter(["1.0.0","1.5.0","3.1.0"]); // ["1.5.0", "3.1.0"]
```

---

# simd

`import { /* many */ } from "dyna:simd";`

Multi-ISA vector math dispatched at runtime to the best instruction set available (scalar / NEON /
SSE4.2 / AVX2 / AVX‑512 / SVE). Operands are typed arrays. **In-place** operations mutate and return
their first array argument; **output** operations write into a caller-provided destination and
return it. Reductions/products return a scalar.

> AVX2/AVX‑512 variants of the f64/i32/UTF‑16 kernels are currently gated off pending
> AVX-hardware verification; those machines transparently run the verified SSE4.2/AVX2 kernels.

### f32 reductions — `(x: Float32Array) → number`

`sum`, `max`, `min`, `normL1` (Σ\|xᵢ\|), `normL2` (√Σxᵢ²), `argmax`, `argmin` (index of the extreme).

**`argmax`/`argmin` break a tie arbitrarily, and not the same way at every length.** The returned
index always attains the extreme — that is the contract — but *which* of several equal elements is
returned depends on the length and on the ISA: a short input is reduced by a scalar loop that keeps
the FIRST, while a longer one is reduced across vector lanes and keeps a later one. Measured on this
build, an array whose maximum occurs at index 5 and index 20 returns `5` at length 63 and `20` at
length 64. If a specific element matters, break the tie yourself; do not assume the numpy convention.

### f32 pairwise — `(a: Float32Array, b: Float32Array) → number`

`dot` (Σ aᵢbᵢ), `distL1`, `distL2`, `distCos` (cosine **distance** = 1 − cosine similarity),
`distCheb` (Chebyshev / L∞).

### f32 elementwise

| Function | Signature | Effect |
|---|---|---|
| `add`/`sub`/`mul`/`div` | `(z, a, b) → z` | `z = a ⊙ b`, elementwise, into `z`. |
| `fma` | `(z, a, b) → z` | `z = z + a·b`. |
| `abs` | `(out, in) → out` | `out = \|in\|`. |
| `scale` | `(a, s: number) → a` | `a *= s`, in place. |
| `addScalar` | `(a, s: number) → a` | `a += s`, in place. |
| `affine` | `(a, s: number, b: number) → a` | `a = a·s + b`, in place. |
| `clamp` | `(a, lo: number, hi: number) → a` | Clamp each element into `[lo, hi]`. |
| `threshold` | `(a, t: number) → a` | Zero elements below `t`. |

### f32 BLAS

| Function | Signature | Effect |
|---|---|---|
| `axpy(y, alpha, x)` | `(Float32Array, number, Float32Array) → y` | `y += alpha·x`. |
| `gemv(y, A, x, m, n, beta)` | `→ y` | `y = A·x + beta·y`; `A` is m×n row-major. |
| `gemvT(y, A, x, m, n, beta)` | `→ y` | As `gemv` but with Aᵀ. |
| `gemm(C, A, B, m, n, k, alpha, beta)` | `→ C` | `C = alpha·A·B + beta·C`; `A` is m×k, `B` is k×n, `C` is m×n (all row-major). |

### f32 activations / transcendentals (in place, `(x: Float32Array) → x`)

`sigmoid`, `relu`, `relu6`, `tanhFast`, `gelu`, `silu`, `softmax`, `logSoftmax`, `vexp`, `vlog`,
`vsqrt`, `vrsqrt`, `vinv`. Two take a parameter: `leakyRelu(x, slope)`, `elu(x, alpha)`. And
`topkIndices(x, k) → number[]` returns the indices of the `k` largest elements.

### f64 — `(Float64Array)`

`f64Sum(x) → number`, `f64Dot(a, b) → number`, `f64Max(x) → number`, `f64Min(x) → number`,
`f64Scale(x, s) → x` (in place), `f64Axpy(y, a, x) → y` (`y += a·x`).

### i32 — `(Int32Array)`

`i32Sum(x) → number` (widened to int64, exact for \|Σ\| ≤ 2⁵³), `i32Min(x)`/`i32Max(x) → number`,
`i32Dot(a, b) → number`, `i32Add(z, a, b)`/`i32Mul(z, a, b) → z` (two's-complement wrap, like
`Math.imul`), `i32Scale(x, s) → x`.

### Prefix scans

`cumsum(x) → x` (inclusive prefix sum) and `cummax(x) → x` (running maximum), each accepting an
`Int32Array` or `Float32Array`, in place.

```js
dot(new Float32Array([1,2,3,4]), new Float32Array([5,6,7,8]));   // 70
const g = new Float32Array([2,1,0.1]); softmax(g);               // → probabilities
const c = new Int32Array([1,2,3,4]); cumsum(c);                  // c = [1,3,6,10]
```

---

# ml

```js
import {
  LinearRegression, LogisticRegression,                    // linear models
  KNClassifier, KNRegressor,               // neighbours
  DecisionTreeClassifier, DecisionTreeRegressor,            // trees
  RandomForestClassifier, RandomForestRegressor,
  GradientBoostingRegressor, GradientBoostingClassifier,
  XGBRegressor, XGBClassifier,                             // second-order boosting
  SVC,                                                     // kernel machine
  GaussianNB,                                              // naive Bayes
  KMeans, DBScan, GaussianMixture,                         // clustering
  PCA,                                                     // decomposition
  StandardScaler, MinMaxScaler,                            // preprocessing
  CSR,                                                     // sparse input
  accuracy, meanSquaredError, meanAbsoluteError,           // metrics
  r2Score, logLoss, confusionMatrix,
  precision, recall, f1, fbeta, specificity,               // classification metrics
  balancedAccuracy, matthewsCorrcoef, cohenKappa,
  rocAuc, averagePrecision,
  trainTestSplit, kFold, stratifiedKFold,                  // model selection
} from "dyna:ml";
```

Classic models implemented natively. Every model is a resource class; `.fit()` returns `this` for
chaining; call `.close()` when done. The metrics are plain functions.

**Two input forms.** Every `fit`/`predict`/`transform` accepts either an `Array` of rows (each row an
`Array` or a `Float64Array`), which is copied cell by cell, **or** a flat `Float64Array` plus an
explicit shape: `fit(X, y, rows, cols)` / `predict(X, rows, cols)`. The flat form aliases the backing
buffer with **no copy and no per-cell JS crossing**, and is the form to use for anything large.

**Matrix results are shape-in, shape-out.** A method that returns a matrix (`transform`,
`predictProba`, `decisionFunction` for multi-class) returns a flat `Float64Array` of `rows*cols` when
you passed the flat form, and an `Array` of row `Array`s when you passed rows. Vector results are
always an `Array`.

**Class labels are values, not indices.** A classifier fitted on labels `[7, -3.5]` predicts `7` and
`-3.5`; the sorted label list is on `.classes`. Ties in a vote go to the smaller label, so results
are deterministic.

**Numerics.** The inner loops are vectorised, so sums accumulate in several lanes at once rather than
strictly left to right, and the compiler may fuse a multiply-add into a single FMA on targets that
have one. A fitted coefficient can therefore differ from a strictly sequential computation, and
between such targets, in the last ULP. Practically: compare model outputs with a tolerance, not
`===`. A point that is *exactly* equidistant from two `KMeans` centroids may be assigned to either;
the clustering is equivalent either way. (The vectorised and sequential builds are diffed over ~9400
values by `tests/test_ml_oracle.js`, which requires every integer label to agree exactly and bounds
float drift at 1e-12 absolute. Timings and method: `bench/ML_REPORT.md`.)

**Determinism.** Every stochastic algorithm (`KMeans`, the forests, the boosters,
`GaussianMixture`) draws from a seeded generator, so a given seed reproduces a given model exactly.


## `class CSR` — sparse input

A compressed-sparse-rows matrix. `ptr[i]..ptr[i+1]` indexes the nonzeros of row `i` in
`values`/`columns`.

| Member | Signature | Description |
|---|---|---|
| `new CSR(values, columns, rowPointers, cols)` | `(number[], number[], number[], number) → CSR` | Every field is validated. |
| `CSR.fromDense(X, rows?, cols?)` | `(number[][] \| Float64Array, …) → CSR` | Drops exact zeros. |
| `.toDense()` | `() → number[][]` | |
| `.row(i)` | `(number) → number[]` | One row, expanded. |
| `.rows` `.cols` `.nnz` `.density` | getters | |
| `.close()` | `() → void` | Release. |

**`LinearRegression` and `LogisticRegression` take a `CSR` wherever they take an `X`.** Their fits
run over the nonzeros — normal equations over nonzero *pairs*, `O(Σ nnz_i²)` against
`O(rows · cols²)`; gradients `O(nnz)` per iteration against `O(rows · cols)`. The fit is the same
model: only summation order differs, because the terms that are skipped are exactly zero.

**Every other estimator throws and names `.toDense()`.** Expanding a sparse matrix behind the
caller's back is the memory the sparse form exists to avoid — a 100 000 × 50 000 one-hot design is
40 GB dense and 24 MB as a CSR — so the expansion happens on a line the caller wrote.

**CSR pays in proportion to what it skips, and costs when there is nothing to skip.** At a few
non-zeros per row it is an order of magnitude cheaper than the dense fit; at full density it is
*slower*, because an index lookup per element is more work than a contiguous load. Wide designs are
capped by something else again: both paths share an `O(p³)` solve of the `p × p` normal equations, so
past a few hundred columns the solve dominates however sparse the accumulation is.

<!-- check:skip -->
```js
const S = CSR.fromDense(X);          // or new CSR(vals, cols, ptr, nCols)
new LogisticRegression().fit(S, y);
new KMeans(3).fit(S.toDense());      // KMeans has no sparse path — say so explicitly
```

## Sample weights

`fit(X, y, { sampleWeight })` gives each row a non-negative multiplier. An integer weight is
exactly equivalent to repeating that row that many times, and a weight of `0` is exactly equivalent
to deleting it.

<!-- check:skip -->
```js
new LinearRegression().fit(X, y, { sampleWeight: w });
```

**Supported by** `LinearRegression`, `LogisticRegression`, every tree model (`DecisionTree*`,
`RandomForest*`, `GradientBoosting*`), `XGBRegressor`, `XGBClassifier`, `KMeans`, `GaussianNB` and
`StandardScaler`.

**The rest throw**, and each says why. That is deliberate: an option accepted and then ignored is
worse than one refused, because nothing tells you the weights did not apply.

| Refuses | Reason |
|---|---|
| `MinMaxScaler` | Its parameters are the column min and max — order statistics that no positive weight can move. |
| `KNClassifier`, `KNRegressor` | The model *is* the training sample; there is nothing fitted for a weight to enter. |
| `PCA`, `GaussianMixture` | No weighted form; resample instead. |
| `DBScan` | A core point is defined by a *count* of neighbours, so weighting it would be a different algorithm rather than the same one weighted. |
| `SVC` | A per-sample weight is a per-sample bound on `C`, which changes the solver's contract rather than its input. |

What each estimator weights: the trees weight the impurity, the leaf value and the class
distribution, while `minSamplesSplit`/`minSamplesLeaf` keep counting **rows** — a weight is how much
a row matters, not how many rows it is. The boosters also weight their base score, without which the
trees would correct a residual measured from the wrong centre. `KMeans` weights the centroids and
the inertia but not the assignment, which does not depend on how much a point counts. `GaussianNB`
weights the priors and the moments. `StandardScaler` weights the mean and the variance.

| Property | Holds |
|---|---|
| all-ones ≡ no weights | **bit-identical** |
| integer weight ≡ duplicated row | to ~1e-10 relative |
| weight 0 ≡ deleted row | to ~1e-10 relative |
| scaling every weight by a constant | no change |

The two row-set identities hold for every estimator whose fit is a function of the row **set**. A
`RandomForest*` bootstraps row indices, so a fit over 200 rows and a fit over the 150 that survive a
zero weight draw different samples whatever the weights say; the weight-invariance identities still
hold for it, which is what says its weighting is right.

The two identities are not *exact*, and the reason is a deliberate one: the least-squares solver
adds a tiny ridge to `XᵀWX` so a singular system is still solvable, and a ridge is a perturbation.
It is scaled by the mean weight, which is what makes the last row of that table hold — with a fixed
absolute ridge, multiplying every weight by 7 would make the regulariser seven times weaker and the
answer would drift.

For `LogisticRegression` the gradient is divided by the **total weight**, not the row count, for
the same reason: otherwise doubling every weight would double the step. Sample weights compose with
`classWeight` rather than replacing it — both are multipliers on a row's gradient contribution.

Weights must be finite and non-negative, and cannot all be zero. Each of those is a separate error
message naming the offending index.


### `class Pipeline`

Composes transformers and a final estimator into one `fit`/`predict`.

<!-- check:skip -->
```js
const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
p.fit(X, y);
p.predict(Xnew);
```

| Member | Signature | Description |
|---|---|---|
| `new Pipeline(stages)` | `(object[]) → Pipeline` | 1–64 stages. Every stage needs `fit`; every stage **but the last** also needs `transform`. Both are checked at construction. |
| `.fit(X, y?)` | | Fits and transforms each leading stage in turn, then fits the last on the result. Returns `this`. |
| `.predict(X)` · `.predictProba(X)` | | Transforms through the leading stages, then asks the last. |
| `.transform(X)` | | Pushes `X` through the **feature** stages. Includes the last stage if it can transform, stops before it if it cannot. |
| `.stage(i)` | `(number) → object` | Negative indices count from the end. |
| `.length` · `.fitted` · `.estimator` | getters | |
| `.close()` | | |

**What it is for is correctness, not speed** — it runs the same calls you would write by hand, and
`predict` is asserted equal to the hand-written composition. What it removes is the classic leak:
fitting a scaler on the whole dataset and only then splitting, so the test fold's mean is already in
the training statistics and the score comes out optimistic. Inside a Pipeline the scaler is fitted
by `fit` and only *applied* by `predict`, so passing one to `crossValScore` — which takes a factory
and builds a fresh Pipeline per fold — refits the scaler on each training fold.

**It owns its stages by reference.** `close()` releases them; it does not call `close()` on each.
A cascade would make a stage shared with anything else unusable the moment one Pipeline holding it
was closed. A stage nothing else references is collected promptly either way.

`fit` forwards only `(X, y)` to the final stage, so there is nowhere to put
[`sampleWeight`](#sample-weights) — set it on the estimator directly if you need it.

### `class LinearRegression`

Closed-form ordinary least squares via the normal equations.

| Member | Signature | Description |
|---|---|---|
| `new LinearRegression()` | | Create an unfitted model. |
| `.fit(X, y)` | `(number[][], number[]) → this` | Fit to feature rows `X` and targets `y`. |
| `.predict(X)` | `(number[][]) → number[]` | Predict for each feature row. |
| `.close()` | `() → void` | Release. |

### `class LogisticRegression`

Binary classifier, full-batch gradient descent on the sigmoid. Labels are read as 0/1 (any non-zero
value is class 1).

| Member | Signature | Description |
|---|---|---|
| `new LogisticRegression()` | | Create an unfitted model. |
| `.fit(X, y)` | `(number[][], number[]) → this` | Fit to features and binary labels. |
| `.predict(X)` | `(number[][]) → number[]` | Predicted class labels (0 or 1). |
| `.predictProba(X)` | `(number[][]) → number[]` | P(class 1) per row. |
| `.close()` | `() → void` | Release. |

### `class KNClassifier` · `class KNRegressor`

Lazy learners: `fit` keeps a copy of the training set, `predict` scans it.

| Member | Signature | Description |
|---|---|---|
| `new KN*(k?, weights?)` | `(number?, string?)` | `k` defaults to 5. `weights` is `"uniform"` (default) or `"distance"` (weight by 1/distance). Any other string is a `TypeError`. |
| `.fit(X, y)` | `(number[][], number[]) → this` | **Throws** `RangeError` if there are fewer than `k` rows. |
| `.predict(X)` | `(number[][]) → number[]` | Majority label, or mean target. |
| `.close()` | `() → void` | Release. |

Under `"distance"` weighting an exact training point has infinite weight, so it predicts its own
target exactly rather than a blend.

### `class DecisionTreeClassifier` · `class DecisionTreeRegressor`

CART: Gini impurity for classification, variance reduction for regression. Split thresholds are the
midpoint between adjacent distinct feature values.

| Member | Signature | Description |
|---|---|---|
| `new DecisionTree*(options?)` | `(object?)` | See the options table below. |
| `.fit(X, y)` | `(number[][], number[]) → this` | Build the tree. |
| `.predict(X)` | `(number[][]) → number[]` | Majority label, or leaf mean. |
| `.depth` | `number` (getter) | Depth of the fitted tree (0 = a single leaf). |
| `.close()` | `() → void` | Release. |

### `class RandomForestClassifier` · `class RandomForestRegressor`

Bootstrap-aggregated trees with a random feature subset per split. The classifier predicts the
argmax of the averaged leaf class distributions; the regressor predicts the mean of the trees.

### `class GradientBoostingRegressor`

Trees fitted successively on the residuals of the running prediction, starting from the target mean.

### `class GradientBoostingClassifier`

Trees fitted successively on the **gradient of the deviance**, starting from the log-odds of the
class priors. Two classes use the binomial deviance and one tree per round; three or more use the
multinomial deviance and one tree per class per round, so `apply()` returns
`nEstimators × classes` columns.

Each tree chooses its partition by least squares on the gradient, and then the value inside every
leaf is replaced by one Newton step on the deviance itself (Friedman's line search). That second
step is what makes the model minimise the loss it claims to; without it the leaf holds a
squared-error fit to a residual, and the model converges slowly to the wrong place.

`.predictProba(X)` is the link function of the raw score — sigmoid for two classes, softmax for
more — not an average of leaf distributions, because a boosted tree fits gradients and has none.
`predict` is the argmax of the same raw scores, so **`predict` and `argmax(predictProba)` cannot
disagree here**, unlike the forest.

**`fit` throws `TypeError` if `y` has fewer than two distinct labels.** There is no log-odds to
boost; a `DecisionTreeClassifier` accepts that input, and this deliberately does not.

| Member | Signature | Description |
|---|---|---|
| `new GradientBoostingClassifier(options?)` | `(object?)` | See the options table below. |
| `.fit(X, y)` | `(number[][], number[]) → this` | |
| `.predict(X)` | `(number[][]) → number[]` | Labels. |
| `.predictProba(X)` | `(number[][]) → number[][]` | Rows sum to 1, columns in `classes` order. |
| `.apply(X)` | `(number[][]) → number[][]` | Leaf index per tree, round-major. |
| `.featureImportances` | `number[]` (getter) | |
| `.depth` | `number` (getter) | |
| `.close()` | `() → void` | Release. |

**Options** (all optional; unknown keys are ignored, out-of-range values **throw** `RangeError`):

| Option | Default | Applies to | Description |
|---|---|---|---|
| `maxDepth` | `0` (unlimited), `3` for boosting | all | Maximum tree depth. |
| `minSamplesSplit` | `2` | all | Minimum samples required to split a node (≥ 2). |
| `minSamplesLeaf` | `1` | all | Minimum samples in a leaf (≥ 1). |
| `maxFeatures` | all features; `sqrt(cols)` / `cols/3` for forests | all | Features considered per split. |
| `maxBins` | `0` (exact splits) | all | Bin each column into at most this many buckets, 2–255. |
| `nEstimators` | `100` | forests, boosting | Number of trees (≥ 1). |
| `learningRate` | `0.1` | boosting | Shrinkage applied to each tree. |
| `subsample` | `1.0` | boosting | Row fraction per tree, in (0, 1]. |
| `seed` | `12345` | forests, boosting | RNG seed. |

#### `maxBins` — histogram split finding

The default splitter sorts every candidate feature at every node. `maxBins` sorts each column **once
per fit** instead, into that many ordered buckets, and a node then scans one byte per row.

**With `maxBins` at or above a column's distinct-value count the two find the same splits.** A
column with `d` distinct values has `d-1` midpoints between consecutive values; one bin per value
enumerates all of them, and the threshold is placed between the values in the nearest non-empty bins
either side of the cut — the same pair the sorted sweep brackets. A classifier fit that way is
identical bit for bit. A regression fit accumulates its sums in a different order, so two split gains
that tie in exact arithmetic can fall the other way; measured over 108 fits, that never happened
where every feature was considered at each node and happened twice in 36 forest fits, worth under
0.002 r². `tests/oracle_ml_hist.js` is the differential.

Below that count the bins quantise and the model genuinely differs — usually by very little, and the
fit is much faster.

Measured on this machine (`tests/bench_ml_hist.js`, ratio = binned ÷ exact):

Boosting gains most, because one binning pass serves every round. A high bin count is not a penalty:
a node only clears and sweeps the range of bins its own rows fall into, and a tree's whole point is
that deep nodes hold a narrow range. **Binning is a fixed per-fit price**, so a fit small enough has
nothing to amortise it against — that is the only case where it does not pay.

### `class XGBRegressor` · `class XGBClassifier`

Boosting with the **second-order** objective. `GradientBoosting*` fits a least-squares tree to the
negative gradient and then repairs each leaf with one Newton step, so the curvature arrives after the
shape of the tree is already decided. These put the curvature in the split criterion. Writing `G` and
`H` for the gradient and hessian sums over a region, a split is worth

```
½ [ G_L²/(H_L+λ) + G_R²/(H_R+λ) − G²/(H+λ) ] − γ
```

and the leaf weight is `−G/(H+λ)`, with `α` soft-thresholding `G` so a leaf can be **exactly** zero.

They are the only models in `dyna:ml` that **accept missing values**: a `NaN` in `X` is a missing
feature, and every split learns which way such a row descends by scoring both. An infinity is still
rejected — it is not a missing marker, it is arithmetic that already failed. `y` may never be `NaN`.

They also take `sampleWeight`, which scales `g` and `h` together — the meaning a weight has for a
Newton step.

| Member | Signature | Description |
|---|---|---|
| `new XGBClassifier(options?)` | `(object?)` | Options below, plus every option in the table above. |
| `.fit(X, y, opts?)` | `(number[][], number[], {sampleWeight?}) → this` | |
| `.predict(X)` | `(number[][]) → number[]` | Labels; regressor returns values. |
| `.predictProba(X)` | `(number[][]) → number[][]` | Classifier only; rows sum to 1. |
| `.apply(X)` | `(number[][]) → number[][]` | Leaf index per tree, round-major. |
| `.featureImportances` | `number[]` (getter) | |
| `.depth` | `number` (getter) | |
| `.bestRounds` | `number` (getter) | Rounds kept; `nEstimators` unless early stopping cut it. |
| `.close()` | `() → void` | Release. |

| Option | Default | Description |
|---|---|---|
| `lambda` | `1.0` | L2 on the leaf weight. |
| `alpha` | `0.0` | L1 on the leaf weight; large enough zeroes every leaf. |
| `gamma` | `0.0` | Minimum gain to keep a split; large enough leaves every tree a single leaf. |
| `minChildWeight` | `1.0` | Minimum **hessian sum** in a child. |
| `colsampleByTree` | `1.0` | Feature fraction drawn once per tree; composes with `maxFeatures`, which draws again per node. |
| `earlyStoppingRounds` | `0` (off) | Stop after this many rounds without validation improvement. |
| `validationFraction` | `0.1` | Rows held out for that comparison, drawn once. |
| `learningRate` | `0.3` | |
| `maxDepth` | `6` | |
| `maxBins` | `255` | These models always use histogram split finding. |

`minChildWeight` is a floor on curvature, not on a row count — a thousand rows of weight `0.001`
carry a hessian sum of 1 and will not pass `minChildWeight: 2`, while the same rows at weight 1 will.

Early stopping keeps the **best** round, not the last one tried; the rounds after it are the evidence
that it was the best, and they are discarded. `bestRounds` reports what survived.

<!-- check:skip -->
```js
const m = new XGBClassifier({ nEstimators: 500, maxDepth: 4,
                              lambda: 2, earlyStoppingRounds: 10 });
m.fit(X, y);                       // X may contain NaN
m.bestRounds;                      // e.g. 118
```

### Model selection

| Function | Signature | Description |
|---|---|---|
| `trainTestSplit(n, opts?)` | `(number, {testSize?, shuffle?, seed?}) → {train, test}` | Index arrays. |
| `kFold(n, opts?)` | `(number, {folds?, shuffle?, seed?}) → {train, test}[]` | `folds` and `k` are the same option. |
| `stratifiedKFold(y, opts?)` | `(number[], {folds?, shuffle?, seed?}) → {train, test}[]` | Preserves class balance per fold. |
| `crossValScore(factory, X, y, opts?)` | `(() => model, …, {folds?, scoring?, shuffle?, seed?}) → number[]` | **Per-fold** scores. |
| `gridSearch(factory, X, y, grid, opts?)` | `((params) => model, …, object, …) → {best, bestScore, results}` | Every combination. |
| `randomSearch(factory, X, y, grid, opts?)` | as above, plus `{nIter, seed}` | `nIter` sampled points. |

**The estimator is a factory, not an instance** — `() => new LogisticRegression()` for
`crossValScore`, `(p) => new RandomForest(p)` for the searches. Reusing one instance would score a
model that had already seen the test fold, and it means these functions need to know nothing about
how any model is constructed.

`crossValScore` returns the **scores, not their mean**, because the spread is the useful part:
0.90 ± 0.01 and 0.90 ± 0.30 are different models and a mean hides it. `scoring` defaults to accuracy;
a regression estimator should pass its own rather than have one guessed from the shape of `y`.
The searches return every point with its per-fold scores in `results` — the runner-up and the spread
are what say whether the winner is real.

<!-- check:skip -->
```js
const cv = crossValScore(() => new DecisionTreeClassifier({ maxDepth: 4 }), X, y, { folds: 5 });

const g = gridSearch(p => new RandomForestClassifier(p), X, y,
                     { nEstimators: [10, 50], maxDepth: [2, 4, 8] }, { folds: 5 });
const model = new RandomForestClassifier(g.best);
```

`randomSearch` samples instead of enumerating, which beats the grid as soon as one parameter matters
much more than another — the grid spends its budget re-testing the irrelevant one at every level of
the relevant one. The same `seed` samples the same points.

### Missing data

**Every `fit` rejects non-finite input**, naming the exact cell. It used to propagate silently: a
NaN feature poisons a mean, a variance, a centroid, a split threshold and every coefficient
downstream, and the model still "fits" — then returns NaN predictions, or predictions that are merely
wrong because one class's statistics lost every comparison they entered. Infinities count as missing
for the same reason.

| Function | Signature | Description |
|---|---|---|
| `imputeMean(X)` | `(number[][]) → number[][]` | Replace each non-finite entry with its **column** mean over the finite values. A column with no finite value throws — filling it would invent data. The input is not mutated. |
| `dropMissing(X, y?)` | `(number[][], number[]?) → {X, y, kept}` | Drop every row holding a non-finite value. `kept` is the surviving **row indices**, so weights, ids or timestamps can follow the same filter without re-deriving it. |

A non-finite **target** is never imputed — inventing a target is inventing the answer — so `y` NaNs
are only handled by `dropMissing`.

<!-- check:skip -->
```js
const clean = dropMissing(X, y);
model.fit(clean.X, clean.y);
const ids = clean.kept.map(i => rowIds[i]);   // carry anything else along
```

### `class SVC`

Kernel support vector classifier trained by Sequential Minimal Optimization. Multi-class is
one-vs-rest.

| Member | Signature | Description |
|---|---|---|
| `new SVC(options?)` | `(object?)` | `{ kernel: "linear" \| "rbf" \| "poly", C, gamma, coef0, degree, tol, maxIter }`. Defaults: `"rbf"`, `C` 1, `gamma` `1/cols`, `coef0` 0, `degree` 3, `tol` 1e-3, `maxIter` 1000. An unknown kernel name is a `TypeError`. |
| `.fit(X, y)` | `(number[][], number[]) → this` | **Throws** `RangeError` if `y` has fewer than two distinct labels. |
| `.predict(X)` | `(number[][]) → number[]` | Predicted labels. |
| `.decisionFunction(X)` | `(number[][]) → number[] \| number[][]` | Signed margin. One value per row for two classes; one per class for more. |
| `.nSupportVectors` | `number` (getter) | Total support vectors kept. |
| `.classes` | `number[]` (getter) | Sorted labels. |
| `.close()` | `() → void` | Release. |

### `class LogisticRegression`

Multinomial logistic regression by full-batch gradient descent with an elastic-net proximal step.

| Member | Signature | Description |
|---|---|---|
| `new LogisticRegression(opts?)` | `(object?)` | See options below. |
| `.fit(X, y)` | `(number[][], number[]) → this` | **Throws** if `y` has fewer than two distinct labels. |
| `.predict(X)` | `(number[][]) → number[]` | Labels, in the values `y` used. |
| `.predictProba(X)` | `(number[][]) → number[][]` | **rows × classes**; each row sums to 1. |
| `.classes` | `number[]` (getter) | Sorted labels. |
| `.coef` | `number[][]` (getter) | One row per weight vector: **1 row for two classes** (a second would be its exact negation), one per class beyond that. |
| `.intercept` | `number \| number[]` (getter) | Scalar for two classes, one per class beyond. |
| `.nIter` / `.converged` | (getters) | Iterations actually run, and whether the gradient reached `tol`. |

| Option | Default | Description |
|---|---|---|
| `maxIter` | `3000` | Iteration cap. |
| `tol` | `1e-4` | Stop when the largest effective gradient falls below this. |
| `learningRate` | `0.1` | Step size. |
| `l2` / `l1` | `0` | Penalty strengths, applied directly. |
| `penalty` + `C` | — | The scikit-learn spelling: `"l1"`, `"l2"`, `"elasticnet"` or `"none"`, with `C` the **inverse** strength (small `C` penalises hard). |
| `classWeight` | — | `"balanced"` weights each class by `n / (k × count)`, so a 2% minority class stops being ignored. |

**`converged: false` is not a failure.** On separable data with no penalty the likelihood has no
finite maximum — the weights grow without bound — so the fit correctly runs to `maxIter` and still
separates perfectly. Where there *is* an optimum, the check earns its place: a regularised fit on
overlapping data stops well short of `maxIter`, at the same accuracy.

**The penalties are proximal, not explicit**, which is a stability property rather than a
refinement: `w -= lr·(g + l2·w)` diverges to NaN as soon as `lr·l2 > 1`, which `penalty:"l2", C:0.01`
reaches immediately. The proximal form is stable at any strength, and its L1 half produces **exact
zeros** — which is what L1 is for. Neither penalty touches the intercept.

### `class GaussianNB`

Gaussian naive Bayes, scored entirely in log space (a product of per-feature densities underflows a
double well before 40 features).

| Member | Signature | Description |
|---|---|---|
| `new GaussianNB(varSmoothing?)` | `(number?)` | Fraction of the largest feature variance added to every variance; default 1e-9. |
| `.fit(X, y)` | `(number[][], number[]) → this` | Fit per-class means and variances. |
| `.predict(X)` | `(number[][]) → number[]` | Most probable label. |
| `.predictProba(X)` | `(number[][]) → number[][]` | Posterior per class; each row sums to 1. |
| `.classes` | `number[]` (getter) | Sorted labels. |
| `.close()` | `() → void` | Release. |

### `class KMeans`

Lloyd's algorithm with seeded k-means++ initialisation.

| Member | Signature | Description |
|---|---|---|
| `new KMeans(k, seed?)` | `(number, number?)` | `k` cluster count; optional RNG `seed`. |
| `.fit(points)` | `(number[][]) → this` | Cluster the points. **Throws** `RangeError` with fewer than `k` rows. |
| `.predict(points)` | `(number[][]) → number[]` | Nearest cluster index per point. |
| `.inertia` | `number` (getter) | Within-cluster sum of squared distances after `fit`. |
| `.close()` | `() → void` | Release. |

### `class DBScan`

Density-based clustering: no cluster count needed, and points in no dense region are labelled noise.
Unlike a centroid method it finds clusters of any shape — a long chain of nearby points is one
cluster even when its ends are far apart.

| Member | Signature | Description |
|---|---|---|
| `new DBScan(eps?, minPts?)` | `(number?, number?)` | Neighbourhood radius (default 0.5) and the minimum neighbour count, including the point itself, for a core point (default 5). |
| `.fit(points)` | `(number[][]) → this` | Label every point. |
| `.labels` | `number[]` (getter) | Cluster id per point; **-1 means noise**. Empty before `fit`. |
| `.nClusters` | `number` (getter) | Cluster count found. |
| `.eps` | `number` (getter) | The configured radius. |
| `.close()` | `() → void` | Release. |

Fit is O(rows² · cols) — there is no spatial index — but memory is O(rows).

### `class GaussianMixture`

Soft clustering by Expectation-Maximisation with **diagonal** covariances, initialised from
k-means++.

| Member | Signature | Description |
|---|---|---|
| `new GaussianMixture(k?, options?)` | `(number?, object?)` | `k` defaults to 3. `{ maxIter: 200, tol: 1e-3, regCovar: 1e-6, seed: 12345 }`. |
| `.fit(points)` | `(number[][]) → this` | Run EM. **Throws** `RangeError` with fewer than `k` rows. |
| `.predict(points)` | `(number[][]) → number[]` | Most likely component index. |
| `.predictProba(points)` | `(number[][]) → number[][]` | Responsibilities; each row sums to 1. |
| `.weights` | `number[]` (getter) | Mixing weights, summing to 1. |
| `.means` | `number[][]` (getter) | `k × cols` component means. |
| `.variances` | `number[][]` (getter) | `k × cols` per-feature variances (regularised). |
| `.logLikelihood` | `number` (getter) | Total log likelihood at convergence. |
| `.nIter` | `number` (getter) | Iterations actually run. |
| `.close()` | `() → void` | Release. |

`regCovar` is added to every variance, so a component collapsing onto a single point cannot produce
an infinite density.

### `class PCA`

Principal components via a Jacobi eigendecomposition of the covariance matrix. Components are
returned with a deterministic sign (largest-magnitude entry positive), so repeated fits agree exactly.

| Member | Signature | Description |
|---|---|---|
| `new PCA(nComponents?, whiten?)` | `(number?, boolean?)` | `0`/omitted keeps all features. `whiten` scales each component to unit variance. |
| `.fit(X)` | `(number[][]) → this` | **Throws** `RangeError` with fewer than two rows, or if `nComponents` exceeds the feature count. |
| `.transform(X)` | `(number[][]) → number[][]` | Project onto the components. |
| `.fitTransform(X)` | `(number[][]) → number[][]` | Both in one pass. |
| `.inverseTransform(P)` | `(number[][]) → number[][]` | Back to feature space (exact when all components are kept). |
| `.components` | `number[][]` (getter) | `nComponents × cols`, unit rows, orthogonal. |
| `.mean` | `number[]` (getter) | Per-feature mean subtracted before projecting. |
| `.explainedVariance` | `number[]` (getter) | Eigenvalue per component, descending. |
| `.explainedVarianceRatio` | `number[]` (getter) | Each as a fraction of the **total** variance. |
| `.close()` | `() → void` | Release. |

### `class StandardScaler` · `class MinMaxScaler`

Per-feature scaling. `StandardScaler` gives zero mean and unit variance (population std, `ddof=0`);
`MinMaxScaler` maps the observed range onto [0, 1] — an out-of-range input extrapolates rather than
clipping. A **constant column scales by 1** instead of dividing by zero, so it becomes 0 rather than
`NaN`.

| Member | Signature | Description |
|---|---|---|
| `new StandardScaler()` / `new MinMaxScaler()` | | Create an unfitted scaler. |
| `.fit(X)` | `(number[][]) → this` | Learn the per-column statistics. |
| `.transform(X)` | `(number[][]) → number[][]` | Apply. |
| `.fitTransform(X)` | `(number[][]) → number[][]` | Both in one pass. |
| `.inverseTransform(X)` | `(number[][]) → number[][]` | Undo. |
| `.mean`, `.std` | `number[]` (getters) | `StandardScaler` only. |
| `.dataMin`, `.dataMax` | `number[]` (getters) | `MinMaxScaler` only. |
| `.close()` | `() → void` | Release. |

### Metrics

Plain functions over two equal-length vectors (`Array` or `Float64Array`). **Throws** `TypeError` on a
length mismatch, `RangeError` on empty input.

| Function | Signature | Description |
|---|---|---|
| `meanSquaredError(yTrue, yPred)` | `→ number` | Mean of the squared residuals. |
| `meanAbsoluteError(yTrue, yPred)` | `→ number` | Mean of the absolute residuals. |
| `r2Score(yTrue, yPred)` | `→ number` | Coefficient of determination. A constant `yTrue` gives 1 when the prediction is exact and 0 otherwise — never `NaN`. |
| `logLoss(yTrue, yProba)` | `→ number` | Mean negative log-likelihood. `yProba` is either a flat vector of P(positive) or a rows × classes matrix — the shape `predictProba` returns. Probabilities are clipped to `[1e-15, 1-1e-15]`, so a confident miss is a large **finite** penalty. A column count that does not match the labels in `yTrue` is a `TypeError`. |
| `accuracy(yTrue, yPred)` | `→ number` | Fraction of exact matches. |
| `confusionMatrix(yTrue, yPred)` | `→ number[][]` | Counts indexed `[true][pred]`, sized by the largest label in **either** vector. Labels must be non-negative integers ≤ 4095. |

### Classification metrics

Binary; the optional `positive` selects which label counts as positive (default `1`). A zero
denominator gives `0`, not `NaN`, so a degenerate fold cannot poison an averaged score.

| Function | Signature | Description |
|---|---|---|
| `precision(yTrue, yPred, positive?)` | `→ number` | TP/(TP+FP). |
| `recall(yTrue, yPred, positive?)` | `→ number` | TP/(TP+FN). |
| `specificity(yTrue, yPred, positive?)` | `→ number` | TN/(TN+FP). Equals `recall` of the negative class. |
| `f1(yTrue, yPred, positive?)` | `→ number` | Harmonic mean of precision and recall. |
| `fbeta(yTrue, yPred, beta, positive?)` | `→ number` | Weighted; `beta > 1` favours recall. **Throws** `RangeError` unless `beta > 0`. |
| `balancedAccuracy(yTrue, yPred, positive?)` | `→ number` | Mean of recall and specificity; `0.5` for any constant classifier. |
| `matthewsCorrcoef(yTrue, yPred, positive?)` | `→ number` | Matthews correlation in `[-1, 1]`; uses every cell of the matrix, so it stays honest under imbalance. |
| `cohenKappa(yTrue, yPred, positive?)` | `→ number` | Agreement corrected for chance. |
| `rocAuc(yTrue, yScore, positive?)` | `→ number` | ROC AUC by the rank identity — exact, and ties are averaged so a constant scorer is exactly `0.5`. **Throws** `RangeError` without both classes present. |
| `averagePrecision(yTrue, yScore, positive?)` | `→ number` | Step-function average precision. |

**Why these and not just `accuracy`.** A classifier that always answers "negative" scores `0.99` on a
1%-positive set. `precision`, `recall`, `f1`, `matthewsCorrcoef` and `cohenKappa` all report `0` for
it, and `balancedAccuracy` reports `0.5`.

### Model selection

These return **indices**, not slices, so the same split works against an `Array` of rows and against
a flat `Float64Array` — and costs O(n) rather than O(n × features).

| Function | Signature | Description |
|---|---|---|
| `trainTestSplit(n, options?)` | `(number \| array, {testSize?, shuffle?, seed?}) → {train, test}` | `testSize` defaults to `0.25`, `shuffle` to `true`, `seed` to `12345`. Neither side is ever empty. `shuffle: false` takes a contiguous head/tail, for time-ordered data. |
| `kFold(n, options?)` | `(number \| array, {k?, shuffle?, seed?}) → {train, test}[]` | `k` defaults to `5`. Fold sizes differ by at most one; every index is in exactly one test fold. |
| `stratifiedKFold(y, options?)` | `(array, {k?, shuffle?, seed?}) → {train, test}[]` | As `kFold`, but each fold keeps the class proportions of `y` to within one sample. |

`n` may be a count or any array (its `length` is used). A given `seed` reproduces a split exactly,
which is what makes two cross-validation scores comparable. **Throws** `RangeError` for `k < 2`,
`k > n`, `n < 2`, or a `testSize` outside `(0, 1)`.

<!-- check:skip -->
```js
const folds = stratifiedKFold(y, { k: 5, shuffle: true, seed: 1 });
for (const { train, test } of folds) {
  clf.fit(train.map(i => X[i]), train.map(i => y[i]));
  scores.push(f1(test.map(i => y[i]), clf.predict(test.map(i => X[i]))));
}
```

<!-- check:skip -->
```js
import { StandardScaler, PCA, RandomForestClassifier, accuracy } from "dyna:ml";

const sc = new StandardScaler(), pca = new PCA(2);
const clf = new RandomForestClassifier({ nEstimators: 50, seed: 1 });
try {
  const Z = pca.fitTransform(sc.fitTransform(X));   // scale, then reduce
  clf.fit(Z, y);
  print(accuracy(y, clf.predict(Z)));
} finally { sc.close(); pca.close(); clf.close(); }
```

## The tree family: probabilities, importances and leaves

`DecisionTreeClassifier`, `DecisionTreeRegressor`, `RandomForestClassifier`, `RandomForestRegressor`
and `GradientBoostingRegressor` share three members beyond `fit`/`predict`.

| Member | Signature | Description |
|---|---|---|
| `.predictProba(X)` | `→ number[][]` | `rows × classes`, columns in `classes` order, each row summing to 1. **Classifiers only** — `TypeError` on a regressor. |
| `.featureImportances` | `number[]` | One weight per feature, non-negative, summing to 1 (all zeros for a model that never split). |
| `.apply(X)` | `→ number[][]` | `rows × trees` leaf indices. |
| `.depth` | `number` | Depth of the deepest tree. |

**`predictProba` averages the leaf class distributions**, not the fraction of trees voting for a
label. That is what makes it useful on a *single* tree: a vote fraction over one tree can only ever
be 0 or 1, which would make `logLoss`, `rocAuc` and `averagePrecision` meaningless for it.

**`predict` IS the argmax of `predictProba`** — the same averaged leaf distributions, so the two
cannot answer the same question differently.

They used to differ: `predict` was a majority vote over each tree's hard label, so a tree that was
51% sure contributed a whole vote to one class and nothing to the other. On noisy labels that disagreed with
`predictProba` on a handful of rows, and was the less accurate of the two. A single tree is unaffected either way: with one tree
the averaged distribution is that tree's own leaf distribution, whose argmax is the label it would
have voted.

For thresholding, ranking, or any metric that takes a score, use `predictProba` — `predict` throws
the score away.

<!-- check:skip -->
```js
const rf = new RandomForestClassifier({ nEstimators: 100, seed: 1 });
rf.fit(X, y);
const score = rf.predictProba(Xtest).map(row => row[1]);   // P(class 1)
rocAuc(ytest, score);
logLoss(ytest, score);
rf.featureImportances;          // e.g. [0.24, 0.60, 0.16]
```

**What `featureImportances` is not.** It is the total impurity decrease each feature achieved,
weighted by the samples that reached each split — computed from the training fit alone. So it
rewards features with many distinct values (they offer more split points), it credits whichever of
two correlated features happened to be chosen first, and an unrestricted tree assigns a pure-noise
feature a visible share by splitting on it deep down where samples are few. It describes how the
forest was built, not what matters.

`apply` returns the identity of the leaf each row lands in — the tree's own encoding of that row,
which is what makes it useful as a feature for a downstream model. Identical rows always land in
identical leaves.

## Persistence

Every fitted model writes to a binary record and back. The format is the same
`DYNS` envelope `dyna:structures` uses, in its own `type_id` range, so there is one format to
version and one reader to audit.

| Member | Signature | Description |
|---|---|---|
| `.serialize()` | `→ Uint8Array` | `TypeError` if the model is not fitted. |
| `.save(path)` | `→ number` | Writes the record; returns the byte count. |
| `Model.deserialize(bytes)` | `→ Model` | **Static.** `bytes` is a `Uint8Array`, any TypedArray view, or an `ArrayBuffer`. |
| `Model.load(path)` | `→ Model` | **Static.** |

<!-- check:skip -->
```js
import { RandomForestClassifier } from "dyna:ml";

const clf = new RandomForestClassifier({ nEstimators: 100, seed: 1 });
clf.fit(X, y);
clf.save("/models/spam.dyns");

// ... a different process, a week later
const loaded = RandomForestClassifier.load("/models/spam.dyns");
loaded.predict(newRows);
```

**Predictions are bit-identical.** A loaded model returns exactly the doubles the saved one did, not
approximately: parameters are stored as IEEE-754 bit patterns, never as text.

**A record holds fitted parameters, never training data** — coefficients, centroids, class priors,
tree nodes, support vectors. The exception names itself: `KNClassifier` and `KNRegressor` *are* their
training sample, so their record carries it, and it is as large as the data was.

**A record knows what it is.** `RandomForestRegressor.deserialize(bytes)` on a classifier record
throws a `TypeError` naming both types rather than reinterpreting leaf values as class labels. The
model's own class must match the record's.

**Loading is reading untrusted input.** The record's checksum is verified before any of it is
interpreted, every length is checked against the bytes that remain before anything is allocated for
it, and a tree's node references must be in range, acyclic and consistent with their leaf flags. A
corrupted or hostile record throws.

Deserializing does not need the constructor's options: everything a prediction depends on is in the
record. Hyper-parameters that only affect fitting are carried too, so calling `fit()` again on a
loaded model behaves as it would have on the original.

---

# compress

`import { gzip, gunzip, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, Compressor } from "dyna:compress";`

Two tiers. **gzip** is DEFLATE with RFC 1952 framing: the better ratio, and what every other tool
reads. **LZ4** is the fast tier: several times
faster to compress and to decompress, for a moderately larger output.

| Function | Signature | Description |
|---|---|---|
| `gzip(data)` | `(bytes) → Uint8Array` | Compress. |
| `gunzip(data, opts?)` | `(bytes, {asString?}) → Uint8Array \| string` | Decompress. **Throws** on corrupt input. |
| `lz4Compress(data, opts?)` | `(bytes, {level?, dict?}) → Uint8Array` | A **raw LZ4 block** — no header, no length, no checksum. |
| `lz4Decompress(data, opts?)` | `(bytes, {dict?}) → Uint8Array` | Decode a raw block. **Throws** on malformed input. |
| `lz4Frame(data, opts?)` | `(bytes, {level?, checksum?}) → Uint8Array` | The **LZ4 frame** (magic `0x184D2204`), which the `lz4` command line reads and writes. `checksum` defaults to `true`. |
| `lz4Unframe(data, opts?)` | `(bytes, {asString?}) → Uint8Array \| string` | Decode a frame, validating the descriptor checksum, every block length, and the content checksum when present. |

`level` is 1–12 and selects **match-finder effort only**: every level emits the same format and is
read by the same decoder, so "LZ4HC" here is a better parse, not a second format.

**Level 12 is the one to reach for when you want gzip's ratio without gzip's cost**: it buys most of
that ratio while staying several times faster to compress and to decompress.
On **already-compressed** bytes every tier declines to expand the input — the frame format falls
back to stored blocks, and the raw block's worst case is one escape byte per 255.


### `class Dictionary`

The **token-substitution** codec: a compiled Aho-Corasick automaton over a list of phrases, which
replaces each occurrence with a short code and leaves everything else as a literal run.

This is not `new Compressor({dict})`, and neither replaces the other. That one seeds an LZ77 window
with a known block and wins when the payload resembles it. This one substitutes known **phrases**
and wins where the payload is far too short for LZ77 to have built a useful window at all — a
JSON-RPC envelope, a log line, a protocol header.

```js
import { fromUtf8 } from "dyna:bytes";
import { Dictionary } from "dyna:compress";

const frame = '{"jsonrpc":"2.0","method":"add","params":[1,2],"id":7}';
const d = new Dictionary(['"jsonrpc":"2.0"', '"method":', '"params":', '"id":']);
const packed = d.compress(fromUtf8(frame)); // one automaton, unbounded records
```

| Member | Signature | Description |
|---|---|---|
| `new Dictionary(phrases)` | `(string[]) → Dictionary` | At least one phrase, at most 65535. An empty phrase throws: it matches everywhere and encodes nothing. |
| `.compress(data)` | `(bytes) → Uint8Array` | |
| `.decompress(data)` | `(bytes) → Uint8Array` | **Throws** on a malformed record *or* a dictionary mismatch. |
| `.id` | `number` (getter) | CRC-32C of the phrase list. |
| `.size` | `number` (getter) | The phrase count. |
| `.close()` | `() → void` | Release the automaton. |

**The record carries the dictionary id, and a mismatch produces nothing.** Every code in a
token-substituted record is a valid index into *any* dictionary, so decoding one against the wrong
phrase list would otherwise succeed and hand back a different string — a silent wrong answer. The
id is checked before a single byte is emitted. It is a function of the phrases **and their order**,
and it is length-prefixed, so `["ab","c"]` and `["a","bc"]` are different dictionaries.

**What it is worth, both directions.** On the 54-byte JSON-RPC frame above with a 9-phrase
dictionary: **54 → 38 bytes**, against gzip's 70 — gzip cannot win on a payload this short because
DEFLATE's own header is most of it. On bytes containing none of the phrases the whole input becomes
one literal run and the output is the input plus 8 bytes of header: **an expansion**. On
already-compressed bytes it does not shrink further. Those losing rows are the point of publishing
them — a dictionary pays exactly to the extent that its phrases actually occur, and nowhere else.

The parse is a dynamic program rather than a greedy walk, because greedy was measurably wrong: with
a phrase list containing both `{"` and `"jsonrpc":"2.0"`, a greedy encoder takes the two-byte match
at position 0 and steps over the fifteen-byte match at position 1, which it can never come back
for. That turned the 54-byte frame into 61 bytes. The parse is allowed to decline a match.

`compress` and `decompress` **type-check** their argument rather than coercing it, so no user code
runs inside a call and one instance is safe to reuse anywhere.

### `class Compressor`

A compiled capability: the configuration goes in the constructor and the data goes in the method,
so the match-finder scratch — a 64 KiB hash table plus 4 bytes per input byte — is allocated once
and reused instead of per call.

```js
import { fromUtf8 } from "dyna:bytes";
import { Compressor } from "dyna:compress";

const stream = [fromUtf8("one record"), fromUtf8("another record")];
const lz4 = new Compressor({ algo: "lz4" }), out = [];
for (const rec of stream) out.push(lz4.compress(rec));   // scratch reused, no per-call alloc
```

| Member | Signature | Description |
|---|---|---|
| `new Compressor(opts?)` | `({algo?, level?, dict?, checksum?})` | `algo` is `"lz4"` (default), `"lz4frame"` or `"gzip"`. |
| `.compress(data)` | `(bytes) → Uint8Array` | |
| `.decompress(data)` | `(bytes) → Uint8Array` | **Throws** on a malformed record or a dictionary mismatch. |
| `.algo` | `string` (getter) | |
| `.dictId` | `number \| null` (getter) | The CRC-32C of the configured dictionary. |
| `.close()` | `() → void` | Release the scratch. |

**Crossover** (N uses of one instance versus N free calls, construction included): **N=2** on a
91-byte record, reaching **4.0×** at N=1000; **N=1** on a 4 KB chunk, 1.28× at scale; **N=2** on a
64 KB page, but only 1.01× — the fixed cost is invisible next to 64 KB of actual compression. The
capability is worth hoisting in proportion to how small the records are.

The input is **type-checked, not coerced**: a string, TypedArray or ArrayBuffer, and anything else
throws. No user JS runs inside a call, so an instance holds no lock and needs none.

### Dictionaries

`{ dict }` seeds the LZ4 match window with a prefix, so a short templated payload can match against
it. On a 103-byte JSON-RPC frame with a 100-byte dictionary: **105 → 24 bytes**. `gzip` of the same
frame is 115 bytes — DEFLATE has no window there either, and its Huffman header costs more than it
saves.

A raw LZ4 block does not record which dictionary produced it, so a `Compressor` with a dictionary
**stamps its record with a 4-byte dictionary id** and refuses to decode one written with a different
dictionary. Without that, a mismatch produces plausible garbage rather than an error.

```js
const data = "the quick brown fox ".repeat(100);
const z = gzip(data);
gunzip(z).length;                  // the original length

const f = lz4Frame(data);          // readable by `lz4 -d`
lz4Unframe(f, { asString: true });
```

---


### Archives — tar and zip

| Function | Signature | Description |
|---|---|---|
| `TarPack(entries)` | `(array) → Uint8Array` | ustar. Each entry is `{name, data?, mode?, mtime?}`; no `data` means a directory. |
| `TarList(bytes, opts?)` | `(bytes) → array` | Entry metadata only. |
| `TarExtract(bytes, opts?)` | `(bytes) → array` | The same, each with its `data`. |
| `ZipPack(entries, {method}?)` | `(array, object) → Uint8Array` | `"deflate"` (default) or `"store"`. |
| `ZipList(bytes, opts?)` | `(bytes) → array` | From the **central directory**, which is the format's index. |
| `ZipRead(bytes, name, opts?)` | `(bytes, string) → Uint8Array` | One member, decompressed and CRC-checked. |

**Every entry name is validated at the PARSE boundary**, so a caller that
writes `entry.name` into a directory is safe without knowing to check: an
absolute path, a `..` segment, a backslash, a drive letter or an embedded NUL
is refused on both the reading and the writing side. Tar-slip and zip-slip are
the same bug and both die here. `{ allowUnsafeNames: true }` is the explicit
opt-out, and it is the only way to see such a name.

The tar reader handles ustar with the `prefix` field, GNU long names (`L`) and
PAX records (`x`), which is what `tar(1)` actually writes. The writer emits
ustar only, and refuses a name too long to split rather than truncating it.

`ZipRead` verifies the member's **CRC** — the format's own integrity check, so a
failure is an error rather than a warning — and refuses an archive whose local
header names a different file than the central directory, which is a real
attack rather than a curiosity. Compression methods are store and deflate;
anything else is refused by number. An encrypted archive is refused as well:
legacy ZipCrypto is broken by design, so reading it would be the wrong favour.

```js
const t = TarPack([{ name: "a.txt", data: "hello" }, { name: "d/" }]);
TarList(t);                      // [{name:"a.txt", size:5, type:"file"}, …]
TarExtract(t)[0].data;           // the bytes

const z = ZipPack([{ name: "a.txt", data: "hello ".repeat(50) }]);
ZipList(z)[0].method;            // "deflate"
ZipRead(z, "a.txt");             // the bytes back, CRC verified
```

# csv

`import { CSVFile } from "dyna:csv";`

File-oriented CSV create/read/update/delete, RFC 4180 (quoted fields, embedded commas/newlines/quotes,
`""` escaping). The module exports one class, **`CSVFile`**, whose constructor binds a file path; every
operation is a method on that instance and takes a single **options object** (the `path` is passed
once, to the constructor — never per call). Mutations are load-modify-store and write **atomically**
(temp file + fsync + rename); reads mmap the file; the structural scan is SIMD-accelerated. **Row
indices are 0-based over data rows** — row `0` is the first row after the header. Errors throw
`Error`/`TypeError`/`RangeError`.

### `new CSVFile(path)`

Binds `path`; does **not** touch the disk. The instance is stateless apart from the path — there is no
open file handle and no explicit save, so a single instance can be reused across operations (each
method re-reads the file). Release it with `.close()` / `[Symbol.dispose]` (optional; a `CSVFile`
holds no OS handle, only the path string). Each method copies the path before coercing arguments, so a
re-entrant `{ valueOf() { f.close(); } }` argument cannot use-after-free.

### `create(options)`

| Option | Type | Description |
|---|---|---|
| `headers` | `string[]` | Column names (≥ 1). |
| `rows` | `string[][]?` | Initial rows; each must have exactly `headers.length` values. |
| `overwrite` | `boolean?` | If `false` (default), **throws** when the file exists. |

Creates the file at the instance path (parent directories are created automatically). **Returns** `{ path, rows }` (`rows` = data rows written). **Throws** if the file exists without `overwrite`, `headers` is empty, or a row's width ≠ `headers.length`.

### `read(options?)`

| Option | Type | Description |
|---|---|---|
| `offset` | `number?` | Data rows to skip (default `0`). |
| `limit` | `number?` | Max rows returned (default: all). |
| `columns` | `string[]?` | Column names to include, in order (default: all). |

Called with no argument, reads the whole file. **Returns** `{ headers: string[], rows: string[][], totalRows: number }` — `totalRows` is the full count regardless of pagination. **Throws** on a missing file or unknown column.

### `addRow(options)`

| Option | Type | Description |
|---|---|---|
| `rows` | `(string[] \| object)[]` | Each row is a positional `string[]` (column order) **or** an object `{ column: value }` (missing columns → `""`, extra keys ignored). |

**Returns** `{ added: number, totalRows: number }`.

### `updateCell(options)`

| Option | Type | Description |
|---|---|---|
| `row` | `number` | 0-based data-row index. |
| `column` | `string?` | Column by name (mutually exclusive with `columnIndex`). |
| `columnIndex` | `number?` | Column by 0-based index. |
| `value` | `string` | New value (`""` clears the cell). |

**Returns** `{ row, column, value }`. **Throws** `RangeError` on an out-of-range row/index; `TypeError` on an unknown column or if no column selector is given.

### `removeRow(options)`

| Option | Type | Description |
|---|---|---|
| `row` | `number` | 0-based data-row index. Remaining rows shift up. |

**Returns** `{ removed: number, totalRows: number }`. **Throws** `RangeError` if out of range.

### `addColumn(options)`

| Option | Type | Description |
|---|---|---|
| `column` | `string` | New column name (must not exist). |
| `defaultValue` | `string?` | Fill for existing rows (default `""`). |

**Returns** `{ column, totalColumns }`. **Throws** if the column already exists.

### `removeColumn(options)`

| Option | Type | Description |
|---|---|---|
| `column` | `string?` | By name (mutually exclusive with `columnIndex`). |
| `columnIndex` | `number?` | By 0-based index. |

**Returns** `{ removedIndex, totalColumns }`.

### `renameColumn(options)`

| Option | Type | Description |
|---|---|---|
| `oldName` | `string` | Existing column (must exist). |
| `newName` | `string` | New name (must not exist unless equal to `oldName` → no-op). |

**Returns** `{ oldName, newName }`.

### `readColumnValuesRange(options)`

| Option | Type | Description |
|---|---|---|
| `column` | `string` | Column name. |
| `start` | `number?` | First data row (inclusive, default `0`). |
| `end` | `number?` | End row (exclusive, default: all). Max **requested** window (`end - start`) is **1000**. |

**Returns** `string[]`.

### `readRowRange(options?)`

| Option | Type | Description |
|---|---|---|
| `start` | `number?` | First data row (inclusive, default `0`). |
| `end` | `number?` | End row (exclusive, default `start + 1`). Max window **100**. |

**Returns** `{ headers: string[], rows: string[][] }`.

### `selectColumnRange(options)`

Project specific columns over a range (like `SELECT col…` with no `WHERE`).

| Option | Type | Description |
|---|---|---|
| `columns` | `string[]` | Column names, in output order (non-empty; all must exist). |
| `start` | `number?` | First data row (inclusive, default `0`). |
| `end` | `number?` | End row (exclusive, default: all). Max window **100**. |

**Returns** `{ columns: string[], rows: string[][] }`.

```js
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";

const users = new CSVFile(new Path("/tmp/u.csv"));
users.create({ headers: ["Name","Age"], rows: [["Alice","30"]], overwrite: true });
users.addRow({ rows: [{ Name: "Bob", Age: "25" }] });
users.updateCell({ row: 0, column: "Age", value: "31" });
users.read();
// { headers:["Name","Age"], rows:[["Alice","31"],["Bob","25"]], totalRows:2 }
```

---

# dataframe

`import { DataFrame } from "dyna:dataframe";`

Columnar tables over TypedArrays. A frame is a set of equal-length **columns**; every operation is a
single JS→C transition followed by one typed C loop over a whole column, rather than N interpreted
steps. Masked and grouped operations run 67–89× the equivalent JS loop.

Numeric columns **alias** the `ArrayBuffer` you pass in — construction copies nothing, so mutating
the array afterwards is visible to the frame. String columns are the one exception: they are
dictionary-encoded and copied at construction, which is what makes `GROUP_BY_SUM` a scatter-add with
no hashing. Plain GC object (no `.close()`). Every result is a fresh `TypedArray` or object that
never aliases native memory.

Two shapes come back, and they compose: a **mask** is a `Uint8Array` with one byte per row (nonzero =
selected), and a **column result** is a `Float64Array` of `ROWS` elements — the same length as the
frame, so it can be handed straight to `new DataFrame`.

## Columns and construction

### `new DataFrame(columns)`

`COLUMNS` is an object mapping column name → `TypedArray | string[]`. Property order is the column
order. All columns must have the same length.

| Column value | Becomes |
|---|---|
| `Float64Array` \| `Float32Array` | float column |
| `Int32Array` \| `Uint32Array` \| `Int16Array` \| `Uint16Array` \| `Int8Array` \| `Uint8Array` | integer column |
| `string[]` | dictionary-encoded string column |

Columns are discriminated by **class id**, never by element width — an `Int32Array` and a
`Float32Array` are both 4 bytes and are never confused. `Uint8ClampedArray` and the `BigInt` arrays
are not accepted: clamped has different write semantics, and a `BigInt` element does not fit a
double.

**Throws** `TypeError` on an unsupported column value, `RangeError` if the columns are ragged or
there are more than 1024 of them.

| Getter | Type | Description |
|---|---|---|
| `.ROWS` | `number` | Row count (the common column length). |
| `.COLS` | `number` | Column count. |
| `.COLUMNS` | `string[]` | Column names, in construction order. |

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
  price: new Float64Array([10, 20, 30, 40]),
  qty:   new Int32Array([1, 2, 3, 4]),
  city:  ["ny", "sf", "ny", "la"],
});

console.log(df.ROWS, df.COLS, df.COLUMNS);  // 4 3 [ "price", "qty", "city" ]
```

## Reductions

Every reduction takes an optional trailing `mask`. A masked reduction **excludes** the masked-out
rows — it does not replace them with zero — so a masked-out `Infinity` or `NaN` cannot reach the
result. See [The numeric contract](#the-numeric-contract) for exactly what each one does with `NaN`,
infinities and an empty selection.

| Method | Signature | Description |
|---|---|---|
| `.SUM(col, mask?)` | `(string, Uint8Array?) → number` | Sum of the selected rows. |
| `.MIN(col, mask?)` / `.MAX(col, mask?)` | `→ number \| undefined` | Extremes, ignoring `NaN`; `undefined` when nothing is selected. |
| `.MEAN(col, mask?)` | `→ number` | `sum / count`; `NaN` when nothing is selected. |
| `.COUNT(col, mask?)` | `→ number` | Selected row count (`ROWS` with no mask). |
| `.PRODUCT(col, mask?)` | `→ number` | Product of the selected rows. |
| `.VARIANCE(col, mask?)` | `→ number` | **Sample** variance, `n−1` divisor; `NaN` below two selected rows. |
| `.STDDEV(col, mask?)` | `→ number` | `Math.sqrt` of `VARIANCE`. |
| `.DOT_PRODUCT(a, b, mask?)` | `(string, string, Uint8Array?) → number` | Σ `a[i]·b[i]` over the selected rows. Runs over `MIN(a.length, b.length)`. |

`VARIANCE` and `STDDEV` use a **two-pass** algorithm — a first pass for the mean, a second for the
squared deviations — not Welford and not `SUM(x²) − n·µ²`. The algebraic shortcut needs only one pass
but cancels catastrophically when the mean is large relative to the spread, which is the normal case
for a price or a timestamp column.

**Throws** `TypeError` on a string column, `RangeError` on an unknown column name, `TypeError` if
`mask` is not a `Uint8Array` of at least `ROWS` bytes.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ price: new Float64Array([10, 20, 30, 40]) });

console.log(df.SUM("price"));                  // 100
console.log(df.MEAN("price"));                 // 25
console.log(df.MIN("price"), df.MAX("price")); // 10 40
console.log(df.VARIANCE("price").toFixed(4));  // 166.6667

const big = df.GT("price", 15);
console.log(df.SUM("price", big), df.COUNT("price", big));  // 90 3
```

### Bitwise reductions

| Method | Signature | Description |
|---|---|---|
| `.BITWISE_AND(col, mask?)` | `(string, Uint8Array?) → number` | AND-fold of the selected rows. |
| `.BITWISE_OR(col, mask?)` | `→ number` | OR-fold. |
| `.BITWISE_XOR(col, mask?)` | `→ number` | XOR-fold. |

Defined on **integer columns only**. A float column is named in the error rather than truncated,
because silently truncating a `Float64Array` to int32 and ANDing it returns a plausible number:

```
TypeError: BITWISE_AND: column 'v' is Float64Array; a bitwise reduction is defined only on
integer columns (Int32/Uint32/Int16/Uint16/Int8/Uint8 Array)
```

A signed column returns a signed result, matching `a &= x` in JS after `ToInt32`. An **unsigned**
column returns the unsigned value, which JS's own bitwise operators cannot express — they coerce to
int32 — but which is what a `Uint32Array` column means. The fold is 32 bits wide for every accepted
width, so the AND identity on an empty unsigned column is `4294967295` on a `Uint8Array` and a
`Uint16Array` too, not `255` or `65535`.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ flags: new Int32Array([0b1100, 0b1010, 0b0110]) });
console.log(df.BITWISE_AND("flags"), df.BITWISE_OR("flags"), df.BITWISE_XOR("flags"));  // 0 14 0

// signed vs unsigned identity over an empty column
console.log(new DataFrame({ v: new Int32Array(0)  }).BITWISE_AND("v"));  // -1
console.log(new DataFrame({ v: new Uint32Array(0) }).BITWISE_AND("v"));  // 4294967295
```

## Masks

A mask is a `Uint8Array` with one byte per row; any nonzero byte is true. Predicates produce one,
and the reductions, `WHERE` and `GROUP_BY_SUM` consume one.

### Predicates → `Uint8Array`

| Method | Signature | Description |
|---|---|---|
| `.GT(col, v)` `.GE(col, v)` `.LT(col, v)` `.LE(col, v)` | `(string, number) → Uint8Array` | Element-wise comparison against a scalar. |
| `.EQ(col, v)` `.NE(col, v)` | `(string, number) → Uint8Array` | Equality / inequality. |
| `.BETWEEN(col, lo, hi)` | `(string, number, number) → Uint8Array` | **Inclusive** both ends. An inverted range selects nothing, which is a meaningful answer. |
| `.IS_NA(col)` / `.NOT_NA(col)` | `(string) → Uint8Array` | Rows whose value is / is not `NaN`. On an integer column `IS_NA` is all zeros. |

### Mask consumers

| Method | Signature | Description |
|---|---|---|
| `.ALL(mask)` / `.ANY(mask)` | `(Uint8Array) → boolean` | Whether every / some byte is nonzero. Vacuous over zero rows: `ALL` → `true`, `ANY` → `false`. |
| `.BITMASK(mask)` | `(Uint8Array) → Uint32Array` | Pack to `CEIL(rows/32)` words, **LSB first** — bit `i` of word `i>>5` is row `i`. |
| `.WHERE(mask, a, b)` | `(Uint8Array, string\|number, string\|number) → Float64Array` | `a` where the byte is nonzero, else `b`. Each of `a` and `b` is a column name (a string) or a number. |

`ALL`, `ANY`, `BITMASK` and `WHERE` take the mask **directly**: it is a required `TypedArray`
argument, not a column name. Passing a column name throws `TypeError: not a TypedArray`, and omitting
the argument throws a `TypeError` naming the byte length the frame requires.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
  price: new Float64Array([10, 20, 30, 40]),
  qty:   new Int32Array([1, 2, 3, 4]),
});

const m = df.BETWEEN("price", 20, 30);
console.log(Array.from(m));              // [ 0, 1, 1, 0 ]
console.log(df.ANY(m), df.ALL(m));       // true false
console.log(Array.from(df.BITMASK(m)));  // [ 6 ]

// pick price where selected, qty elsewhere
console.log(Array.from(df.WHERE(m, "price", "qty")));  // [ 1, 20, 30, 4 ]
console.log(Array.from(df.WHERE(m, 1, 0)));            // [ 0, 1, 1, 0 ]
```

## Element-wise operations → `Float64Array`

Every method here returns a `Float64Array` of `ROWS` elements whatever the input column's type, so
the result lines up with the frame it came from and can be fed straight back into `new DataFrame`.

| Method | Signature | Description |
|---|---|---|
| `.ABS(col)` `.ROUND(col)` `.FLOOR(col)` `.CEIL(col)` | `(string) → Float64Array` | Element-wise, matching the `Math` function of the same name. |
| `.SQRT(col)` `.LOG(col)` `.EXP(col)` | `(string) → Float64Array` | Element-wise; `SQRT` and `LOG` of a negative give `NaN`. |
| `.SIGN(col)` | `(string) → Float64Array` | `-1`, `0` or `1`. |
| `.CLIP(col, lo, hi)` | `(string, number, number) → Float64Array` | Clamp into `[lo, hi]`. `NaN` elements pass through unchanged. |
| `.FILL_NA(col, value)` | `(string, number) → Float64Array` | Replace `NaN` with `value`. A `NaN` `value` is legal and is a no-op. |

`CLIP` **refuses** a `NaN` bound (`TypeError`) and an inverted range (`RangeError`). Both are caller
errors with a plausible-looking result: every comparison against `NaN` is false, so a `NaN` bound
would silently turn `CLIP` into a copy, and `CLIP(3, 5, 1)` is `5` — a value outside the range asked
for. `BETWEEN` accepts an inverted range because there it means "select nothing", which is an answer.

### Arithmetic

| Method | Signature | Description |
|---|---|---|
| `.ADD(col, rhs)` `.SUB(col, rhs)` `.MUL(col, rhs)` `.DIV(col, rhs)` `.POW(col, rhs)` | `(string, string\|number) → Float64Array` | `rhs` is a **column** when it is a string, otherwise it is coerced to a number. |
| `.RSUB(col, k)` | `(string, number) → Float64Array` | `k − col`. |
| `.RDIV(col, k)` | `(string, number) → Float64Array` | `k / col`. |

`RSUB` and `RDIV` exist because subtraction and division are not commutative and `100 - col` has no
other cheap spelling. They take a **number only** — `RSUB(a, b)` on two columns is just `SUB(b, a)`,
and passing a column name says so:

```
TypeError: rsub: the right operand must be a number; for two columns use sub with them swapped
```

Two-column operations run over `MIN(a.length, b.length)` rows.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
  price: new Float64Array([10, 20, 30, 40]),
  qty:   new Int32Array([1, 2, 3, 4]),
});

console.log(Array.from(df.MUL("price", "qty")));   // [ 10, 40, 90, 160 ]
console.log(Array.from(df.RSUB("price", 100)));    // [ 90, 80, 70, 60 ]
console.log(Array.from(df.CLIP("price", 15, 35))); // [ 15, 20, 30, 35 ]

// a result is a column: feed it straight back
const totals = new DataFrame({ total: df.MUL("price", "qty") });
console.log(totals.SUM("total"));                  // 300
```

## Grouping

### `GROUP_BY_SUM(keyCol, valueCol, mask?)`

Returns `{ keys, values }` — `keys` is a `string[]` for a dictionary-encoded key column or a
`number[]` of codes for an integer one, and `values` is a `Float64Array` of the same length holding
the sum of `valueCol` per group.

The key column must be a **string column or an integer column**: the codes index the accumulator
directly, so this is a single scatter-add pass with no hashing at all. That is the whole reason
string columns are dictionary-encoded at construction.

An **integer** key column is the slower of the two. Groups are `0..max`, so the cardinality has to be
discovered with a full extra pass before anything can be allocated — a hostile column must not be
able to demand an unbounded allocation — whereas the dictionary already knows its own cardinality.
Pass a string column, or an explicit code column whose range you know, when that pass matters.
Negative keys and a cardinality above 2²⁰ are refused with `RangeError`.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
  city: ["ny", "sf", "ny", "la", "sf"],
  amt:  new Float64Array([1, 2, 3, 4, 5]),
});

const g = df.GROUP_BY_SUM("city", "amt");
console.log(g.keys);                  // [ "ny", "sf", "la" ]
console.log(Array.from(g.values));    // [ 4, 7, 4 ]

// grouped over a subset
const sel = df.GROUP_BY_SUM("city", "amt", new Uint8Array([1, 1, 0, 0, 1]));
console.log(Array.from(sel.values));  // [ 1, 7, 0 ]
```

## Ordering and order statistics

Every method here works on the **selected** rows only when given a mask, and every one refuses a string
column: a dictionary code is an insertion counter, not a sortable value, so ordering by it would return
a plausible wrong answer rather than an error.

| Method | Signature | Returns |
|---|---|---|
| `SORT` | `(column, mask?)` | `Float64Array` — selected values ascending |
| `ARG_SORT` | `(column, mask?)` | `Uint32Array` — original row indices in sorted order |
| `RANK` | `(column, mask?)` | `Float64Array`, `ROWS` long — 1-based rank, ties averaged |
| `MEDIAN` | `(column, mask?)` | `Number \| undefined` |
| `QUANTILE` | `(column, q, mask?)` | `Number \| undefined` — linear interpolation |
| `PERCENTILE_CONT` | `(column, p, mask?)` | `Number \| undefined` — same as `QUANTILE` |
| `PERCENTILE_DISC` | `(column, p, mask?)` | `Number \| undefined` — an actual element |
| `N_LARGEST` | `(column, k, mask?)` | `Float64Array` — k largest, descending |
| `N_SMALLEST` | `(column, k, mask?)` | `Float64Array` — k smallest, ascending |

`SORT` is **stable**: equal values keep their original relative order, so `ARG_SORT` is reproducible.
`NaN` sorts **last** in both directions — a comparator that merely compares NaN reports every pair equal
and leaves an unspecified permutation, so NaN is *placed* rather than compared.

`MEDIAN` is `PERCENTILE_CONT` with `q` fixed at `0.5` — one implementation, so the two cannot drift.
`RANK` averages ties because that is the only rule whose column sum stays `n(n+1)/2`, which is what lets
it compose with `SUM` and `MEAN`.

```js
import { DataFrame } from "dyna:dataframe";

const v = Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]);
const df = new DataFrame({ v });

console.log(df.SORT("v"));            // Float64Array(8) [ 1, 1, 2, 3, 4, 5, 6, 9 ]
console.log(df.ARG_SORT("v"));         // Uint32Array(8) [ 1, 3, 6, 0, 2, 4, 7, 5 ]
console.log(df.RANK("v"));            // [4, 1.5, 5, 1.5, 6, 8, 3, 7] — the two 1s share rank 1.5
console.log(df.MEDIAN("v"));          // 3.5   — even count, so it interpolates
console.log(df.QUANTILE("v", 0.25));  // 1.75
console.log(df.N_LARGEST("v", 3));     // Float64Array(3) [ 9, 6, 5 ]
console.log(df.N_SMALLEST("v", 3));    // Float64Array(3) [ 1, 1, 2 ]
```

`PERCENTILE_DISC` returns a value that is **present in the column**; `PERCENTILE_CONT` interpolates and
generally does not. On an integer column the difference is visible immediately:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Int32Array.from([1, 2, 3, 4]) });
console.log(df.PERCENTILE_CONT("v", 0.5));   // 2.5 — an interpolated value, not in the column
console.log(df.PERCENTILE_DISC("v", 0.5));   // 2   — an element of the column
```

The edge cases. An order statistic of nothing is `undefined`, not `NaN` — `MIN`/`MAX` already answer
that way, and `MEAN`/`VARIANCE` answer `NaN`, so the two families stay distinguishable. An out-of-range
quantile is refused rather than clamped, because a clamped `q` returns a plausible number produced from
a caller error:

```js
import { DataFrame } from "dyna:dataframe";

const empty = new DataFrame({ v: new Float64Array(0) });
console.log(empty.MEDIAN("v"));                       // undefined

const nan = new DataFrame({ v: Float64Array.from([NaN, NaN]) });
console.log(nan.MEDIAN("v"));                         // undefined — no non-NaN value to return
console.log(new DataFrame({ v: Float64Array.from([1, NaN, 3]) }).SORT("v"));
                                                      // [1, 3, NaN] — NaN placed last

const df = new DataFrame({ v: Float64Array.from([1, 2, 3]) });
try { df.QUANTILE("v", 1.5); } catch (e) { console.log(e.message); }
// quantile(col, q): q must be in [0, 1], got 1.5
```

## Cumulative scans and shifts

| Method | Signature | Returns |
|---|---|---|
| `CUM_SUM` `CUM_PROD` `CUM_MAX` `CUM_MIN` | `(column, mask?)` | `Float64Array`, `ROWS` long |
| `SHIFT` | `(column, periods)` | `Float64Array` — positive lags, negative leads |
| `DIFF` | `(column, periods)` | `Float64Array` — `x[i] - x[i - periods]` |

Results are always `ROWS` long, so they line up with the frame they came from and can be handed
straight back to `new DataFrame`. Under a mask, a skipped row's slot carries the **running
accumulator** rather than `NaN`, which makes `CUM_SUM(col, mask)[rows - 1]` equal `SUM(col, mask)`.

`CUM_SUM` and `CUM_PROD` propagate `NaN` from the index where it appears onward — arithmetic forces it.
`CUM_MAX` and `CUM_MIN` **ignore** it, matching scalar `MAX` and `MIN`.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]) });

console.log(df.CUM_SUM("v"));      // [3, 4, 8, 9, 14, 23, 25, 31]
console.log(df.CUM_PROD("v"));     // [3, 3, 12, 12, 60, 540, 1080, 6480]
console.log(df.CUM_MAX("v"));      // [3, 3, 4, 4, 5, 9, 9, 9]
console.log(df.CUM_MIN("v"));      // [3, 1, 1, 1, 1, 1, 1, 1]
console.log(df.SHIFT("v", 2));    // [NaN, NaN, 3, 1, 4, 1, 5, 9]
console.log(df.SHIFT("v", -2));   // [4, 1, 5, 9, 2, 6, NaN, NaN]  — a negative period leads
console.log(df.DIFF("v", 1));     // [NaN, -2, 3, -3, 4, 4, -7, 4]
```

Out-of-range slots are filled with `NaN`, never `0` — a zero would read as "no change", which is a
plausible wrong answer. `DIFF` is the lag-k difference `x[i] - x[i-k]`, not the iterated k-th
difference, which is pinned by the identity `DIFF(c, k) === sub(c, shift(c, k))`.

```js
import { DataFrame } from "dyna:dataframe";

const nan = new DataFrame({ v: Float64Array.from([1, NaN, 3]) });
console.log(nan.CUM_SUM("v"));   // [1, NaN, NaN] — poisoned from the NaN onward
console.log(nan.CUM_MAX("v"));   // [1, 1, 3]     — NaN ignored, as scalar max does

const df = new DataFrame({ v: Float64Array.from([1, 2, 3]) });
try { df.DIFF("v", 2.5); } catch (e) { console.log(e.message); }
// diff(col, periods): periods must be an integer
```

A fractional period is refused rather than truncated, because truncation turns a caller's `2.5` into a
silent lag of 2, and an `Int32` coercion would wrap `4294967297` to `1`.

## Distinct values and frequency

| Method | Signature | Returns |
|---|---|---|
| `UNIQUE` | `(column, mask?)` | `Float64Array`, or `String[]` for a string column |
| `N_UNIQUE` | `(column, mask?)` | `Number` — distinct values **present** |
| `VALUE_COUNTS` | `(column, mask?)` | `{ keys, values }` — count-descending |
| `TOP_K` | `(column, k, mask?)` | `{ keys, values }` — the k most frequent |
| `MODE` | `(column, mask?)` | the most frequent value, or `undefined` |
| `DROP_DUPLICATES` | `(column, mask?)` | `Uint8Array` mask, `ROWS` long |
| `GROUP_ARRAY` | `(keyCol, valueCol, mask?)` | `{ keys, values }` — values per group |
| `GROUP_UNIQ_ARRAY` | `(keyCol, valueCol, mask?)` | `{ keys, values }` — deduplicated per group |

Keys compare by `SameValueZero`: `NaN` equals itself and counts as one distinct value, and `-0`
collides with `+0`. That is the rule `new Set()` uses, which is the reference a caller will check
against. `UNIQUE` returns values in **first-seen** order; `VALUE_COUNTS` is sorted by count descending
with first appearance breaking ties, so `TOP_K` is exactly its prefix.

`N_UNIQUE` counts distinct codes **actually present**, never the dictionary size. On a whole column the
two agree today, but under a mask they do not — and "distinct cities among orders over $100" is the
query people run.

```js
import { DataFrame } from "dyna:dataframe";

const v = Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]);
const k = ["a", "b", "a", "c", "b", "a", "c", "b"];
const df = new DataFrame({ v, k });

console.log(df.UNIQUE("v"));        // Float64Array(7) [ 3, 1, 4, 5, 9, 2, 6 ] — first-seen, not sorted
console.log(df.UNIQUE("k"));        // ["a", "b", "c"] — real strings, never dictionary codes
console.log(df.N_UNIQUE("v"));       // 7
console.log(df.MODE("v"));          // 1 — the only value appearing twice

const vc = df.VALUE_COUNTS("k");
console.log(vc.keys, vc.values);    // [ "a", "b", "c" ] Float64Array(3) [ 3, 3, 2 ]

const tk = df.TOP_K("k", 2);
console.log(tk.keys, tk.values);    // [ "a", "b" ] Float64Array(2) [ 3, 3 ]
```

`DROP_DUPLICATES` returns a **mask**, not a compacted column, so it composes with everything else — a
compacted column would change the row count and could not be handed back to the frame:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]) });
const firsts = df.DROP_DUPLICATES("v");
console.log(firsts);                        // Uint8Array(8) [ 1, 1, 1, 0, 1, 1, 1, 1 ] — the repeat 1 is dropped
console.log(df.COUNT("v", firsts));         // 7 — equals nunique, which is the identity that ties them
```

`GROUP_ARRAY` collects each group's values; `GROUP_UNIQ_ARRAY` deduplicates within the group. Both return
keys element-for-element identical to `GROUP_BY_SUM`'s, so the four compose:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    k: ["a", "b", "a", "c", "b", "a", "c", "b"],
    v: Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]),
    w: Float64Array.from([1, 1, 2, 2, 1, 1, 2, 2]),
});

const ga = df.GROUP_ARRAY("k", "v");
console.log(ga.keys);                          // ["a", "b", "c"]
console.log(ga.values.map(g => Array.from(g))); // [[3,4,9], [1,5,6], [1,2]]

const gu = df.GROUP_UNIQ_ARRAY("k", "w");
console.log(gu.values.map(g => Array.from(g))); // [[1,2], [1,2], [2]] — deduplicated per group

const empty = new DataFrame({ e: new Float64Array(0) });
console.log(empty.N_UNIQUE("e"));               // 0
```

## Positional access

| Method | Signature | Returns |
|---|---|---|
| `HEAD` `TAIL` | `(column, n?, mask?)` | `Float64Array` — first/last `n` selected rows, `n` defaults to 5 |
| `FIRST` `LAST` | `(column, mask?)` | the first/last selected value, or `undefined` |
| `ARG_MIN` `ARG_MAX` | `(column, mask?)` | absolute row index, or `undefined` |

These copy; they never alias native memory, which is what every other result in this module does too.
`HEAD` and `TAIL` clamp when `n` exceeds the row count and refuse a negative `n`. `TAIL` returns rows in
ascending order, so `HEAD(k)` followed by `TAIL(rows - k)` reconstructs the column.

**`ARG_MIN` agrees with `MIN`, including on NaN**: both ignore it. They part company in exactly one
place, and it is asserted rather than hidden — an all-NaN column has a `MIN` of `+Infinity`, the
identity, but there is no row to point at, so `ARG_MIN` is `undefined`.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]) });

console.log(df.HEAD("v", 3));   // Float64Array(3) [ 3, 1, 4 ]
console.log(df.TAIL("v", 3));   // Float64Array(3) [ 9, 2, 6 ]
console.log(df.HEAD("v"));      // Float64Array(5) [ 3, 1, 4, 1, 5 ] — n defaults to 5
console.log(df.FIRST("v"));     // 3
console.log(df.LAST("v"));      // 6
console.log(df.ARG_MIN("v"));    // 1 — first occurrence of the minimum, ties go to the first
console.log(df.ARG_MAX("v"));    // 5
```

```js
import { DataFrame } from "dyna:dataframe";

const nan = new DataFrame({ v: Float64Array.from([NaN, NaN]) });
console.log(nan.MIN("v"));       // Infinity  — the identity, because min ignores NaN
console.log(nan.ARG_MIN("v"));    // undefined — there is no row to point at

const empty = new DataFrame({ v: new Float64Array(0) });
console.log(empty.FIRST("v"));   // undefined

const df = new DataFrame({ v: Float64Array.from([1, 2, 3]) });
try { df.HEAD("v", -1); } catch (e) { console.log(e.message.slice(0, 46)); }
// head(col, n): n must be a non-negative number
```

## Moments, correlation and regression

Fourteen methods over one two-pass loop. Each is a closed form on the same running quantities, so there
is a single implementation to keep correct.

| Method | Signature | Returns |
|---|---|---|
| `VARIANCE_POP` `STDDEV_POP` | `(column, mask?)` | population (÷n) forms of `VARIANCE`/`STDDEV` |
| `SKEW` `KURTOSIS` | `(column, mask?)` | third and fourth standardised moments; kurtosis is excess |
| `MEAN_WEIGHTED` | `(column, weightCol, mask?)` | weighted arithmetic mean |
| `COV_POP` `COV_SAMP` | `(a, b, mask?)` | covariance, ÷n and ÷(n−1) |
| `CORR` | `(a, b, mask?)` | Pearson correlation |
| `REGR_SLOPE` `REGR_INTERCEPT` `REGR_R2` `REGR_AVG_X` `REGR_AVG_Y` | `(y, x, mask?)` | least-squares fit of y on x |
| `DESCRIBE` | `(column, mask?)` | `{ count, sum, mean, min, max, variance, stddev, skew, kurtosis }` |

`VARIANCE` and `STDDEV` remain the **sample** (÷(n−1)) forms; `VARIANCE_POP` and `STDDEV_POP` are the new
population ones. All of them use the two-pass algorithm — mean first, then centred powers — because the
naive `Σx² − (Σx)²/n` is catastrophically wrong on large-mean, small-variance data, and third and
fourth powers make that cancellation worse, not better.

```js
import { DataFrame } from "dyna:dataframe";

const v = Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]);
const w = Float64Array.from([1, 1, 2, 2, 1, 1, 2, 2]);
const df = new DataFrame({ v, w });

console.log(df.VARIANCE("v"));      // 7.553571428571429   — sample, ÷(n-1)
console.log(df.VARIANCE_POP("v"));   // 6.609375            — population, ÷n
console.log(df.SKEW("v"));          // 0.6682892518272332
console.log(df.KURTOSIS("v"));      // -0.534949616887145  — excess, so a normal sample is ~0
console.log(df.MEAN_WEIGHTED("v", "w"));  // 3.6666666666666665
console.log(df.CORR("v", "v"));     // 1 — a column correlates perfectly with itself

const d = df.DESCRIBE("v");
console.log(d.count, d.mean, d.min, d.max);   // 8 3.875 1 9
```

The regression family fits `y` on `x` and shares the same one loop:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    y: Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]),
    x: Float64Array.from([1, 1, 2, 2, 1, 1, 2, 2]),
});

console.log(df.REGR_SLOPE("y", "x"));      // -1.25
console.log(df.REGR_INTERCEPT("y", "x"));  // 5.75
console.log(df.REGR_R2("y", "x"));         // 0.05910165484633569
console.log(df.REGR_AVG_X("y", "x"));       // 1.5
console.log(df.REGR_AVG_Y("y", "x"));       // 3.875
console.log(df.COV_POP("y", "x"));         // -0.3125
console.log(df.COV_SAMP("y", "x"));        // -0.35714285714285715
```

The degenerate cases. A correlation needs both columns to vary; with zero variance the ratio is `0/0`
and the answer is `NaN` rather than an invented number. A third moment needs at least three points:

```js
import { DataFrame } from "dyna:dataframe";

const flat = new DataFrame({
    a: Float64Array.from([1, 1, 1]),
    b: Float64Array.from([1, 2, 3]),
});
console.log(flat.CORR("a", "b"));   // NaN — a has no variance, so the ratio is 0/0

const two = new DataFrame({ a: Float64Array.from([1, 2]) });
console.log(two.SKEW("a"));         // 0 — fewer than three points carry no skew
```

## Logical reductions and checked sums

| Method | Signature | Returns |
|---|---|---|
| `BOOL_AND` `BOOL_OR` `BOOL_XOR` | `(column, mask?)` | `Boolean` — folds JS **truthiness** of each element |
| `SUM_CHECKED` | `(column, mask?)` | `Number` — an integer sum that refuses to round |

These are not `ALL`/`ANY`, which take a `Uint8Array` mask directly and fold over bytes. `BOOL_AND` and
friends take a **column name**, work on any of the eight numeric dtypes, and use JS truthiness — so
`NaN` is **false**, and so is `-0`. A naive `v != 0` gets NaN backwards, since `NaN != 0` is true.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    t:  Float64Array.from([1, 2, 3]),
    f:  Float64Array.from([1, 0, 3]),
    nn: Float64Array.from([1, NaN, 3]),
});

console.log(df.BOOL_AND("t"));    // true
console.log(df.BOOL_AND("f"));    // false — a zero is falsy
console.log(df.BOOL_AND("nn"));   // false — NaN is falsy, as Boolean(NaN) is
console.log(df.BOOL_OR("f"));     // true
console.log(df.BOOL_XOR("f"));    // false — an even number of truthy elements

const empty = new DataFrame({ e: new Float64Array(0) });
console.log(empty.BOOL_AND("e")); // true — the identity of a conjunction
```

`SUM_CHECKED` exists because `SUM` cannot be exact at the JS boundary. The accumulator is a 64-bit
integer and never overflows for 32-bit elements, but the **return value is a double**, so a total past
2⁵³ is rounded silently. `SUM_CHECKED` detects that and throws instead of answering:

```js
import { DataFrame } from "dyna:dataframe";

const v = new Uint32Array(2097153).fill(4294967295);
v[v.length - 1] = 2097153;          // exact total is 2**53 + 1

console.log(sumIsExact());
function sumIsExact() {
    const df = new DataFrame({ v });
    const rounded = df.SUM("v");     // 9007199254740992 — off by one, and silent
    try {
        df.SUM_CHECKED("v");
        return "SUM_CHECKED agreed: " + rounded;
    } catch (e) {
        return "SUM_CHECKED refused: " + e.message.slice(0, 40);
    }
}
// SUM_CHECKED refused: SUM_CHECKED: column 'v' totals 9007199
```

`SUM_CHECKED` accepts integer columns only; a float column has no exact integer total to check, so it
refuses rather than pretending.

## Grouped aggregates and rolling windows

| Method | Signature | Returns |
|---|---|---|
| `GROUP_BY_MEAN` `GROUP_BY_MIN` `GROUP_BY_MAX` | `(keyCol, valueCol, mask?)` | `{ keys, values }` |
| `GROUP_BY_COUNT` | `(keyCol, mask?)` | `{ keys, values }` — no value column |
| `SUM_MAP` `MIN_MAP` `MAX_MAP` | `(keyCol, valueCol, mask?)` | aliases of `GROUP_BY_SUM`/`Min`/`Max` |
| `ROLLING_SUM` `ROLLING_MEAN` `ROLLING_MIN` `ROLLING_MAX` | `(column, window, mask?)` | `Float64Array`, `ROWS` long |
| `DROP_NA` | `(columns?)` | `Uint8Array` mask of rows where every named column is non-`NaN` |

The key column follows `GROUP_BY_SUM` exactly: an integer or dictionary-encoded string column, negative
keys refused, and cardinality capped. `GROUP_BY_COUNT` takes **no** value column, and refuses one rather
than silently reinterpreting it as a mask.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    k: ["a", "b", "a", "c", "b", "a", "c", "b"],
    v: Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]),
});

console.log(df.GROUP_BY_MEAN("k", "v").values);   // [5.333333333333333, 4, 1.5]
console.log(df.GROUP_BY_MIN("k", "v").values);    // [3, 1, 1]
console.log(df.GROUP_BY_MAX("k", "v").values);    // [9, 6, 2]
console.log(df.GROUP_BY_COUNT("k").values);       // [3, 3, 2]
console.log(df.SUM_MAP("k", "v").values);        // [16, 12, 3] — the same function as GROUP_BY_SUM
```

A rolling window's first `window - 1` slots are `NaN`: the window has not filled, and there is no
partial answer that is not a different statistic. `ROLLING_MEAN` divides by the contributing count.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6]) });

console.log(df.ROLLING_SUM("v", 3));   // [NaN, NaN, 8, 6, 10, 15, 16, 17]
console.log(df.ROLLING_MIN("v", 3));   // [NaN, NaN, 1, 1, 1, 1, 2, 2]
console.log(df.ROLLING_MAX("v", 3));   // [NaN, NaN, 4, 4, 5, 9, 9, 9]
```

A window longer than the column is legal and every slot is `NaN` — that is a value, not a caller error.
A window of zero is not, because there is no statistic it could mean:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([1, 2, 3]) });
console.log(df.ROLLING_SUM("v", 99));   // [NaN, NaN, NaN] — never fills, which is an answer
try { df.ROLLING_SUM("v", 0); } catch (e) { console.log(e.message.slice(0, 48)); }
// ROLLING_SUM: window must be a positive integer, got
```

`DROP_NA` returns a mask of the rows that are complete across **every** named column — the multi-column
conjunction is the capability, since a single-column form would be `NOT_NA` under another name:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    a: Float64Array.from([1, NaN, 3]),
    b: Float64Array.from([1, 2, NaN]),
});

console.log(df.DROP_NA("a"));    // Uint8Array(3) [ 1, 0, 1 ] — rows where a is present
console.log(df.DROP_NA());       // Uint8Array(3) [ 1, 0, 0 ] — rows where EVERY column is present
```

## Approximate aggregates

Four sketches with fixed memory, for cardinalities and distributions too large to hold exactly. These
are the only methods in the module allowed to be wrong, so each publishes an error bound.

| Method | Signature | Structure | Bound |
|---|---|---|---|
| `APPROX_COUNT_DISTINCT` | `(column, mask?)` | HyperLogLog, 16 KB | 5% relative |
| `APPROX_PERCENTILE` | `(column, q, mask?)` | t-digest, ~37 KB | rank error ≤ 0.01 |
| `APPROX_TOP_K` | `(column, k, mask?)` | Space-Saving | never under-counts; exact above N/m |
| `APPROX_SIMILARITY` | `(a, b, mask?)` | bottom-k MinHash | 0.10; exact below 256 distinct |

Every sketch is seeded by a compile-time constant and hashes through explicit little-endian bytes, so
the same input gives the same answer on every run and every platform. Estimates are returned unrounded:
rounding one would fake a precision it does not have.

```js
import { DataFrame } from "dyna:dataframe";

const m = new Float64Array(20000);
for (let i = 0; i < m.length; i++) m[i] = i % 4096;   // exactly 4096 distinct values
const df = new DataFrame({ m });

const exact = df.N_UNIQUE("m");
const est = df.APPROX_COUNT_DISTINCT("m");
console.log(exact);                                    // 4096
console.log(Math.abs(est - exact) / exact < 0.05);     // true — inside the documented bound
```

On a dictionary-encoded string column the distinct set is already known, so `APPROX_COUNT_DISTINCT` and
`APPROX_TOP_K` return the **exact** answer rather than estimating something the frame can count:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ k: ["a", "b", "a", "c", "b", "a", "c", "b"] });
console.log(df.APPROX_TOP_K("k", 2).keys);   // ["a", "b"] — exact, matching TOP_K
console.log(df.TOP_K("k", 2).keys);         // ["a", "b"]
```

`APPROX_SIMILARITY` estimates the Jaccard index of two columns' value sets, and is exact while the union
stays under the sketch width:

```js
import { DataFrame } from "dyna:dataframe";

const same = new DataFrame({
    p: Float64Array.from([1, 2, 3, 4]),
    q: Float64Array.from([1, 2, 3, 4]),
});
console.log(same.APPROX_SIMILARITY("p", "q"));   // 1

const none = new DataFrame({
    p: Float64Array.from([1, 2]),
    q: Float64Array.from([3, 4]),
});
console.log(none.APPROX_SIMILARITY("p", "q"));   // 0 — disjoint sets
```

## Higher moments and dispersion

| Method | Signature | Returns |
|---|---|---|
| `SEM` | `(column, mask?)` | standard error of the mean, sample stddev over √n |
| `SKEW_SAMP` `KURT_SAMP` | `(column, mask?)` | the sample-adjusted third and fourth moments |
| `MAD` | `(column, mask?)` | mean of \|x − mean\| |
| `MEDIAN_ABSOLUTE_DEVIATION` | `(column, mask?)` | median of \|x − median\| |
| `ENTROPY` | `(column, mask?)` | Shannon entropy in **bits** over the value distribution |
| `COUNT_NULLS` | `(column, mask?)` | how many selected rows are `NaN` |

`SKEW` and `KURTOSIS` are the **population** forms; these are the sample ones, adjusted by the usual
n-corrections. They differ on any real column, and the sample forms need n ≥ 3 and n ≥ 4 respectively —
below that they are 0 rather than a division by a negative.

`MAD` and `MEDIAN_ABSOLUTE_DEVIATION` are different statistics, not spellings of one: they centre on
different points and fold differently. A symmetric column hides that; an asymmetric one does not.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 20]) });

console.log(df.SEM("v"));         // 1.707825127659933  — STDDEV / sqrt(n)
console.log(df.SKEW("v"));        // 1.6060526914810147  (population)
console.log(df.SKEW_SAMP("v"));   // 1.9045442052463915  (sample)
console.log(df.KURTOSIS("v"));    // 2.0472380952380957  (population, excess)
console.log(df.KURT_SAMP("v"));   // 4.583510204081634   (sample)
console.log(df.MAD("v"));         // 3.6   — centred on the mean
console.log(df.MEDIAN_ABSOLUTE_DEVIATION("v"));   // 2.5 — centred on the median
```

`ENTROPY` is 0 for a constant column, 1 bit for two equally likely values, 2 bits for four. `NaN` is
a **value** to it, as it is throughout the cardinality family, so a column of two ones and two `NaN`s
is a fair two-way split. `COUNT_NULLS` plus the non-null count is always the selected row count:

```js
import { DataFrame } from "dyna:dataframe";

console.log(new DataFrame({ e: Float64Array.from([7, 7, 7]) }).ENTROPY("e"));       // 0
console.log(new DataFrame({ e: Float64Array.from([1, 1, 2, 2]) }).ENTROPY("e"));    // 1
console.log(new DataFrame({ e: Float64Array.from([1, 2, 3, 4]) }).ENTROPY("e"));    // 2

const n = new DataFrame({ a: Float64Array.from([1, NaN, 3, NaN]) });
console.log(n.COUNT_NULLS("a"));                       // 2
console.log(n.COUNT_NULLS("a") + n.COUNT("a", n.NOT_NA("a")));   // 4 — every row
```

## Regression sums and the correlation matrix

| Method | Signature | Returns |
|---|---|---|
| `REGR_COUNT` | `(y, x, mask?)` | rows where both columns contribute |
| `REGR_SXX` `REGR_SYY` `REGR_SXY` | `(y, x, mask?)` | the raw centred sums the fit is built from |
| `CORR_MATRIX` | `([col, …], mask?)` | `{ columns, matrix, n }` — n×n row-major |

`REGR_*(y, x)` takes the **dependent** variable first, so `SXX` is the spread of the *second*
argument. That is what makes `SXY / SXX` reproduce `REGR_SLOPE` exactly; reversing them still computes
something, it just stops being the slope.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    y: Float64Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 20]),
    x: Float64Array.from([1, 1, 2, 2, 1, 1, 2, 2, 1, 1]),
});

console.log(df.REGR_COUNT("y", "x"));   // 10
console.log(df.REGR_SXX("y", "x"));     // 2.4000000000000004  — the x spread
console.log(df.REGR_SYY("y", "x"));     // 262.5               — the y spread
console.log(df.REGR_SXY("y", "x"));     // -4
console.log(df.REGR_SXY("y", "x") / df.REGR_SXX("y", "x"));  // -1.6666666666666665
console.log(df.REGR_SLOPE("y", "x"));                        // -1.6666666666666665
```

`CORR_MATRIX` routes every pair through the same moments `CORR` uses, so no cell can disagree with
`CORR` itself. The matrix is symmetric by construction and carries 1 on the diagonal:

```js
import { DataFrame } from "dyna:dataframe";

const m = new DataFrame({
    x: Float64Array.from([1, 2, 3, 4, 5]),
    y: Float64Array.from([2, 4, 6, 8, 10]),   // exactly 2x
    z: Float64Array.from([5, 4, 3, 2, 1]),    // exactly reversed
});
const cm = m.CORR_MATRIX(["x", "y", "z"]);

console.log(cm.n);            // 3
console.log(cm.columns);      // [ "x", "y", "z" ]
console.log(cm.matrix);
// Float64Array(9) [ 1, 0.9999999999999998, -0.9999999999999998,
//                   0.9999999999999998, 1, -0.9999999999999998,
//                   -0.9999999999999998, -0.9999999999999998, 1 ]
// a perfect correlation lands a ulp below 1: the cell IS what CORR returns
console.log(cm.matrix[1] === m.CORR("x", "y"));   // true — a cell IS CORR
```

## Quantile variants and histograms

| Method | Signature | Returns |
|---|---|---|
| `QUANTILE_EXACT_LOW` `QUANTILE_EXACT_HIGH` | `(column, q, mask?)` | the order statistic below / above the position |
| `QUANTILES` | `(column, [q, …], mask?)` | `Float64Array`, one per q, from ONE sort |
| `QUANTILES_TDIGEST` | `(column, [q, …], mask?)` | one per q, off ONE approximate digest (bounded memory) |
| `QUANTILE_EXACT_WEIGHTED` | `(column, weightCol, q, mask?)` | the value where cumulative weight reaches q |
| `HISTOGRAM` `HISTOGRAM_NORMALIZED` | `(column, bins, mask?)` | `{ edges, counts }` |
| `UNIQ_UP_TO` | `(column, n, mask?)` | exact distinct count, stopping once it passes n |

`_LOW` and `_HIGH` both return a value **present in the column**, unlike `PERCENTILE_CONT` which
interpolates between them:

```js
import { DataFrame } from "dyna:dataframe";

const q = new DataFrame({ v: Int32Array.from([1, 2, 3, 4]) });
console.log(q.QUANTILE_EXACT_LOW("v", 0.5));    // 2
console.log(q.QUANTILE_EXACT_HIGH("v", 0.5));   // 3
console.log(q.PERCENTILE_CONT("v", 0.5));       // 2.5 — not in the column

const df = new DataFrame({ v: Float64Array.from([1,2,3,4,5,6,7,8,9,20]) });
console.log(df.QUANTILES("v", [0, 0.25, 0.5, 0.75, 1]));
// Float64Array(5) [ 1, 3.25, 5.5, 7.75, 20 ]
console.log(df.UNIQ_UP_TO("v", 5));     // 6  — stops one past the cap
console.log(df.UNIQ_UP_TO("v", 100));   // 10 — under the cap it is exact
```

`HISTOGRAM` uses equal-width bins over the observed range with an **inclusive top edge**, so the
maximum is never lost, and the counts always sum to the non-NaN selected row count:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([1,2,3,4,5,6,7,8,9,20]) });
const h = df.HISTOGRAM("v", 4);

console.log(h.edges);    // Float64Array(5) [ 1, 5.75, 10.5, 15.25, 20 ]
console.log(h.counts);   // Float64Array(4) [ 5, 4, 0, 1 ]
console.log(df.HISTOGRAM_NORMALIZED("v", 4).counts);
// Float64Array(4) [ 0.5, 0.4, 0, 0.1 ]  — sums to 1
```

The edge cases: a zero-width range does not divide by zero, a zero bin count is refused, and an
out-of-range q is refused rather than clamped.

```js
import { DataFrame } from "dyna:dataframe";

const flat = new DataFrame({ c: Float64Array.from([5, 5, 5]) });
console.log(flat.HISTOGRAM("c", 3).counts);   // Float64Array(3) [ 3, 0, 0 ]

const df = new DataFrame({ v: Float64Array.from([1, 2, 3]) });
try { df.HISTOGRAM("v", 0); } catch (e) { console.log(e.message.slice(0, 44)); }
// HISTOGRAM(col, bins): bins must be a positive
try { df.QUANTILES("v", [0.5, 2]); } catch (e) { console.log(e.message.slice(0, 40)); }
// QUANTILES: q must be in [0, 1], got 2
```

## Weighted aggregates

A weight column turns frequency into mass. Every method here has an unweighted sibling, and the two
give **different answers** on the same data — which is the point, and what the tests pin.

| Method | Signature | Returns |
|---|---|---|
| `TOP_K_WEIGHTED` `APPROX_TOP_SUM` | `(column, weightCol, k, mask?)` | `{ keys, values }` ranked by summed weight |
| `ANY_HEAVY` | `(column, weightCol?, mask?)` | the value holding **strictly** over half the weight, else `undefined` |
| `QUANTILE_EXACT_WEIGHTED` | `(column, weightCol, q, mask?)` | q-th value by cumulative weight |

```js
import { DataFrame } from "dyna:dataframe";

/* value 1 is rare but heavy; 3 is frequent but light */
const df = new DataFrame({
    v: Float64Array.from([1, 2, 2, 3, 3, 3]),
    w: Float64Array.from([10, 1, 1, 1, 1, 1]),
});

console.log(df.TOP_K_WEIGHTED("v", "w", 2).keys);   // [ 1, 3 ]  — by weight
console.log(df.TOP_K("v", 2).keys);                 // [ 3, 2 ]  — by count
console.log(df.APPROX_TOP_SUM("v", "w", 2).keys);   // [ 1, 3 ]  — same as weighted
console.log(df.ANY_HEAVY("v", "w"));                // 1
console.log(df.ANY_HEAVY("v"));                     // undefined — none exceeds half
console.log(df.QUANTILE_EXACT_WEIGHTED("v", "w", 0.5));   // 1
console.log(df.MEDIAN("v"));                              // 2.5
```

## Grouped bitwise, collection and intersection

| Method | Signature | Returns |
|---|---|---|
| `GROUP_BIT_AND` `GROUP_BIT_OR` `GROUP_BIT_XOR` | `(keyCol, valueCol, mask?)` | `{ keys, values }`, integer columns only |
| `GROUP_CONCAT` | `(column, separator?, mask?)` | `String`, joined in **row** order |
| `GROUP_ARRAY_SORTED` | `(keyCol, valueCol, mask?)` | each group's values ascending |
| `GROUP_ARRAY_LAST` `GROUP_ARRAY_SAMPLE` | `(keyCol, valueCol, k, mask?)` | at most k per group |
| `GROUP_ARRAY_INTERSECT` | `(keyCol, valueCol, mask?)` | values present in EVERY group |

One group folded is exactly the scalar `BITWISE_*`, and every collection variant shares
`GROUP_BY_SUM`'s keys, so the families compose:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    k: ["a", "b", "a", "c", "b", "a"],
    bits: Int32Array.from([12, 10, 6, 15, 3, 9]),
    v: Float64Array.from([3, 1, 4, 1, 5, 9]),
});

console.log(df.GROUP_BIT_AND("k", "bits").values);   // Float64Array(3) [ 0, 2, 15 ]
console.log(df.GROUP_BIT_OR("k", "bits").values);    // Float64Array(3) [ 15, 11, 15 ]
console.log(df.GROUP_BIT_XOR("k", "bits").values);   // Float64Array(3) [ 3, 9, 15 ]
console.log(df.GROUP_CONCAT("k"));                   // a,b,a,c,b,a
console.log(df.GROUP_CONCAT("k", " | "));            // a | b | a | c | b | a
console.log(df.GROUP_CONCAT("v", "-"));              // 3-1-4-1-5-9
```

`_LAST` keeps the final k rows a group saw; `_SAMPLE` takes a deterministic stride, so two calls
agree. `GROUP_ARRAY_INTERSECT` empties as soon as one group lacks a value:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    k: ["a", "b", "a", "c", "b", "a"],
    v: Float64Array.from([3, 1, 4, 1, 5, 9]),
});
console.log(df.GROUP_ARRAY_LAST("k", "v", 2).values[0]);     // Float64Array(2) [ 4, 9 ]
console.log(df.GROUP_ARRAY_SAMPLE("k", "v", 1).values[0]);   // Float64Array(1) [ 3 ]

/* a={1,2} b={2,3} c={2}: only 2 is in all three */
const g = new DataFrame({ k: ["a","a","b","b","c"], x: Float64Array.from([1,2,2,3,2]) });
console.log(g.GROUP_ARRAY_INTERSECT("k", "x"));   // Float64Array(1) [ 2 ]
const dis = new DataFrame({ k: ["a","a","b"], x: Float64Array.from([1,2,3]) });
console.log(dis.GROUP_ARRAY_INTERSECT("k", "x"));  // Float64Array(0) []
```

## Time series

| Method | Signature | Returns |
|---|---|---|
| `EMA` | `(column, alpha, mask?)` | `Float64Array`, `ROWS` long — exponential moving average |
| `DELTA_SUM` | `(column, mask?)` | the total of **positive** consecutive differences |
| `DELTA_SUM_TIMESTAMP` | `(column, timeCol, mask?)` | the same, but summed in **timestamp** order, not row order |
| `RATE` | `(valueCol, timeCol, mask?)` | change per unit time across the whole selection |
| `IRATE` | `(valueCol, timeCol, mask?)` | the same, from only the last two selected rows |

`QUANTILES` sorts once and answers exactly; `QUANTILES_TDIGEST` builds a bounded t-digest once and
reads each q from it, so memory is fixed and large columns stay cheap at the cost of a small
approximation. `DELTA_SUM_TIMESTAMP` orders rows by the time column before differencing, so an
out-of-order frame still sums the rises along the timeline.

```js
import { DataFrame } from "dyna:dataframe";

const big = new Float64Array(10000);
for (let i = 0; i < big.length; i++) big[i] = i;
const d = new DataFrame({ v: big });
console.log(Array.from(d.QUANTILES_TDIGEST("v", [0.5, 0.9, 0.99])).map(Math.round));
// [ 5000, 9000, 9900 ]  -- within ~0.1% of the exact [ 5000, 8999, 9899 ]

const ts = new DataFrame({ t: Float64Array.from([3, 1, 2, 4]),
                           x: Float64Array.from([10, 5, 8, 6]) });
console.log(ts.DELTA_SUM("x"));             // 3  -- row order: only 8 > 5
console.log(ts.DELTA_SUM_TIMESTAMP("x", "t"));  // 5  -- time order 5,8,10,6: 3 + 2
```

`EMA` seeds on the first selected value and carries the running value through a masked-out row,
matching the cumulative scans. `DELTA_SUM` ignores drops, which is what makes it a counter total
across resets. `RATE` and `IRATE` need an explicit time column — this module has no implicit ordering.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    v: Float64Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 20]),
    t: Float64Array.from([0, 1, 2, 3, 4, 5, 6, 7, 8, 10]),
});

console.log(df.EMA("v", 0.5)[0]);   // 1     — seeded, not blended
console.log(df.EMA("v", 0.5)[1]);   // 1.5
console.log(df.RATE("v", "t"));     // 1.9   — (20-1)/(10-0)
console.log(df.IRATE("v", "t"));    // 5.5   — (20-9)/(10-8), the last interval only

const c = new DataFrame({ c: Float64Array.from([1, 5, 2, 7]) });
console.log(c.DELTA_SUM("c"));      // 9  — the rises 4 and 5; the drop is ignored
```

An alpha outside `(0, 1]` is refused, and a rate needs at least two selected rows:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: Float64Array.from([1, 2, 3]), t: Float64Array.from([0, 1, 2]) });
const one = new DataFrame({ a: Float64Array.from([1]), b: Float64Array.from([1]) });
console.log(Number.isNaN(one.RATE("a", "b")));   // true
try { df.EMA("v", 0); } catch (e) { console.log(e.message.slice(0, 40)); }
// EMA(col, alpha): alpha must be in (0, 1]
```

## Slope, decay, positional insert, bitmap and ranges

| Method | Signature | Returns |
|---|---|---|
| `BOUNDING_RATIO` | `(xCol, yCol, mask?)` | slope between the **leftmost and rightmost x**, `NaN` under two distinct x |
| `EXPONENTIAL_TIME_DECAYED_AVG` | `(valueCol, timeCol, tau, mask?)` | mean weighted by `exp(-(tMax - t) / tau)` |
| `GROUP_ARRAY_INSERT_AT` | `(valueCol, posCol, size, fill?, mask?)` | `Float64Array(size)`, each value at its own position |
| `GROUP_BITMAP` | `(column, mask?)` | distinct non-negative integers, counted in a bitmap |
| `QUANTILE_TDIGEST_WEIGHTED` | `(column, weightCol, q, mask?)` | approximate weighted quantile, bounded memory |
| `RANGE_AGG` | `(startCol, endCol, mask?)` | `{ starts, ends }` — the half-open ranges merged into their union |
| `RANGE_INTERSECT_AGG` | `(startCol, endCol, mask?)` | `{ start, end }` common to all, or `undefined` |

`BOUNDING_RATIO` and `RATE` look interchangeable and are not: the first picks its two points by **x
value**, the second by **row**. They agree only on a frame already sorted by x.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    x: Float64Array.from([3, 1, 2, 5, 4]),
    y: Float64Array.from([30, 10, 20, 50, 41]),
    t: Float64Array.from([0, 1, 2, 3, 4]),
});

console.log(df.BOUNDING_RATIO("x", "y"));   // 10  — (50-10)/(5-1), the extreme x
console.log(df.RATE("y", "x"));             // 11  — (41-30)/(4-3), the first and last ROWS

console.log(df.EXPONENTIAL_TIME_DECAYED_AVG("y", "t", 0.5));    // 41.65099906779822
console.log(df.EXPONENTIAL_TIME_DECAYED_AVG("y", "t", 1e12));   // 30.200000000012402
console.log(df.MEAN("y"));                                      // 30.2
```

A short `tau` leans on the latest rows; an effectively infinite one collapses onto the plain mean,
which is the property to assert rather than any particular decayed figure. `tau` must be positive.

`GROUP_ARRAY_INSERT_AT` builds a dense array from a value column and a position column. Positions at
or past `size` are dropped rather than growing the array, and a later row overwrites an earlier one
at the same position. `GROUP_BITMAP` agrees with `N_UNIQUE` on every integer column, but is bounded
by the value **range** rather than the row count — past 2^26 it refuses and names `N_UNIQUE`.

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({
    v: Float64Array.from([30, 10, 20, 50, 40]),
    p: Int32Array.from([2, 0, 1, 4, 3]),
    k: Int32Array.from([5, 5, 7, 9, 9]),
});

console.log(df.GROUP_ARRAY_INSERT_AT("v", "p", 6, -1));  // Float64Array(6) [ 10, 20, 30, 40, 50, -1 ]
console.log(df.GROUP_ARRAY_INSERT_AT("v", "p", 3, -1));  // Float64Array(3) [ 10, 20, 30 ]
console.log(df.GROUP_BITMAP("k"), df.N_UNIQUE("k"));     // 3 3
```

Ranges are **half-open**, so `[1,2)` and `[2,3)` join into `[1,3)` with no gap, and an empty or
inverted range covers nothing and is dropped. `RANGE_INTERSECT_AGG` is `undefined` unless every range
overlaps.

```js
import { DataFrame } from "dyna:dataframe";

const rg = new DataFrame({
    lo: Float64Array.from([1, 2, 8, 3, 20]),
    hi: Float64Array.from([4, 6, 9, 5, 25]),
});
const u = rg.RANGE_AGG("lo", "hi");
console.log(u.starts, u.ends);
// Float64Array(3) [ 1, 8, 20 ] Float64Array(3) [ 6, 9, 25 ]
console.log(rg.RANGE_INTERSECT_AGG("lo", "hi"));   // undefined — they are disjoint

const ov = new DataFrame({ lo: Float64Array.from([1,2,3]), hi: Float64Array.from([10,9,8]) });
console.log(ov.RANGE_INTERSECT_AGG("lo", "hi"));   // { start: 3, end: 8 }
```

The weighted digest is approximate where `QUANTILE_EXACT_WEIGHTED` sorts, so compare it by direction
and a tolerance, never by equality:

```js
import { DataFrame } from "dyna:dataframe";

const wv = new DataFrame({
    v: Float64Array.from([1, 2, 2, 3, 3, 3]),
    w: Float64Array.from([10, 1, 1, 1, 1, 1]),
});
console.log(wv.QUANTILE_TDIGEST_WEIGHTED("v", "w", 0.5));   // 1.4545454545454546
console.log(wv.QUANTILE_EXACT_WEIGHTED("v", "w", 0.5));     // 1
console.log(wv.MEDIAN("v"));                                // 2.5 — unweighted
```

## The numeric contract

The rules below are the part of this module that costs a **wrong answer** rather than an error. They
are the contract, not implementation detail.

### Float reductions reassociate

A floating-point reduction folds the column with several independent accumulators and combines them
at the end. The number of accumulators is a compile-time property chosen per column type and per
aggregate; what matters to a caller is the consequence: **the addition order is not left-to-right,
so a float `SUM`, `MEAN`, `VARIANCE`, `STDDEV`, `PRODUCT` or `DOT_PRODUCT` is not bit-identical to the
same arithmetic written as a JS loop.** Both are correctly rounded at every step; they round in
different places.

The difference is real and observable. Over a column holding `1e16` followed by 4095 copies of `1.0`,
a left-to-right loop loses every one of the small addends into the running total, while independent
accumulators let them sum among themselves first:

```js
import { DataFrame } from "dyna:dataframe";

const a = new Float64Array(4096).fill(1);
a[0] = 1e16;
let leftToRight = 0;
for (let i = 0; i < a.length; i++) leftToRight += a[i];

const reassociated = new DataFrame({ v: a }).SUM("v");

console.log(leftToRight);                          // 10000000000000000  — all 4095 lost
console.log(1e16 + 4095);                          // 10000000000004096  — the exact answer
console.log(reassociated > leftToRight);           // true  — recovers most of them
console.log(reassociated <= 1e16 + 4095);          // true  — but not all
```

How much it recovers is a function of the accumulator count, so no exact value is printed here:
that number is not part of the contract and changes when the count does.

Where every partial sum is exact, order cannot matter and the two agree bit-for-bit:

```js
import { DataFrame } from "dyna:dataframe";

const b = new Float64Array(1000);
for (let i = 0; i < 1000; i++) b[i] = i;
let leftToRight = 0;
for (let i = 0; i < 1000; i++) leftToRight += b[i];

console.log(leftToRight === new DataFrame({ v: b }).SUM("v"));  // true
```

### `NaN`: `MIN`/`MAX` ignore it, the additive reductions propagate it

`MIN` and `MAX` compare, and every comparison against `NaN` is false, so a `NaN` never displaces the
running extreme. `SUM`, `MEAN`, `PRODUCT`, `VARIANCE` and `STDDEV` arithmetic on it and yield `NaN`.

| Column | `MIN` | `MAX` | `SUM` / `MEAN` / `PRODUCT` / `VARIANCE` |
|---|---|---|---|
| `[1, NaN, 3]` | `1` | `3` | `NaN` |
| `[NaN, NaN, NaN]` | `Infinity` | `-Infinity` | `NaN` |

An all-`NaN` column exposes the accumulator's seed: nothing ever beat `+Infinity`, so that is what
comes back. This is **not** the same as an empty column — see below.

`CLIP` is a deliberate exception in the other direction: it passes `NaN` **elements** through
unchanged rather than clamping them, while refusing a `NaN` **bound** outright. That is a different
contract from the `MIN`/`MAX` reduction and is not derived from it.

### Infinities, and why masking excludes rather than zeroes

A masked reduction **excludes** the masked-out rows. It does not multiply them by a 0/1 weight,
because `0 * Infinity` is `NaN` — one masked-out infinity would poison the whole result. A column
containing `Infinity` filtered by an ordinary `df.LT("col", 5)` is a routine query, so that would be
a reachable silent wrong answer:

```js
import { DataFrame } from "dyna:dataframe";

const df = new DataFrame({ v: new Float64Array([1, Infinity, 5, -Infinity, 2]) });
const m  = new Uint8Array([1, 0, 1, 0, 1]);

console.log(df.SUM("v", m), df.MIN("v", m), df.MAX("v", m), df.COUNT("v", m));  // 8 1 5 3
console.log(df.SUM("v"), df.MIN("v"), df.MAX("v"));  // NaN -Infinity Infinity
```

Unmasked, the infinities are in the input and behave as IEEE-754 says: `MIN` is `-Infinity`, `MAX` is
`Infinity`, and `SUM` is `NaN` because `+Infinity + -Infinity` is `NaN`.

### An empty selection

An empty column and a mask that selects nothing are the same case. Each aggregate returns its
identity, or a value that says "no rows" where it has no identity:

| Aggregate | Empty result |
|---|---|
| `SUM` | `0` |
| `PRODUCT` | `1` |
| `COUNT` | `0` |
| `MIN` / `MAX` | `undefined` |
| `MEAN` / `VARIANCE` / `STDDEV` | `NaN` |
| `BITWISE_AND` | `-1` on `Int32Array` / `Int16Array` / `Int8Array`; `4294967295` on `Uint32Array` / `Uint16Array` / `Uint8Array` |
| `BITWISE_OR` / `BITWISE_XOR` | `0` |
| `ALL` / `ANY` | `true` / `false` |

`MIN` and `MAX` return `undefined`, not `±Infinity`: an empty column has no minimum, and returning
the accumulator seed would be a plausible-looking number that no row produced.

```js
import { DataFrame } from "dyna:dataframe";

const e = new DataFrame({ v: new Float64Array(0) });
console.log(e.SUM("v"), e.PRODUCT("v"), e.COUNT("v"));  // 0 1 0
console.log(e.MIN("v"), e.MAX("v"));                    // undefined undefined
console.log(e.MEAN("v"));                               // NaN

// a mask selecting nothing behaves identically
const df = new DataFrame({ v: new Float64Array([1, 2, 3]) });
const none = new Uint8Array([0, 0, 0]);
console.log(df.SUM("v", none), df.MIN("v", none));      // 0 undefined
```

### Integer sums round once, at the end

An integer column sums into a 64-bit integer accumulator, with no floating-point rounding at any
step, and is converted to a JS number once at the end. A JS `+=` loop accumulates in a double and
starts losing whole units past 2⁵³:

```js
import { DataFrame } from "dyna:dataframe";

const N = 5000000;
const v = new Int32Array(N).fill(2147483647);

let loop = 0;
for (let i = 0; i < N; i++) loop += v[i];

console.log(new DataFrame({ v }).SUM("v"));  // 10737418235000000  (exact)
console.log(loop);                           // 10737418235805696
console.log(2 ** 53);                        // 9007199254740992
```

### `DOT_PRODUCT` contracts to FMA

The dot kernel is written as `acc += a[i] * b[i]`, and the compiler is permitted to contract that
into a fused multiply-add — clang has done so by default since clang 14, and this build sets no
`-ffp-contract` flag to stop it. On arm64 the kernel compiles to `fmla.2d`. The product is therefore
computed with a **single** rounding rather than two.

Two consequences worth stating plainly:

- `DOT_PRODUCT` differs from a strictly-rounded reference — including the same sum written as
  `MUL` followed by `SUM`, which cannot contract across the two calls.
- The result can differ between builds targeting **different instruction sets**, because whether the
  contraction happens at all depends on the target having an FMA instruction.

This module's math is therefore **not** covered by the cross-platform bit-reproducibility that
`CONFIG_OPENLIBM` provides. That option vendors one implementation of the elementary functions so
`LOG` and `EXP` give identical results on every host; it says nothing about FMA contraction in a
multiply-add loop. Where you need a bit-identical dot product across architectures, sum the products
in JS.

## Errors

| Condition | Error |
|---|---|
| Unknown column name | `RangeError: no such column: '…'` |
| Arithmetic or a reduction on a string column | `TypeError: cannot reduce a string column` / `… cannot apply to a string column` |
| Bitwise reduction on a float column | `TypeError` naming the column, its type and the accepted types |
| Ragged columns at construction | `RangeError: all columns must have the same length (n), got m` |
| Unsupported column value at construction | `TypeError` naming the accepted array types |
| Mask that is not a `Uint8Array` of at least `ROWS` bytes | `TypeError` |
| `CLIP` with a `NaN` bound / an inverted range | `TypeError` / `RangeError` |
| `RSUB` / `RDIV` with a column as the right operand | `TypeError` naming the commutative spelling |
| `GROUP_BY_SUM` on a float key column, a negative key, or > 2²⁰ groups | `TypeError` / `RangeError` |

Arguments are coerced to native values **before** any column pointer is taken, on every method, so a
`valueOf` that detaches a column's `ArrayBuffer` mid-call fails cleanly rather than reading freed
memory. A detached or out-of-bounds buffer is caught at bind time and throws.

---

# Built-in prototype extensions

Hundreds of native methods that ECMAScript itself does not define, installed **non-enumerable** on
the built-in prototypes/constructors via the engine's own method tables (the exact mechanism as
`map`/`filter`). **No import** — present in every build.

**Conventions.**
- **Naming** — plain names; an ES-standard method is never shadowed (`map filter reduce find sort
  slice concat join …` stay ES). There is no `is`, because it would collide with ES `Object.is`.
- **Immutability** — value-producing `Array`/`Object`/`Date` methods return a **new** value. Exceptions:
  `Object.set` (deep-mutates), `Number.times/upto/downto` and `Number.range` (build fresh arrays).
- **Matchers** — many `Array` methods accept **value | predicate `fn` | `RegExp`** (kind resolved once
  per call). Set-ops use SameValueZero; `Object.equals` is deep (SameValue leaf).
- **Date** — English/ISO, **local-time**, immutable.
- **Security** — iteration builders cap element count at `1e8` and throw `RangeError` *before*
  allocating; `pad`/`hex` cap width at 65536; `Function.until` caps at `1e7` iterations;
  `String.removeTags` is single-pass O(n).
- **⚡ SIMD** — the marker flags methods that dispatch to `simd.*` byte/vector kernels (scalar →
  NEON/SSE4.2/AVX2/AVX‑512). SIMD pays only on long spans (64-byte gate); short inputs run a scalar
  oracle. The complete SIMD-accelerated set is: `%TypedArray%` `sum min max mean average dot`; `String`
  `compact count escapeHTML unescapeHTML stripTags lines encodeBase64 decodeBase64`. Everything else is
  scalar (the generic `Array` cannot SIMD strided tagged values).

## Array.prototype

**Aggregate & query** — a `match` is a value, a predicate `fn`, or a `RegExp`; a `map` is a mapper `fn`,
a property-key string, or omitted (identity).

| Method | Signature | Description |
|---|---|---|
| `sum` / `average` / `mean` | `() → number` | Σ / arithmetic mean (mean of empty = `NaN`). |
| `median` / `product` | `() → number` | Middle value (coerced to a C buffer first) / Π. |
| `min` / `max` | `(map?) → element` | Extremum by optional mapper. |
| `count` / `none` / `any` / `all` | `(match) → number \| boolean` | Count / quantifiers over the matcher. |
| `countBy` / `indexBy` | `(fn) → object` | `{key: count}` / `{key: lastElement}` grouped by `fn(el)`. |
| `scan` | `(fn, acc) → array` | Running reduce (all intermediate accumulators). |
| `reduceBy` | `(valueFn, acc, keyFn) → object` | Group by `keyFn(el)`, fold each group from a fresh (shallow-cloned) `acc`. |
| `startsWith` / `endsWith` | `(sublist) → boolean` | Deep-equal prefix / suffix test. |

**Access & window**

| Method | Signature | Description |
|---|---|---|
| `first` / `last` / `head` | `() → element` | Ends (`head` = `first`). |
| `nth` | `(i) → element` | `i`-th element (negative from the end). |
| `init` / `tail` | `() → array` | All but last / all but first. |
| `take` / `drop` / `takeLast` / `dropLast` | `(n) → array` | Fixed-count slices. |
| `takeWhile` / `dropWhile` / `takeLastWhile` / `dropLastWhile` | `(match) → array` | Matcher-bounded slices. |

**Transform & structure**

| Method | Signature | Description |
|---|---|---|
| `unique` / `uniq` / `uniqBy` | `(map?) → array` | Dedup by SameValueZero (or by `fn(el)`). |
| `dropRepeats` / `dropRepeatsWith` / `dropRepeatsBy` | `(pred? / fn?) → array` | Drop *consecutive* duplicates (deep-equal; or `pred(prev,cur)`; or by `fn(el)`). |
| `compact` | `() → array` | Drop falsey (`null`/`undefined`/`NaN`/`false`/empty) elements. |
| `flatten` / `unnest` / `transpose` | `() → array` | Deep flatten / flatten one level / matrix transpose. |
| `sortWith` | `(comparators) → array` | Stable sort by a list of comparators — first non-zero wins. |
| `intersperse` | `(sep) → array` | Insert `sep` between elements. |
| `aperture` | `(n) → array` | Sliding windows of width `n`. |
| `splitEvery` / `splitAt` / `splitWhen` | `(n \| i \| match) → array` | Chunk / split at index / split at first match. |
| `shuffle` / `sample` | `() → array \| element` | Fisher–Yates / random element. |
| `sortBy` / `groupBy` / `partition` | `(map \| fn \| match) → …` | Stable sort / `{key:[…]}` / `[pass, fail]`. |
| `pluck` | `(key) → array` | `el[key]` for each. |
| `adjust` / `update` | `(i, fn) / (i, v) → array` | Non-mutating element edit. |
| `move` / `swap` | `(from, to) / (i, j) → array` | Non-mutating reorder. |
| `zip` / `zipWith` / `zipObj` / `fromPairs` | `(b) / (fn,b) / (vals) / () → …` | Pair, combine, or build objects. |
| `xprod` / `innerJoin` | `(b) / (pred, b) → array` | Cartesian product / relational join. |

**Build & combine**

| Method | Signature | Description |
|---|---|---|
| `append` / `prepend` | `(x) → array` | Add one element at an end. |
| `insert` / `insertAll` | `(i, x) / (i, xs) → array` | Splice-insert (out-of-range appends). |
| `removeAt` / `removeRange` | `(i) / (start, count) → array` | Non-mutating removal. |
| `remove` / `reject` | `(match) → array` | Drop matching elements (aliases). |
| `union` / `intersect` / `intersection` / `difference` / `without` | `(b) → array` | Set ops (SameValueZero). |
| `unionWith` / `differenceWith` | `(pred, b) → array` | Same, but equality is your `pred(a,b)`. |
| `symmetricDifference` / `symmetricDifferenceWith` | `(b) / (pred, b) → array` | Elements in either but not both. |
| `Array.repeat` *(static)* | `(x, n) → array` | `n` copies of `x` (the same reference); `RangeError` past `1e8`. |

```js
[1,2,3,4].sum();                 // 10
[5,3,1,4,2].sortBy();            // [1,2,3,4,5]
["a","b","c"].intersperse("-");  // ["a","-","b","-","c"]
[{n:1},{n:2}].pluck("n");        // [1, 2]
[1,2,3].union([3,4,5]);          // [1,2,3,4,5]
[1,1,2,3,3].dropRepeats();       // [1,2,3]  (only *adjacent* dupes)
[1,2,3].symmetricDifference([2,3,4]);   // [1,4]
```

## Iterator.prototype  (lazy pipelines)

The full ECMAScript iterator-helper surface, so a pipeline can run without materialising the
intermediate arrays that `Array.prototype.map().filter()` builds.

| Member | Signature | Description |
|---|---|---|
| `map` / `filter` / `flatMap` | `(fn) → Iterator` | Lazy transforms. |
| `take` / `drop` | `(n) → Iterator` | Lazy slices. |
| `every` / `some` / `find` / `forEach` | `(fn) → …` | Terminal; close the iterator on early exit. |
| `includes` | `(value) → boolean` | Terminal. **SameValueZero**, so `NaN` finds `NaN` and `±0` match. Closes the iterator on a hit. |
| `reduce` / `toArray` | `(…) → …` | Terminal. |
| `[Symbol.dispose]()` | `() → undefined` | `IteratorClose`, so an iterator works with `using`. A no-op when there is no `return` method. |
| `Iterator.from(x)` *(static)* | `(iterable) → Iterator` | Wrap anything iterable. |
| `Iterator.concat(...its)` *(static)* | `(…iterables) → Iterator` | Sequential concatenation. |
| `Iterator.zip(iterables, options?)` *(static)* | `(iterable, {mode?, padding?}) → Iterator` | Step inputs in lockstep, yielding an array per round. |
| `Iterator.zipKeyed(obj, options?)` *(static)* | `(object, {mode?, padding?}) → Iterator` | As `zip` over an object's own enumerable keys, yielding an object per round. |

**`zip` modes.** `"shortest"` (default) stops at the first exhausted input **and closes the rest** —
which is what keeps zipping an infinite generator against a finite list from hanging or leaking.
`"longest"` runs until all are spent, substituting `padding[i]` (or `undefined`) for finished inputs.
`"strict"` **throws** `TypeError` unless every input finishes on the same round.

Options are read once, at construction. A string is rejected both as the input list and as an
individual input — use `Iterator.from(str)` to be explicit.

```js
Iterator.zip([[1,2,3,4], ["a","b","c"]]).toArray();
// [[1,"a"], [2,"b"], [3,"c"]]
Iterator.zip([[1,2,3,4], ["a","b","c"]], { mode: "longest", padding: ["-","-"] }).toArray();
// [[1,"a"], [2,"b"], [3,"c"], [4,"-"]]
Iterator.zipKeyed({ id: [1,2], name: ["a","b"] }).toArray();
// [{id:1, name:"a"}, {id:2, name:"b"}]
```

### The lazy tier

`Array.prototype.lazy()` and `String.prototype.lazy()` (code points) are the entry points: they
return an iterator, so the whole surface below composes with the ECMAScript helpers above.

A method is on `Iterator.prototype` **iff it is single-pass and O(1) in additional state.** Each one
answers exactly what the `Array.prototype` method of the same name answers — the eager form is the
authority, and `tests/test_iterator_lazy.js` pins the two equal over a corpus.

| Intermediate (→ `Iterator`) | Signature | Description |
|---|---|---|
| `takeWhile` / `dropWhile` | `(match) → Iterator` | `match` is a value, a `RegExp` or a predicate, resolved once at construction. `takeWhile` closes the source at the first rejection. |
| `scan` | `(fn, initial) → Iterator` | Running accumulation, `initial` first. |
| `intersperse` | `(sep) → Iterator` | `sep` between each pair. |
| `compact` | `() → Iterator` | Drops `null` and `undefined`. |
| `dropRepeats` / `dropRepeatsWith` / `dropRepeatsBy` | `(…) → Iterator` | Collapses runs of equal neighbours: deep equality, a comparator, or a mapped key. |
| `aperture` | `(n) → Iterator` | Sliding windows of `n`, as fresh arrays. `n ≥ 1`. |
| `splitEvery` | `(n) → Iterator` | Consecutive chunks of `n`; the last may be short. `n ≥ 1`. |
| `zipWith` | `(fn, other) → Iterator` | Pairs with another iterable, `fn(a, b)`, ending — and closing both — at the shorter. |
| `pluck` | `(key) → Iterator` | `element[key]`. |
| `init` / `tail` | `() → Iterator` | All but the last (one element of lookahead) / all but the first. |
| `unique` / `uniq` / `uniqBy` | `(map?) → Iterator` | First occurrence wins, SameValueZero on the mapped value. |
| `tee` | `(n = 2) → Iterator[]` | `n` independent iterators over one source. |

| Terminal | Signature | Description |
|---|---|---|
| `sum` / `average` / `mean` / `product` | `() → number` | |
| `min` / `max` | `(map?) → any` | The **element** whose mapped value is smallest / largest, first on a tie. |
| `none` / `any` / `all` | `(match) → boolean` | Close the source as soon as the answer is known. |
| `count` | `(match?) → number` | Every element, or the matching ones. |
| `first` / `head` / `nth` / `findIndex` | `(…) → any` | Close the source on the hit. `nth` takes a non-negative index. |
| `last` | `() → any` | |
| `countBy` / `indexBy` / `groupBy` | `(map?) → object` | |
| `reduceBy` | `(fn, seed, keyFn) → object` | |

Two of these hold state proportional to the input rather than to the configuration, and it is worth
knowing which: **`unique` holds one entry per distinct value**, and **`tee` buffers the lag between
its fastest and slowest branch** — draining one branch fully before touching another buffers the
whole source. `aperture` and `splitEvery` hold one window, whose size is configuration.

An operation that is inherently buffering is **not** here: `sort`, `sortBy`, `reverse`, `flatten`,
`transpose`, `shuffle`, `median`, `union`, `intersect`, `difference`, `partition`, `splitAt`,
`takeLast`, `dropLast`. Call `.toArray()` and use the `Array.prototype` form.

**Laziness is the point.** `src.lazy().map(f).take(3).toArray()` pulls exactly 3 elements and calls
`f` exactly 3 times, where the eager `filter().slice()` would walk the whole source first — so the
lazy form wins by however much of the input it skips, and the eager form catches up once `take`
reaches a large fraction of it. Against that, a **full traversal with no early exit costs more**:
nothing is bypassed, so the per-element helper machinery is paid for nothing. Both cases are in
`tests/bench_lazy.js`, and the losing one stays there.

## Array.prototype  (transducers)

The standard transducer protocol — a *transformer* is any object with
`@@transducer/init`, `@@transducer/step`, `@@transducer/result`, and early-exit is the
`{ "@@transducer/reduced": true, "@@transducer/value": v }` wrapper. Hand-write one, or bring any
transformer that follows the protocol; the engine drives it.

| Method | Signature | Description |
|---|---|---|
| `transduce` | `(xf, fn, acc) → any` | Reduce through transducer `xf`, seeded with `acc`; `fn` is a `(acc,x)` reducer or a transformer. |
| `into` | `(acc, xf) → any` | Transduce into a **fresh** container chosen by `acc`'s type — array (push), string (concat) or object (assign). |
| `sequence` | `(Applicative) → any` | Transpose an array of applicatives. For the `Array` applicative this is the **cartesian product**. |
| `traverse` | `(Applicative, fn) → any` | `sequence` of `map(fn, this)`. |

```js
const double = xf => ({ "@@transducer/init":   () => xf["@@transducer/init"](),
                        "@@transducer/result": a  => xf["@@transducer/result"](a),
                        "@@transducer/step":  (a,x)=> xf["@@transducer/step"](a, x*2) });
[1,2,3,4].into([], double);              // [2,4,6,8]  — no intermediate array
[[1,2],[3,4]].sequence(Array);           // [[1,3],[1,4],[2,3],[2,4]]
```

`sequence`/`traverse` take **any applicative**, not only `Array`. The type supplies `of`, `map` and
`ap`, under either naming convention: `fantasy-land/of` · `fantasy-land/map` · `fantasy-land/ap`
(where `ap` is on the **value**, `xs.ap(fs)`), or plain `of` · `map` · `ap` (where `ap` is on the
**functions**, `fs.ap(xs)`). The fantasy-land name is tried first, so a type carrying both is
unambiguous.

<!-- check:skip -->
```js
[Just(1), Just(2), Just(3)].sequence(Just);        // Just([1, 2, 3])
[Just(1), Nothing(), Just(3)].sequence(Just);      // Nothing — one failure fails the traversal
[1, 2, 3].traverse(Just, x => Just(x * 2));        // Just([2, 4, 6])
```

`Array` keeps its own path — for a list, `map` and `ap` are two nested loops, and going through the
methods would allocate a closure per element to say what the loop already says. The two are told
apart once, by whether `of([])` carries an `ap`. A type with a `map` but no `ap` is **neither**
applicative and throws `TypeError`, rather than falling through to the list fold and answering `[]`.

## Array.prototype  (`*FromIndex`)

Every one is the matching Array method, but **starting at `startIndex`** (negative = from the end) and,
when the optional `loop` boolean is passed, **wrapping around to the front**. Callbacks always receive the
*original* element and its de-shifted original index.

| Method | Signature |
|---|---|
| `mapFromIndex` `forEachFromIndex` `filterFromIndex` | `(startIndex, loop?, fn, thisArg?)` |
| `findFromIndex` `findIndexFromIndex` `someFromIndex` `everyFromIndex` | `(startIndex, loop?, match, thisArg?)` |
| `reduceFromIndex` `reduceRightFromIndex` | `(startIndex, loop?, reducer, initial?)` |

```js
["a","b","c","d","e"].mapFromIndex(2, x => x);          // ["c","d","e"]
["a","b","c","d","e"].mapFromIndex(2, true, x => x);    // ["c","d","e","a","b"]  (wrapped)
[10,20,30,40].filterFromIndex(1, x => x > 15);          // [20,30,40]
```

> Two quirks the test suite pins, so treat them as part of the contract: a **falsy**
> `reduceFromIndex` seed is dropped (the first element seeds instead), and `reduceRightFromIndex`
> reports shifted indices in the non-loop case.

## %TypedArray%.prototype  (SIMD reductions)

| Method | Signature | Description |
|---|---|---|
| `sum` ⚡ | `() → number` | SIMD Σ (f64/f32/i32 kernel by array type). |
| `min` / `max` ⚡ | `() → number` | SIMD horizontal min/max. |
| `mean` / `average` ⚡ | `() → number` | SIMD sum ÷ length. |
| `dot` ⚡ | `(other) → number` | SIMD dot product with another same-type TypedArray. |

```js
new Float64Array([1,2,3,4]).sum();                          // 10
new Float32Array([1,2,3]).dot(new Float32Array([4,5,6]));   // 32
```

## String.prototype

**Predicates & slice**

| Method | Signature | Description |
|---|---|---|
| `isEmpty` / `isBlank` | `() → boolean` | Length 0 / empty-or-whitespace. |
| `first` / `last` | `(n=1) → string` | Leading / trailing `n` chars. |
| `from` / `to` | `(i) → string` | Substring from / up to index (negative from end). |

**Split & scan**

| Method | Signature | Description |
|---|---|---|
| `chars` / `codes` | `() → array` | Code-point strings / char codes (pre-sized fast array). |
| `words` | `() → string[]` | Whitespace-delimited words. |
| `lines` ⚡ | `() → string[]` | Split on `\n` (`simd.count_u8` presize + `simd.find_u8`). |
| `count` ⚡ | `(sub) → number` | Non-overlapping occurrences (`simd.count_u8` for a 1-char needle). |
| `forEach` | `(fn) → string[]` | `fn(char, i)` per code point; returns the char array. |

**Transform**

| Method | Signature | Description |
|---|---|---|
| `reverse` | `() → string` | Reversed (direct alloc + tight copy; auto-vectorized). |
| `compact` ⚡ | `() → string` | Collapse whitespace runs → single space, trim (`simd.find_first_of`). |
| `insert` | `(str, i=end) → string` | Insert at a code-unit index. |
| `remove` / `removeAll` | `(m) → string` | Delete first / all matches (string or `RegExp`). |
| `shift` | `(n) → string` | Caesar-shift each char code by `n`. |
| `truncate` / `truncateOnWord` | `(len, from='right', ellipsis='…') → string` | Clip with ellipsis. |
| `pad` | `(n, char=' ') → string` | Pad both sides to width `n`. |

**Case & inflection**

| Method | Signature | Description |
|---|---|---|
| `capitalize` | `(all=false, downcaseRest=false) → string` | Capitalize first (or each) word. |
| `camelize` / `underscore` / `dasherize` / `spacify` | `(upperFirst=true?) → string` | camelCase / snake / kebab / spaced. |
| `titleize` | `() → string` | Title Case with a lowercase stop-word list. |
| `humanize` | `() → string` | `user_name_id` → `"User name"`. |
| `parameterize` | `() → string` | Lowercase URL slug (`-`; ASCII, no accent transliteration). |
| `pluralize` / `singularize` | `() → string` | English rules + small irregular/uncountable table. |

**HTML / URL / Base64**

| Method | Signature | Description |
|---|---|---|
| `escapeHTML` ⚡ | `() → string` | Escape `& < >` (`simd.find_first_of`). |
| `unescapeHTML` ⚡ | `() → string` | Decode named + numeric entities (`simd.find_u8`). |
| `stripTags` ⚡ | `() → string` | Remove `<…>` tags, keep text (`simd.find_u8`). |
| `removeTags` | `(tagName?) → string` | Remove element(s) **and** content; single-pass **O(n)**. |
| `escapeURL` / `unescapeURL` | `(all=false? / partial=false?) → string` | Percent encode / decode. |
| `encodeBase64` ⚡ | `() → string` | `simd.base64_encode`. |
| `decodeBase64` ⚡ | `() → string` | `simd.base64_decode`. |

**Terminal text: ANSI, width, clustering**

One CSI/OSC grammar backs all four; the width tables are Unicode 15.1
East_Asian_Width plus the zero-advance categories `Mn`/`Me`/`Cf`.

| Method | Signature | Description |
|---|---|---|
| `stripAnsi` ⚡ | `() → string` | Remove CSI/OSC/DCS escape sequences (`simd.find_u8`). A lone `ESC` is not a sequence and is kept. |
| `displayWidth` | `({ambiguousAsWide=false}?) → number` | Terminal cells. Escapes, controls, combining marks and format characters count 0; East Asian W/F and emoji count 2. |
| `wrapAnsi` | `(columns, {hard=false, trim=true}?) → string` | Wrap to `columns` cells, breaking at whitespace; active SGR state is re-emitted after each break. |
| `graphemes` | `() → string[]` | Extended grapheme clusters. Emoji ZWJ sequences, flags and skin-tone modifiers are one cluster each. |

`displayWidth` counts a grapheme cluster, not a code point: an emoji ZWJ
sequence is 2 cells, not the sum of its parts. `ambiguousAsWide` is **off** by
default — a terminal renders East_Asian_Width `A` at 1 unless configured
otherwise. A word wider than `columns` overflows unless `hard: true`.

**Convert**

| Method | Signature | Description |
|---|---|---|
| `toNumber` | `(base=10) → number` | Lenient parse (`strtod` / `strtoll`); `NaN` on failure. |
| `format` | `(...args) → string` | `{0}`/`{name}` template (`{{`/`}}` literal braces). |

```js
"  many   spaces  ".compact();       // "many spaces"
"banana".count("a");                 // 3
"<b>hi</b>".stripTags();             // "hi"
"hello_world".titleize();            // "Hello World"
"person".pluralize();                // "people"
"{0} + {1}".format(2, 3);            // "2 + 3"
const red = "\u001b[31m" + "red" + "\u001b[0m";
red.stripAnsi();                     // "red"
red.displayWidth();                  // 3   (escapes are invisible)
"\u4f60\u597d".displayWidth();       // 4   (two wide characters)
"\u{1f468}\u200d\u{1f469}\u200d\u{1f467}".displayWidth();  // 2   (one glyph, not 6)
"hello world".wrapAnsi(5);           // "hello\nworld"
"e\u0301x".graphemes();              // ["e\u0301", "x"]  -- 2 clusters, 3 code units
```

## Number.prototype

**Arithmetic & relational**

| Method | Signature | Description |
|---|---|---|
| `negate` / `inc` / `dec` | `() → number` | −x / x+1 / x−1. |
| `abs` `sqrt` `exp` `sin` `cos` `tan` `asin` `acos` `atan` | `() → number` | libm delegation. |
| `add` `subtract` `multiply` `divide` `modulo` `pow` | `(n) → number` | `modulo` == `fmod` == JS `%`. |
| `gt` / `gte` / `lt` / `lte` | `(n) → boolean` | Relational (NaN → false). |

**Predicates & math**

| Method | Signature | Description |
|---|---|---|
| `isInteger` / `isOdd` / `isEven` | `() → boolean` | Integer tests. |
| `isMultipleOf` | `(n) → boolean` | `this % n === 0`. |
| `mathMod` | `(n) → number` | Non-negative modulus; `NaN` unless both integer and `n ≥ 1`. |
| `clamp` | `(min, max) → number` | Clamp into range. |
| `log` | `(base=e) → number` | Change-of-base logarithm. |
| `round` / `ceil` / `floor` | `(places=0) → number` | Precision rounding (negative places → tens/hundreds). |
| `chr` | `() → string` | The char for this char code. |

**Formatting**

| Method | Signature | Description |
|---|---|---|
| `pad` | `(place, sign=false, base=10) → string` | Zero-pad the integer part. |
| `hex` | `(place=1) → string` | Hex, zero-padded. |
| `format` | `(place=0, thousands=',', decimal='.') → string` | Grouped thousands. |
| `abbr` / `metric` / `bytes` | `(precision=0) → string` | `2k` (÷1000 k/m/b/t) / SI / byte size (÷1024). |
| `ordinalize` | `() → string` | `1`→`"1st"`, `11`→`"11th"`. |
| `duration` | `() → string` | Treat `this` as ms → `"2 hours"`. |

**Iteration** — build arrays; element count capped at `1e8` (throws `RangeError` before allocating).

| Method | Signature | Description |
|---|---|---|
| `times` | `(fn?) → array` | `[fn(0)…fn(n-1)]` (or `[0…n-1]`). |
| `upto` / `downto` | `(end, step=1, fn?) → array` | Inclusive numeric range. |
| `Number.range` *(static)* | `(start, end, step=1) → array` | End-**exclusive** numeric range (unlike `upto`/`downto`, which are inclusive). |

```js
(3.14159).round(2);       // 3.14
(1536).bytes(1);          // "1.5KB"
(1234567).format();       // "1,234,567"
(3).times(i => i*i);      // [0, 1, 4]
Number.range(0, 5);       // [0, 1, 2, 3, 4]
```

## Object  (static on the `Object` constructor)

**Type guards & nil** — `(v) → boolean` unless noted (class-id based; wrapper objects report the
primitive tag).

| Method | Description |
|---|---|
| `isObject isArray isBoolean isNumber isString isFunction isDate isRegExp isError isSet isMap isArguments` | Type tests. |
| `isNil` / `isNotNil` | `== null` / not. |
| `type(v) → string` | `"Number"`, `"Array"`, `"Null"`, … (class-id based). |
| `defaultTo(d, v) → any` | `v` unless `null`/`undefined`/`NaN`, then `d`. |
| `propIs(Ctor, name, obj) → boolean` | `obj[name]` is an instance of `Ctor` (incl. primitives). |

**Query** — `path` is a dotted string or an array.

| Method | Signature | Description |
|---|---|---|
| `size` / `isEmpty` | `(o) → number \| boolean` | Count of own enumerable string keys. |
| `keysIn` / `valuesIn` | `(o) → array` | Enumerable keys/values **including inherited**. |
| `toPairs` / `fromPairs` | `(o) / (pairs) → …` | `[[k,v]]` ⇄ object. |
| `has` / `hasIn` / `hasPath` | `(k,o) / (k,o) / (path,o) → boolean` | Own / `in` / deep-own presence. |
| `prop` / `propOr` / `props` | `(k,o) / (d,k,o) / (keys,o) → …` | Read one / with default / many. |
| `path` / `pathOr` / `paths` | `(path,o) / (d,path,o) / (list,o) → …` | Deep read. |
| `get` | `(o, path, default?) → any` | Deep read taking the **object first** (`path`/`pathOr` take the path first). |

**Predicates**

| Method | Signature | Description |
|---|---|---|
| `equals` / `identical` | `(a, b) → boolean` | Deep structural (SameValue leaf; cycles throw) / SameValue. |
| `propEq` / `pathEq` / `eqProps` | `(val,k,o) / (val,path,o) / (k,a,b) → boolean` | Focused equality. |
| `propSatisfies` / `pathSatisfies` | `(pred, k\|path, o) → boolean` | Focused predicate. |
| `where` / `whereEq` / `whereAny` | `(spec, o) → boolean` | All preds / deep-eq per key / any pred. |

**Build & transform** (immutable unless noted)

| Method | Signature | Description |
|---|---|---|
| `clone` | `(o) → any` | Deep clone (arrays, plain objects, Date, RegExp; other exotics by ref). |
| `pick` / `pickAll` / `omit` / `pickBy` | `(keys\|pred, o) → object` | Select / with missing / drop / by predicate. |
| `project` | `(keys, arr) → array` | `arr.map(pick(keys))`. |
| `assoc` / `dissoc` | `(k, v, o) / (k, o) → object` | Shallow set / delete. |
| `assocPath` / `dissocPath` | `(path, v, o) / (path, o) → object` | Immutable deep set / delete. |
| `set` | `(o, path, v) → object` | **Mutates** `o` (deep, creates intermediates); returns `o`. |
| `modify` / `modifyPath` | `(k\|path, fn, o) → object` | Apply `fn` to a focus. |
| `evolve` | `(transforms, o) → object` | Per-key transform (fn or nested transforms). |
| `mapObjIndexed` / `forEachObjIndexed` | `(fn, o) → object` | `{k: fn(v,k,o)}` / side effect returns `o`. |
| `mapKeys` / `renameKeys` | `(fn, o) / (map, o) → object` | Transform / rename keys. |
| `invert` / `invertObj` / `objOf` | `(o) / (o) / (k,v) → object` | Swap keys↔values / `{[k]:v}`. |
| `tap` | `(fn, x) → x` | Run `fn(x)` for effect, return `x`. |
| `defaults` | `(o, source) → object` | `o` wins, `source` fills gaps. |
| `merge` / `mergeRight` / `mergeLeft` | `(a, b) → object` | Shallow (left key order; right/left wins). |
| `mergeDeepRight` / `mergeDeepLeft` | `(a, b) → object` | Recursive merge. |
| `mergeWith` / `mergeWithKey` | `(fn, a, b) → object` | Resolve conflicts with `fn`. |

```js
Object.pick(["a","c"], {a:1,b:2,c:3});          // {a:1, c:3}
Object.path("a.b.c", {a:{b:{c:42}}});           // 42
Object.mergeDeepRight({a:{x:1}}, {a:{y:2}});     // {a:{x:1, y:2}}
Object.evolve({n: x=>x*2}, {n:5, s:"k"});        // {n:10, s:"k"}
Object.equals({a:[1,2]}, {a:[1,2]});             // true
```

## Function.prototype  (combinators)

Each returns a **new function** capturing the receiver + args; `this` is the receiving function.
No `R.__` placeholders. Composing beyond ~253 functions throws `RangeError`.

| Method | Signature | Description |
|---|---|---|
| `pipe` / `compose` / `flow` | `(...fns) → fn` | L→R / R→L composition (`flow` = `pipe`). |
| `o` / `on` | `(g) → fn` | `x=>f(g(x))` / `(a,b)=>f(g(a),g(b))`. |
| `both` / `allPass` | `(...preds) → fn` | Logical AND (short-circuit). |
| `either` / `anyPass` / `complement` | `(...preds) / () → fn` | OR / NOT. |
| `juxt` | `(...fns) → fn` | `x => [f(x), g(x), …]`. |
| `converge` | `(...branches) → fn` | `(...a) => f(b0(...a), b1(...a), …)`. |
| `useWith` | `(...ts) → fn` | `(a,b,…) => f(t0(a), t1(b), …)`. |
| `flip` / `unary` / `binary` / `nAry` | `() / () / () / (n) → fn` | Swap first two args / fix arity. |
| `once` / `thunkify` | `() → fn` | Memoize first result / `(...a)()` ⇒ `f(...a)`. |
| `partial` / `partialRight` | `(...a) → fn` | Pre-bind leading / trailing args. |
| `curry` / `curryN` | `() / (n) → fn` | Auto-curry (reusable, no arg bleed). |
| `ifElse` / `when` / `unless` | `(t,f) / (fn) / (fn) → fn` | Conditional (`this` is the predicate). |
| `until` | `(fn) → fn` | `x => apply fn until this(x)` (≤ 1e7 iterations). |
| `tryCatch` | `(handler) → fn` | `(...a) => try f(...a) catch (e) handler(e, ...a)`. |
| `unapply` / `comparator` | `() → fn` | `(...a)=>f(a)` / predicate → sort comparator. |

```js
const f = (x=>x+1).pipe(x=>x*2, x=>-x);   f(3);     // -8
const add3 = ((a,b,c)=>a+b+c).curry();    add3(1)(2)(3);   // 6
const safe = (x=>{ if(x<0) throw Error("neg"); return x }).tryCatch(()=>0);   safe(-1);  // 0
```

### Rate limiting and caching

| Method | Signature | Description |
|---|---|---|
| `debounce(ms)` | `(number) → fn` | Runs only the **last** call of a burst, `ms` after it. |
| `throttle(ms)` | `(number) → fn` | Runs the **first** call at once and the last of the burst at the trailing edge. |
| `delay(ms, ...args)` | `(number, ...any) → {cancel}` | Schedules **one** call and returns a handle. |
| `memoize(keyFn?)` | `(fn?) → fn` | Caches results in a `Map`. |

`debounce` and `throttle` return a wrapper carrying **`.cancel()`** — forget the pending call — and
**`.flush()`** — run it now and return its result. `delay` returns a handle rather than a function,
because its call is already committed; the two are impossible to confuse. `ms` must be a
non-negative number, and anything else is a `RangeError` rather than a silent zero.

They schedule through `globalThis.setTimeout`, looked up **when called**, not when wrapped — so an
embedding with no event loop can still build a debounced function, and the error when there is no
scheduler says so.

`memoize` keys on a real `Map`, so keys compare by SameValueZero: `1` and `"1"` are different, `NaN`
is one key, and objects key by identity. The **default key is the first argument**; calling a
default-keyed memo with more than one argument is a `TypeError` naming `memoize(fn)`, because any
one-argument key for a two-argument call is a silent collision. A call that **throws is not cached** —
the next call gets to try again.

```js
const save = ((doc) => print("wrote", doc)).debounce(300);
save("a"); save("b"); save("c");  // writes "c", once, 300 ms later
save.cancel();                    // ...or not

const scroll = (() => paint()).throttle(16);   // leading + trailing, ~60fps
const fib = (n => n < 2 ? n : fib(n-1) + fib(n-2)).memoize();
const dist = ((a, b) => hypot(a, b)).memoize((a, b) => a + "|" + b);
```

There is no `lazy`: a lazily-initialised value is `(() => expensive()).once()`, which already
computes at most once and returns the same result forever.

## Function  (static combinators on the constructor)

The point-free “glue” that has no natural receiver — it lives on the `Function` constructor itself.

| Method | Signature | Description |
|---|---|---|
| `Function.identity` | `(x) → x` | Returns its argument. |
| `Function.always` | `(x) → fn` | A function that always returns `x`. |
| `Function.of` | `(x) → [x]` | Singleton array. |
| `Function.not` / `Function.negate` | `(x) → boolean` / `(n) → number` | Logical `!x` / arithmetic `-n`. |
| `Function.applyTo` | `(x, f) → f(x)` | Feed `x` to `f`. |
| `Function.cond` | `([[pred, transform], …]) → fn` | Run the first matching pair’s transform (else `undefined`). |
| `Function.uncurryN` | `(depth, fn) → fn` | Collapse `depth` curried calls into one. |
| `Function.lift` / `Function.liftN` | `(fn) / (arity, fn) → fn` | Lift `fn` over lists — the cartesian product of its arguments. |
| `Function.ap` | `(fns, xs) → array` | `[f(x) …]`; or the S-combinator `x => f(x)(g(x))` when `fns` is a function. |

```js
Function.cond([[x=>x<0,()=>"neg"], [x=>x>0,()=>"pos"], [()=>true,()=>"zero"]])(-5);  // "neg"
Function.lift((a,b)=>a+b)([1,2],[10,20]);   // [11,21,12,22]
```

## Date.prototype  (English/ISO, local-time, immutable)

**Predicates** — `() → boolean`

| Group | Methods |
|---|---|
| State | `isValid isToday isYesterday isTomorrow isFuture isPast isWeekday isWeekend isLeapYear` |
| Day-of-week | `isSunday isMonday isTuesday isWednesday isThursday isFriday isSaturday` |
| Month | `isJanuary … isDecember` |

**Query & compare**

| Method | Signature | Description |
|---|---|---|
| `getWeekday` | `() → 0–6` | Day of week (Sun–Sat). |
| `getISOWeek` | `() → 1–53` | ISO-8601 week number. |
| `daysInMonth` | `() → number` | Days in this month. |
| `isBefore` / `isAfter` | `(d) → boolean` | Ordering. |
| `isBetween` | `(a, b) → boolean` | Inclusive; bounds auto-ordered. |

**Diffs** — whole units (`trunc`); invalid date → `NaN`. `<unit> ∈ {milliseconds, seconds, minutes,
hours, days, weeks, months, years}` (months/years use calendar math).

| Method | Signature | Description |
|---|---|---|
| `<unit>Since` / `<unit>Until` | `(d) → number` | From `d` to this / this to `d`. |
| `<unit>Ago` / `<unit>FromNow` | `() → number` | From this to now / now to this. |

**Produce** — `→ Date` (immutable; JS field-overflow / MakeDay)

| Method | Signature | Description |
|---|---|---|
| `addMilliseconds … addYears` | `(n) → Date` | Add a signed amount of a unit. |
| `beginningOfDay` / `endOfDay` | `() → Date` | 00:00 / 23:59:59.999. |
| `beginningOfWeek` / `endOfWeek` | `() → Date` | Sunday 00:00 / Saturday 23:59:59.999. |
| `beginningOfMonth` / `endOfMonth` | `() → Date` | First / last day. |
| `beginningOfYear` / `endOfYear` | `() → Date` | Jan 1 / Dec 31. |
| `advance` / `rewind` | `(spec) → Date` | Apply/subtract `{years,months,weeks,days,hours,minutes,seconds,milliseconds}`. |
| `clone` | `() → Date` | Copy. |

**Format** — `→ string`

| Method | Signature | Description |
|---|---|---|
| `iso` | `() → string` | `toISOString` alias. |
| `format` | `(mask?) → string` | `{token}` substitution; no mask → `"yyyy-MM-dd HH:mm:ss"`. |
| `relative` | `() → string` | `"2 days ago"` / `"in 3 days"` / `"just now"`. |

`format` tokens: `yyyy yy MM M dd d HH H hh h mm m ss s SSS Mon Month dow Weekday tt TT`.

```js
const d = new Date(2024, 1, 29, 15, 30);
d.isLeapYear();                              // true
d.addDays(1).getMonth();                     // 2  (Mar 1)
d.endOfMonth().getDate();                    // 29
d.format("{Weekday}, {Month} {d}, {yyyy}");  // "Thursday, February 29, 2024"
new Date(2024,0,1).daysUntil(new Date(2024,0,11));   // 10
```

## Lens  (the `Lens` global)

`Lens` is a **constructor function**: a lens is an ordinary object whose prototype is
`Lens.prototype` (non-enumerable config, no new intrinsic class), so `lens instanceof Lens` holds.
`set`/`over` are immutable and preserve container type (arrays stay arrays).

| Method | Signature | Description |
|---|---|---|
| `Lens(getter, setter)` / `new Lens(...)` | `(getter, setter) → Lens` | Custom lens (same as `Lens.lens`); callable with or without `new`. |
| `Lens.prop` / `Lens.index` | `(k) / (i) → Lens` | Focus a property / array index. |
| `Lens.path` | `(p) → Lens` | Focus a deep path (dotted string or array). |
| `Lens.lens` | `(getter, setter) → Lens` | Custom; `getter(obj)`, `setter(newVal, obj)`. |
| `Lens.view` / `lens.view` | `(lens, o) / (o) → any` | Read the focus. |
| `Lens.set` / `lens.set` | `(lens, v, o) / (v, o) → any` | Immutable set (new container). |
| `Lens.over` / `lens.over` | `(lens, fn, o) / (fn, o) → any` | Immutable `fn`-modify. |

Obeys the lens laws: `view(set(v,s)) ≡ v`, `set(view(s),s) ≡ s`, `set(v, set(_,s)) ≡ set(v,s)`.

```js
const nameL = Lens.prop("name");
nameL.view({name:"ada"});                 // "ada"
nameL.set("bob", {name:"ada", age:1});    // {name:"bob", age:1}   (original unchanged)
Lens.over(Lens.index(1), x=>x*10, [1,2,3]);   // [1, 20, 3]
```

## TLS

TLS needs `CONFIG_TLS=y` (links OpenSSL >= 3.0). Without it, every `tls:` option
throws by name rather than connecting in the clear.

<!-- check:skip -->  <!-- fragment: `handlers` is defined by the surrounding prose -->
```js
// client: the hostname drives BOTH SNI and certificate verification
TCPServer.connect({ host: "example.com", port: 443, tls: true }, handlers);
TCPServer.connect({ host, port: 443,
                    tls: { servername, alpn: ["http/1.1"], ca, minVersion: "1.3",
                           rejectUnauthorized: false } }, handlers);

// server: cert and key are both REQUIRED PEM paths; there is no self-signed default
new TCPServer({ port, tls: { cert: "/p/cert.pem", key: "/p/key.pem" } });

new HTTPClient().get("https://example.com/");   // https works from the same client
```

`connect` fires only after the handshake. A failed handshake reports through the
same handler with the error naming the check that failed (`certificate has
expired`, `hostname mismatch`); `close` does not fire, because no usable
connection existed. Verification defaults to full — chain, hostname and expiry.

## dyna:crypto — ciphers and signatures

Needs `CONFIG_TLS=y`; absent otherwise, as a missing export rather than a weaker
cipher.

<!-- check:skip -->  <!-- AESGCM needs a CONFIG_TLS=y build -->
```js
const c = new AESGCM(key);                  // key 16/24/32 bytes
const sealed = c.seal(nonce12, plaintext, aad);   // ciphertext || 16-byte tag
const plain  = c.open(nonce12, sealed, aad);      // THROWS on auth failure
new ChaCha20Poly1305(key32);                // same interface

Ed25519Generate();                          // { privateKey, publicKey }
Ed25519Sign(privateKey, message);           // 64 bytes
Ed25519Verify(publicKey, message, sig);     // boolean
X25519Generate(); X25519Derive(priv, peerPub);

JWTSign(payload, pemPrivateKey, { alg: "RS256" });
JWTVerify(token, pemPublicKey, { algorithms: ["RS256"] });   // allowlist REQUIRED
```

`open()` throws rather than returning a boolean so a forged message cannot be
used by forgetting to check. `Ed25519Verify` returns a boolean because a bad
signature yields no plaintext; a wrong-size *signature* returns false while a
wrong-size *key* throws — an attacker supplies the former.

`JWTVerify` requires an explicit `algorithms` allowlist and never reads `alg`
from the token to choose a key. That closes `alg:none` and RS256/HS256
confusion. RS256/384/512 and ES256/384/512 take PEM keys.

## dyna:scrape — Fetcher and Crawl

<!-- check:skip -->  <!-- fragment: `agent` is defined by the surrounding prose -->
```js
const f = new Fetcher({ agent, client,      // BOTH required, no defaults
                        robots: true, minDelayMs: 1000, retries: 3,
                        maxRedirects: 5, maxBodyBytes: 8 << 20 });
f.get(url);      // { status, headers, body, url } or { skippedByRobots: true }
f.stats();       // { fetched, skippedByRobots, retried, throttledMs, bytes }

const c = new Crawl(f, { maxPages: 100, maxDepth: 2, sameHost: true });
for (const page of c.start(seedUrl, extractor, HTMLParse)) { /* page.value */ }
```

`agent` has no default: a shared one is indistinguishable from anonymous and
denies an operator the one thing they need. `client` is injected so `dyna:scrape`
does not link `dyna:net`, and so a test can drive the policy against a mock.
`Crawl` takes links from the extractor's `links` field rather than scanning HTML
itself, and its iterator is lazy — one fetch per step.
