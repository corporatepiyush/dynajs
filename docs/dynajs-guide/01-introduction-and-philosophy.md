<p align="right"><i>Chapter 1 of <a href="README.md">The DynaJS Guide</a> · Next: <a href="02-installation-and-first-steps.md">2 · Installation &amp; First Steps →</a></i></p>

# Chapter 1 — Introduction & Philosophy

> **In this chapter**
> [1.1 A first taste](#11-a-first-taste) ·
> [1.2 What DynaJS is](#12-what-dynajs-is) ·
> [1.3 The QuickJS lineage](#13-the-quickjs-lineage) ·
> [1.4 The core thesis](#14-the-core-thesis-a-runtime-is-its-standard-library) ·
> [1.5 Non-goals](#15-what-dynajs-deliberately-does-not-do) ·
> [1.6 Where it fits](#16-where-dynajs-fits-and-where-it-doesnt) ·
> [1.7 The positioning table](#17-the-positioning-in-one-table)

---

## 1.1 A first taste

Four things that would each be an npm install (or a native addon) elsewhere. All of them come from
the standard library, with no dependencies and no build step:

```js
import { SHA256Hex } from "dyna:hash";
import { v7 } from "dyna:uuid";
import { dot } from "dyna:simd";
import { parseAddr, contains } from "dyna:net";

// A content hash — native, streaming-capable, standard test-vector verified.
print(SHA256Hex("hello world"));
//   b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9

// A time-ordered UUID (RFC 9562 v7) — good as a database key.
print(v7());
//   019f9a9d-74e6-7be4-8f8f-dfaf26fe0609   (sorts by creation time)

// A SIMD dot product over Float32Array — one native call, vectorized.
print(dot(new Float32Array([1,2,3,4]), new Float32Array([5,6,7,8])));
//   70

// IP/CIDR reasoning that JavaScript has no built-in for.
print(contains("10.0.0.0/8", "10.1.2.3"));     // true
print(parseAddr("::ffff:127.0.0.1").is6);      // true — an IPv4-mapped IPv6 address
```

Save it as `taste.js` and run `dynajs taste.js`. No install step, no `package.json`, no build.
[Chapter 2](02-installation-and-first-steps.md) gets you to exactly that point.

The rest of this chapter is the *why*. It is short and opinionated, and it will save you from
picking the wrong tool for the job.

> [!TIP]
> In a hurry? Skip to [Chapter 2](02-installation-and-first-steps.md). If you're *evaluating*
> DynaJS, read [§1.6](#16-where-dynajs-fits-and-where-it-doesnt) and
> [§1.7](#17-the-positioning-in-one-table) first.

---

## 1.2 What DynaJS is

DynaJS is a JavaScript runtime built on a fork of Fabrice Bellard's **QuickJS** engine (the
`2026-06-04` release).

QuickJS is tiny, correct, and embeddable: a complete ES2023+ engine in a handful of C files, with a
bytecode compiler, a small register/stack virtual machine, reference-counting garbage collection,
and near-instant startup.

DynaJS keeps that core and adds what a *runtime* needs to be useful on its own — all shipped inside
the binary, with no package manager and nothing to resolve at install time:

- a curated, native, **SIMD-accelerated standard library** under the `dyna:` module namespace
- an async I/O reactor and buffered file I/O
- an HTTP client and application server
- cryptographic hashes, MACs and key derivation, compression, and codecs
- a multi-ISA vector-math engine and classic machine-learning models

The one-line positioning:

> **DynaJS = QuickJS + a native async reactor + a hand-built, dependency-free,
> performance-first standard library.**

Every capability is first-party, compiled in, and versioned with the runtime itself.

---

## 1.3 The QuickJS lineage

Where DynaJS came from explains most of its character. QuickJS was engineered by Bellard (of FFmpeg,
QEMU, and the Bellard formula fame) around a few uncompromising ideas.

| | |
|---|---|
| **Small and complete** | The whole engine is a unity build — a master C file that `#include`s layered source fragments — compiling to a compact static library and a ~1 MB interpreter. No JIT, no multi-megabyte snapshot, no warmup. |
| **Fast startup, low memory** | With no JIT to warm and no heavyweight heap to initialize, the process is ready in microseconds and idles in a few megabytes. That is the opposite end of the spectrum from V8, which trades startup and memory for peak throughput on long-running hot loops. |
| **Correctness first** | QuickJS tracks the ECMAScript test suite (`test262`) closely. DynaJS inherits and *guards* that: the project holds a fixed baseline — **58 failures out of 83,744** — and every change must keep it. Modern language features get added without regressing conformance. |
| **Refcount GC + cycle collection** | Objects are freed the moment their last reference drops; a separate trial-deletion pass reclaims cycles. Combined with the runtime's native-module memory model, this gives remarkably *flat* memory behaviour — no stop-the-world pauses tied to a tracing collector's schedule. |

> [!NOTE]
> Modern QuickJS — the 2026 vintage DynaJS forks — is **not** the old 2019 engine. It has **rope
> strings** (concatenation without copying), a **slab allocator** that stores the refcount in the
> malloc block header, a **short-BigInt** representation, and a **token-threaded (computed-goto)**
> interpreter for fast dispatch. DynaJS builds on all of that.

---

## 1.4 The core thesis: a runtime is its standard library

A bare engine executes JavaScript. A *runtime* lets you build things: read files, hash bytes, open
sockets, parse data, do math at speed.

The usual answer is a small core plus a large third-party ecosystem. That buys reach, and it costs
something specific: a program's real dependency surface becomes a graph you did not write, cannot
easily audit, and which changes underneath you between builds.

DynaJS makes the opposite bet — **the runtime ships the batteries itself**, curated and native. The
capability is in the binary you built, or it is not there at all.

| Task | DynaJS |
|---|---|
| Pad a string | `"x".padStart(10)` (ECMAScript) |
| Hash bytes | `import { SHA256Hex } from "dyna:hash"` |
| Vector maths on typed arrays | `import { f64Dot } from "dyna:simd"` |
| Parse an IP or CIDR | `import { parseAddr } from "dyna:net"` |
| Fit and serve a small model | `import { RandomForestClassifier } from "dyna:ml"` |

Everything in the right column lives in the binary, is written in C, is SIMD-accelerated where that
pays, and is verified in-tree against standard vectors or reference implementations.

That is why the standard library is the heart of this book ([Chapter 4](04-standard-library.md)).
It is also why DynaJS is **deliberately not Node-compatible** — the two philosophies pull in
opposite directions.

---

## 1.5 What DynaJS deliberately does *not* do

Being explicit about non-goals is part of the design.

| Non-goal | What that means in practice |
|---|---|
| **No package ecosystem, and no compatibility layer for one** | No `require()`, no CommonJS, no drop-in shims. DynaJS modules live under `dyna:`, are ES modules, and have their own APIs designed for this runtime. |
| **No JIT** | DynaJS is an interpreter. It wins on startup, memory, and predictability, not on multi-minute numeric hot loops where a tracing JIT pulls ahead. Where raw compute matters, DynaJS gives you **native SIMD kernels** (`dyna:simd`, `dyna:ml`) rather than hoping a JIT vectorizes your loop. |
| **No package manager, no dependency directory** | Dependencies you don't control are a cost. DynaJS's answer to "I need X" is "X should be a curated native module," not "add a dependency." |
| **No re-skinning of JavaScript built-ins** | There is no module duplicating `parseInt`, no module duplicating `/\p{L}/u`, and no second `readFile`. A native module has to earn its place by filling a *real* gap or delivering *real* performance. |

> [!IMPORTANT]
> **That curation bar is enforced, including retroactively.** Two modules were deleted after review
> found they duplicated capability the runtime already had: `dyna:sort` (its `sort(arr, cmp)` was
> exactly `arr.toSorted(cmp)`) and `dyna:search` (the same SIMD kernel and byte-offset convention as
> `dyna:matcher`). Hex and base64 existed in *three* modules and now exist in one. Shrinking the
> surface is treated as progress.

---

## 1.6 Where DynaJS fits (and where it doesn't)

### ✅ An excellent fit when you want

| | |
|---|---|
| **Instant startup, low memory** | CLI tools, serverless/edge functions with cold-start budgets, embedded scripting, short-lived workers. A QuickJS-lineage process is ready before V8 has finished parsing its snapshot. |
| **Predictable, flat memory** | Long-running services that must not accumulate RSS. Reference counting frees promptly; classes owning a descriptor, socket or large buffer release **deterministically** via `close()` / `[Symbol.dispose]()`. Measured: **1.8–2.2 MB flat** across 1→1024 HTTP connections, against Node's 64→97 MB. |
| **A dependency-free standard library** | No supply chain, no `npm audit`, no lockfile drift. What ships in the binary is what runs. |
| **Native vector math from JavaScript** | `dyna:simd` exposes a multi-ISA (scalar / NEON / SSE4.2 / AVX2 / AVX‑512 / SVE) kernel set — dot products, norms, distances, activations, GEMM, f32 and f64 — with no native-addon build step. |
| **Data-plane utilities at C speed** | Hashing, compression, substring search (**14.6 GiB/s**, ~31× `String.indexOf`), byte manipulation, base64/hex/base32 — all native, several SIMD-accelerated. |

### ⚠️ Reach for a different tool when you need

| | |
|---|---|
| **The npm ecosystem** | If your project's value is "it glues together 400 npm packages," DynaJS is the wrong runtime — by design. |
| **Peak long-running numeric throughput in plain JS** | A tracing JIT (V8/JSC) out-runs an interpreter on a hot arithmetic loop that runs for minutes. DynaJS's counter is native kernels; if your workload can't use them, weigh the trade-off. |
| **A large third-party catalogue** | The standard library is 21 curated modules, not a registry. Some SIMD paths (certain AVX‑512 kernels) are conservatively gated pending hardware verification and fall back to the verified AVX2 path. |

> [!NOTE]
> **How to read performance claims in this book.** Every number has a benchmark behind it in
> [`bench/`](../../bench/), and the reports record what was tried and **reverted** as well as what
> landed — a SIMD kernel that lost to portable C, a distance identity that was slower *and* less
> accurate, an inline cache that bought nothing. If a claim can't be reproduced on the hardware in
> front of us, the docs say so rather than quoting it.

---

## 1.7 The positioning, in one table

| Dimension | DynaJS |
|---|---|
| Engine / language | QuickJS fork, C17 interpreter — no JIT, no warmup, predictable timing |
| Startup | **microseconds** |
| Idle memory | **a couple of MB**, and flat under load |
| Concurrency model | single-thread kqueue/epoll/io_uring reactor, plus workers |
| Standard library | 22 native `dyna:*` modules, curated, SIMD-accelerated, compiled in |
| Third-party code | **none by design** — the standard library grows instead |
| Native SIMD from the language | **yes, built in** — not an add-on or an FFI hop |
| Deployment unit | one binary; the capability set is fixed at build time |
| Best at | startup, footprint, embedding, curated native speed from JavaScript |

The rest of this book substantiates each row with runnable examples.

---

<p align="center">
<b>Next:</b> <a href="02-installation-and-first-steps.md">Chapter 2 — Installation &amp; First Steps →</a><br>
<sub><a href="README.md">← Back to the guide index</a></sub>
</p>
