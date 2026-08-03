<h1 align="center">The DynaJS Guide</h1>

<p align="center">
  <i>A complete book on the DynaJS runtime — for beginners, for experienced engineers,<br>
  and for AI coding agents.</i>
</p>

<p align="center">
  <a href="01-introduction-and-philosophy.md">1 · Introduction</a> ·
  <a href="02-installation-and-first-steps.md">2 · Installation</a> ·
  <a href="03-language-and-runtime.md">3 · Language &amp; Runtime</a> ·
  <a href="04-standard-library.md">4 · Standard Library</a> ·
  <a href="API.md">★ API Reference</a>
</p>

---

## Start here

```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynascript/master/install.sh | bash

dynajs -e 'import("dyna:hash").then(h => print(h.SHA256Hex("hi")))'
#   8f434346648f6b96df89dda901c5176b10a6d83961dd3c1ac88b59b2dc327aa4
```

That is the whole install story: one binary, no package manager, no lockfile, nothing to resolve.
Hashing, compression, HTTP, SIMD vector math, machine learning, IP parsing and sixteen other
capabilities are already inside it.

DynaJS is a small, fast JavaScript runtime built on a fork of Fabrice Bellard's **QuickJS**
(release `2026-06-04`), written in C17 as a unity build. It starts in microseconds, idles in a few
megabytes, and ships a native, SIMD-accelerated standard library under the `dyna:` namespace.

> [!IMPORTANT]
> **A deliberate stance, stated up front.**
> DynaJS has no package ecosystem. Capabilities arrive as **first-party native modules** under
> `dyna:` — audited, curated, SIMD-accelerated, dependency-free, and designed together rather than
> assembled. Modules are named and shaped for DynaJS. The unit of trust is the binary you built.

