<p align="right"><i><a href="02-installation-and-first-steps.md">← 2 · Installation</a> · Chapter 3 of <a href="README.md">The DynaJS Guide</a> · <a href="04-standard-library.md">4 · Standard Library →</a></i></p>

# Chapter 3 — The Language & the Runtime

> **In this chapter**
> [3.1 The ECMAScript baseline](#31-the-ecmascript-baseline) ·
> [3.2 Deterministic resource disposal](#32-deterministic-resource-disposal) ·
> [3.3 `std` and `os`](#33-the-std-and-os-system-modules) ·
> [3.4 Workers & shared memory](#34-parallelism-workers-and-shared-memory) ·
> [3.5 BigInt](#35-bigint-and-numeric-range)

**If you know modern JavaScript, you already know most of DynaJS.** This chapter pins down exactly
*which* JavaScript you can rely on, then covers the four runtime-level pieces that are not modules:
deterministic resource disposal, `std`/`os`, workers, and BigInt.

Think of it as "the parts of the platform you get before you `import` anything."

---

## 3.1 The ECMAScript baseline

DynaJS inherits QuickJS's close tracking of the ECMAScript standard and holds a fixed `test262`
conformance baseline — **58 failures out of 83,744 tests (99.93%)** — that engine changes must not
regress. Every commit re-runs it.

In practice you get the full modern language, on the ES2023–ES2026 track:

```js
// Arrays: non-mutating copies + reverse search (ES2023)
print([3, 1, 2].toSorted().join(","));         // 1,2,3   (original untouched)
print([1, 2, 3, 4].toReversed().join(","));    // 4,3,2,1
print([1, 2, 3].with(1, 99).join(","));        // 1,99,3
print([5, 1, 8, 2].findLast(x => x < 5));      // 2
print([5, 1, 8, 2].findLastIndex(x => x < 5)); // 3

// Grouping (ES2024)
const grouped = Object.groupBy([1, 2, 3, 4, 5], n => (n % 2 ? "odd" : "even"));
print(JSON.stringify(grouped));                // {"odd":[1,3,5],"even":[2,4]}

// Everyday modern syntax
const user = { name: "Ada", address: { city: "London" } };
print(user?.address?.city ?? "unknown");       // London
const { name, ...rest } = user;                // destructuring + rest
```

<details>
<summary><b>The full "yes, that works" list</b></summary>

| Area | Supported |
|---|---|
| **Async** | `async`/`await`, **top-level `await`** in modules, async generators, `for await…of`, `Promise.any` / `allSettled` / `withResolvers` |
| **Classes** | private fields (`#x`) and private methods, static blocks, `static` fields, accessors |
| **Collections** | `Map`, `Set`, `WeakMap`, `WeakSet`, `WeakRef`, `FinalizationRegistry` |
| **Binary data** | every typed array, `DataView`, `ArrayBuffer` (incl. resizable), `SharedArrayBuffer`, `Atomics` |
| **Strings** | `replaceAll`, `at`, `padStart`/`padEnd`, `matchAll`, well-formed Unicode methods |
| **Objects** | `Object.hasOwn`, `Object.groupBy`, spread, getters/setters, `Proxy`, `Reflect` |
| **Syntax** | logical assignment (`??=`, `||=`, `&&=`), numeric separators, optional chaining, nullish coalescing |
| **RegExp** | named groups, lookbehind, the `d` (indices) and `v` (set notation) flags, `u` mode |
| **Errors** | `Error.cause`, `AggregateError`, `SuppressedError` |
| **Numbers** | `BigInt` (with short-BigInt optimization), `Number` statics, `Math` full surface |

</details>

### Two specifics worth knowing

**There is no `structuredClone` global.**

```js
print(typeof structuredClone);        // "undefined"
```

Copy explicitly, JSON round-trip plain data, or reach for a `dyna:*` module that already gives you
the shape you want.

**Resource disposal is a library, not syntax.** The disposable *protocol* — `Symbol.dispose`,
`DisposableStack`, `SuppressedError` — is fully present, and the next section is entirely about it.
What DynaJS parses is the library form:

```js
const stack = new DisposableStack();      // ✅ this is the API
const r = stack.use(openSomething());
```

---

## 3.2 Deterministic resource disposal

This is the runtime idea that matters most, and it connects directly to how every native module
manages memory.

**In a tracing-GC runtime, a file handle or socket is released "eventually," whenever the collector
runs a finalizer. Under DynaJS, `close()` frees the native resource immediately.** The class
finalizer is only a safety net for a leaked object, not the primary path. That is the mechanism
behind the flat RSS in the HTTP benchmark — 1.8 MB at one connection, 2.2 MB at 1024.

### The protocol

| Piece | What it does |
|---|---|
| `Symbol.dispose` / `Symbol.asyncDispose` | Well-known symbols an object implements to say *"here is how to release me."* |
| `DisposableStack` / `AsyncDisposableStack` | Containers that collect disposables and release them in **reverse** order. |
| `SuppressedError` | Folds a disposer failure together with an in-flight error, so you never silently lose one. |

### Which classes are disposable — and which aren't

DynaJS draws the line by **what the object owns**, not by which module it came from.

| | Owns | Examples | You must |
|---|---|---|---|
| **Resource classes** | a descriptor, a socket, or a large buffer | `FileReader`/`FileWriter`, `App`, `HTTPClient`, `CSVFile`, every `dyna:ml` model | call `close()` (or `[Symbol.dispose]()`, or use a stack) |
| **Plain objects** | just memory, and not much of it | `Hasher`, `Random`, every `dyna:structures` container (`Graph` included), `Matcher` | nothing — the GC reclaims them like a `Map` |

> [!TIP]
> The rule of thumb: **if a class has a `.close()`, releasing it early buys you something
> measurable.** If it doesn't, there was nothing worth managing.

### One resource: `try`/`finally`

```js
import { FileWriter, Path } from "dyna:file";

const w = new FileWriter(new Path("/tmp/out.txt"), { bufferSize: 4096 });
try {
  w.write("some data\n");
  w.sync();          // durable flush
} finally {
  w.close();         // deterministic release — memory returns *now*
}
```

### Several resources: `DisposableStack`

A stack removes the nested-`try`/`finally` pyramid and disposes everything in reverse on scope exit:

```js
import { FileReader, FileWriter } from "dyna:file";

function copyFiltered(src, dst) {
  const stack = new DisposableStack();
  const r = stack.use(new FileReader(src, { bufferSize: 1 << 16 }));
  const w = stack.use(new FileWriter(dst, { bufferSize: 1 << 16 }));

  let line;
  while ((line = r.readLine()) !== null) {
    if (!line.startsWith("#")) w.write(line + "\n");
  }
  w.sync();
  stack.dispose();     // disposes w, then r — reverse order, always runs
}
```

| Method | Use it for |
|---|---|
| `stack.use(x)` | register a disposable (returns `x`, so you can assign in one line) |
| `stack.defer(fn)` | register an arbitrary cleanup callback |
| `stack.adopt(value, fn)` | register a *non*-disposable value together with its disposer |
| `stack.move()` | transfer ownership to a new stack — for handing a half-built resource set to a caller |

`defer` and `adopt` work on anything, not just disposables, and they run in reverse registration
order like everything else on the stack:

```js
const stack = new DisposableStack();
stack.defer(() => print("deferred cleanup ran"));
stack.adopt(42, (v) => print("adopted", v));
stack.dispose();
//   adopted 42
//   deferred cleanup ran
```

`move()` is how a factory function builds several resources and hands them over as one unit —
if construction throws halfway, the local stack unwinds what it already owns:

```js
function build() {
  const stack = new DisposableStack();
  stack.defer(() => print("released"));
  return stack.move();       // this stack is now empty; the returned one owns everything
}

const owned = build();
print("built; nothing released yet");
owned.dispose();
//   built; nothing released yet
//   released
```

### When two disposers fail

If more than one disposer throws, `dispose()` folds them into a `SuppressedError` rather than
losing one — `.error` is the latest failure, `.suppressed` the one it displaced:

```js
const s = new DisposableStack();
s.defer(() => { throw new Error("first disposer"); });
s.defer(() => { throw new Error("second disposer"); });
try { s.dispose(); }
catch (e) {
  print(e.constructor.name);   // SuppressedError
  print(e.error.message);      // first disposer     (disposed last, so thrown last)
  print(e.suppressed.message); // second disposer
}
```

---

## 3.3 The `std` and `os` system modules

These are the classic QuickJS low-level modules — the thin layer over libc and the OS. They are
available in module context, or with the `--std` flag for scripts.

```js
import * as std from "std";
import * as os from "os";

// Low-level file I/O (std): open, read/write, printf-style formatting.
const f = std.open("/tmp/note.txt", "w");
f.puts("written via std\n");
f.close();
print(std.loadFile("/tmp/note.txt"));   // "written via std\n"

// OS facilities (os): filesystem, time, process.
const [entries, err] = os.readdir(".");
print(err ? "error" : `${entries.length} entries`);
os.sleep(10);                            // milliseconds
const t0 = os.now();                     // monotonic-ish clock

// Force a GC cycle — deterministic, useful in tests and benchmarks.
std.gc();
```

> [!TIP]
> For higher-level, ergonomic filesystem work prefer the native modules —
> [`dyna:file`](04-standard-library.md#dynafile--the-filesystem-module) gives you a buffered
> reader/writer with per-OS fast paths, plus metadata, globbing and temp files. `std`/`os` remain the
> escape hatch for raw syscall-level control.

---

## 3.4 Parallelism: workers and shared memory

DynaJS is single-threaded per context and scales across cores with **workers**. Each worker is an
isolated JavaScript context with its own heap; they communicate by message passing, and share raw
memory through `SharedArrayBuffer` + `Atomics`.

```
   main.js                                   compute-worker.js
   ┌──────────────────┐   postMessage    ┌──────────────────────┐
   │ new os.Worker()  │ ───────────────► │ os.Worker.parent     │
   │ worker.onmessage │ ◄─────────────── │ parent.postMessage() │
   └────────┬─────────┘                  └──────────┬───────────┘
            │        SharedArrayBuffer + Atomics    │
            └───────────────  zero copy  ───────────┘
```

The pattern is three lines: `new os.Worker(path)` on the parent, `os.Worker.parent` inside the
worker, `postMessage`/`onmessage` on both sides.

```js
// main.js
import * as os from "os";

const worker = new os.Worker("./compute-worker.js");

worker.onmessage = (e) => {
  const msg = e.data;
  if (msg.type === "result") {
    print("worker computed:", msg.value.toFixed(2));   // 666666166.46
    worker.onmessage = null;             // let the program exit
  }
};

worker.postMessage({ type: "start", n: 1_000_000 });
```

```js
// compute-worker.js
import * as os from "os";
const parent = os.Worker.parent;

parent.onmessage = (e) => {
  const { type, n } = e.data;
  if (type === "start") {
    let acc = 0;
    for (let i = 0; i < n; i++) acc += Math.sqrt(i);
    parent.postMessage({ type: "result", value: acc });
  }
};
```

### Sharing memory instead of copying it

A `SharedArrayBuffer` in a message is *not* copied — both contexts address the same bytes, and
`Atomics` gives you the synchronization primitives:

```js
// main.js
import * as os from "os";

const sab = new SharedArrayBuffer(1024 * 4);
const view = new Int32Array(sab);
for (let i = 1; i < 1024; i++) view[i] = i;      // slot 0 is the accumulator

const w = new os.Worker("./sum-worker.js");
w.onmessage = () => { print("total:", Atomics.load(view, 0)); w.onmessage = null; };
w.postMessage({ buf: sab, from: 1, to: 1024 });
//   total: 523776
```

```js
// sum-worker.js
import * as os from "os";

os.Worker.parent.onmessage = (e) => {
  const { buf, from, to } = e.data;
  const view = new Int32Array(buf);              // same memory, no copy
  let acc = 0;
  for (let i = from; i < to; i++) acc += view[i];
  Atomics.add(view, 0, acc);                     // publish atomically
  os.Worker.parent.postMessage("done");
};
```

> [!NOTE]
> Because each worker starts from the tiny QuickJS baseline, **spinning up a worker is cheap** — a
> genuinely different cost model from spawning a V8 isolate.

---

## 3.5 BigInt and numeric range

DynaJS has arbitrary-precision `BigInt` with an optimized **short-BigInt** representation for small
values (they avoid heap allocation entirely). Several standard-library modules return `BigInt`
precisely *because* it is exact where `Number` is not:

```js
import { factorial, gcd, bitLen, popcount } from "dyna:mathx";

print(factorial(25));                 // 15511210043330985984000000n  (exact, > 2^53)
print(gcd(462n, 1071n));              // 21n
print(bitLen(1000n));                 // 10   (bits needed to represent)
print(popcount(0xffffn));             // 16   (set bits)
```

**Rule of thumb:** use `Number` for measurements and ordinary arithmetic; reach for `BigInt` (and the
modules that return it) whenever a value can exceed 2⁵³ or must be bit-exact — 64-bit integer math,
large factorials and LCMs, exact identifiers, and the 64-bit accessors in
[`dyna:bytes`](04-standard-library.md#dynabytes--the-byte-buffer-javascript-never-shipped).

---

<p align="center">
<b>Next:</b> <a href="04-standard-library.md">Chapter 4 — The Standard Library, Module by Module →</a><br>
<sub>the heart of the book · <a href="README.md">← Back to the guide index</a></sub>
</p>