> [!TIP]
> **Batteries on the prototypes, too.** Beyond the `dyna:*` modules, DynaJS installs hundreds of
> native methods that ECMAScript does not define straight onto the built-in prototypes — written in
> C, **non-enumerable, and with no import**. None of them ever shadows an ECMAScript method.
>
> ```js
> [1,1,2,3].dropRepeats()                       // [1,2,3]
> [10,9,1].sortBy()                             // [1,9,10]   numeric, unlike bare .sort()
> [[1,2],[3,4]].sequence(Array)                 // [[1,3],[1,4],[2,3],[2,4]]
> ((x=>x+1).pipe(x=>x*2))(3)                    // 8
> Object.mergeDeepRight({a:{x:1}}, {a:{y:2}})   // {a:{x:1,y:2}}
> new Float64Array([1,2,3,4]).sum()             // 10   (SIMD)
> ```
>
> Full catalogue: [**API Reference → Built-in prototype extensions**](API.md#built-in-prototype-extensions).

---

## Table of contents

| | Chapter | What you'll learn | Read it if… |
|:--:|---|---|---|
| **1** | [**Introduction & Philosophy**](01-introduction-and-philosophy.md) | What DynaJS is, the QuickJS lineage, the "a runtime is its standard library" thesis, and where DynaJS fits | …you're deciding whether DynaJS fits your problem |
| **2** | [**Installation & First Steps**](02-installation-and-first-steps.md) | Building from source, the CLI and its flags, the REPL, your first scripts, ES modules and the `dyna:` namespace | …you want a working binary in the next five minutes |
| **3** | [**The Language & the Runtime**](03-language-and-runtime.md) | The ECMAScript baseline (ES2023–2026), deterministic resource disposal, `std`/`os`, workers and `SharedArrayBuffer`, BigInt | …you know JavaScript and want to know what's different here |
| **4** | [**The Standard Library**](04-standard-library.md) | Every `dyna:*` module with worked, verified examples — text, bytes, crypto, math, SIMD, ML, files, networking (HTTP, TCP/UDP, DNS, Redis, PostgreSQL, SQLite), data structures, graphs | …you're writing real programs |
| **★** | [**API Reference**](API.md) | Every module, every function: complete signatures, return types, and throwing conditions | …you need the exact contract |

### Suggested reading paths

| You are… | Path | Why |
|---|---|---|
| New to runtimes | 1 → 2 → 3 → 4 | start to finish, about an afternoon |
| New to DynaJS | 1 → 3.2 → 4 | positioning, what's different, then the library |
| Evaluating for production | 1.5 → 1.6 → 4 → API | fit, comparison, surface, contracts |
| Writing code right now | 2 → 4 → API | build it, find the module, get the signature |
| An AI coding agent | API → 4 | contracts first, then usage patterns |

> [!NOTE]
> **Every code example in this book is real and runs.** The APIs were extracted from the source and
> the test suites, not invented, and the printed outputs are what the binary actually produced.

---

## Install in one command

The installer clones, builds from source with the full native standard library, and installs the
`dynajs` binary — overwriting any previous installation. Running it again is how you **upgrade** or
**repair** an install.

```sh
# No checkout needed:
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynascript/master/install.sh | bash

# Anywhere root is not available:
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynascript/master/install.sh | bash -s -- --prefix "$HOME/.local"

# From a checkout, which is where --dry-run and --verbose are easiest to reach:
./install.sh --dry-run             # the preflight report, then stop
./install.sh --verbose             # a live build transcript
./install.sh --uninstall           # remove it
```

**Supported:** macOS, Linux, FreeBSD. Needs `git`, `make`, and a C compiler (`clang` preferred);
pass `--with-deps` to let it install those via your package manager. To build by hand, see
[Chapter 2](02-installation-and-first-steps.md).

---

## How DynaJS compares (in brief)

DynaJS is **not** trying to beat V8 on a long numeric hot loop — a tracing JIT wins there, and
DynaJS answers with native SIMD kernels instead. Where DynaJS *does* win:

| | |
|---|---|
| ⚡ **Startup** | microseconds — no JIT warmup, no snapshot to load |
| 📉 **Memory** | a couple of MB idle, and *flat* under load: reference counting plus deterministic native disposal, with a shared receive buffer instead of per-connection allocation |
| 🔀 **I/O concurrency** | a single-thread kqueue/epoll/io_uring reactor that runs *your* handlers — ~114k req/s at 3.0 MB peak RSS, p99 of **8.5 ms at 1024 connections** ([`bench/REPORT.md`](../../bench/REPORT.md)) |
| 🔋 **Batteries** | a curated, dependency-free, SIMD-accelerated standard library. Nothing to resolve, no supply chain, no lockfile |

**What DynaJS is, on its own terms**

| | |
|---|---|
| Engine | QuickJS fork — an interpreter, not a JIT: predictable timing, no warmup, tiny footprint |
| Startup / idle | microseconds / a couple of MB |
| Standard library | 22 native `dyna:*` modules, compiled into the binary |
| Third-party code | none, by design |
| SIMD | built into the language surface, not an add-on |
| Deployment unit | one binary |

**It fits** CLI tools run thousands of times, cold-start-sensitive edge and serverless work,
embedding, memory-bound services, and native vector maths driven from JavaScript.

**It does not fit** work whose value comes from a large third-party package ecosystem, or workloads
that need a JIT's peak throughput on long-running numeric loops written in JavaScript itself —
DynaJS moves that work into native modules instead.

---

## Lineage: from QuickJS to DynaJS

DynaJS is a fork of Fabrice Bellard's **QuickJS** (`2026-06-04`). QuickJS contributed the DNA that
defines DynaJS: a tiny, correct, `test262`-tracking engine with a bytecode compiler, a small VM,
reference-counting GC with cycle collection, and near-instant startup — no JIT, no snapshot.

The 2026-vintage engine DynaJS builds on already has **rope strings**, a **slab allocator** with the
refcount in the malloc-block header, **short-BigInt**, and a **token-threaded (computed-goto)
interpreter**.

What DynaJS *adds* is the thing that turns an embeddable engine into a runtime:

```
                    ┌─────────────────────────────────────────────┐
   DynaJS adds ───► │  dyna:*  native standard library            │
                    │  SIMD · crypto · http · file · ml · …       │
                    ├─────────────────────────────────────────────┤
                    │  async I/O reactor (kqueue/epoll/io_uring)  │
                    ├─────────────────────────────────────────────┤
                    │  deterministic native-resource memory model │
                    ╞═════════════════════════════════════════════╡
   QuickJS gave ──► │  bytecode compiler · small VM · refcount GC │
                    │  + cycle collection · test262 conformance   │
                    └─────────────────────────────────────────────┘
```

The engine stayed small and correct; the runtime grew around it.

---

<p align="center">
<i>This guide is versioned with the engine. The runtime identifies itself as</i>
<code>DynaJS version 2026-06-04</code>.
</p>
