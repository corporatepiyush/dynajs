<p align="right"><i><a href="01-introduction-and-philosophy.md">← 1 · Introduction</a> · Chapter 2 of <a href="README.md">The DynaJS Guide</a> · <a href="03-language-and-runtime.md">3 · Language &amp; Runtime →</a></i></p>

# Chapter 2 — Installation & First Steps

> **In this chapter**
> [2.1 Building from source](#21-building-from-source) ·
> [2.2 The CLI](#22-the-command-line-interface) ·
> [2.3 Your first script](#23-your-first-script) ·
> [2.4 Modules & the `dyna:` namespace](#24-es-modules-and-the-dyna-namespace) ·
> [2.5 The REPL](#25-the-repl) ·
> [2.6 A bigger program](#26-a-slightly-bigger-first-program)

One command and you have a runtime with the whole native standard library in it:

```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynascript/master/install.sh | bash

dynajs -e 'import("dyna:hash").then(c => print(c.SHA256Hex("hi")))'
#   8f434346648f6b96df89dda901c5176b10a6d83961dd3c1ac88b59b2dc327aa4
```

The rest of this chapter fills in the CLI, the REPL, ES modules, and two real programs.

> [!NOTE]
> Everything here is copy-paste-runnable, and every printed output below is what the binary actually
> produced. If a command doesn't do what the book says, that's a bug in the book, not in you.

---

## 2.1 Building from source

DynaJS is a C17 project with **no external dependencies** for the core. You need a C compiler
(**clang** preferred; gcc works on Linux) and `make`. On macOS the build auto-selects clang; on
Linux it defaults to gcc, and you can force clang with `CONFIG_CLANG=y`.

### The fast path

```sh
curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynascript/master/install.sh | bash
```

It clones into `~/.cache/dynajs-build`, builds with `CONFIG_NATIVE_MODULES=y`, and installs one
binary to `/usr/local/bin/dynajs`. Running it again is how you upgrade, and how you repair a build
that went wrong — it discards the old tree every time.

It reads nothing from stdin, so the pipe is safe, and it asks no questions. What it does instead is
**print the whole plan first** — your OS, the compiler it found, the job count, the install
directory, whether that directory will need `sudo`, and which version it is about to replace — then
report each phase as it happens. Options go after `bash -s --`:

| Flag | What it does |
|---|---|
| `--prefix DIR` | Install to `DIR/bin` instead. `--prefix "$HOME/.local"` needs no root at all. |
| `--dry-run` | Print the preflight report and stop, having downloaded nothing. |
| `--verbose` | A live build transcript, plus every command the script runs. |
| `--jobs N` | Parallel build jobs. A value that is not a number is **refused**, not clamped. |
| `--with-deps` | Install the prerequisites first. Where there is **no package manager at all** — a fresh Mac — it installs Homebrew and uses that. |
| `--uninstall` | Remove the binary again. |
| `--help` | All of the above, with the environment variables that mirror them. |

The full build log is kept at `~/.cache/dynajs-build/install.log` whether it succeeds or fails, and
a failure prints its last 25 lines rather than telling you where to go looking.

One dependency is worth knowing about because its absence is **silent**: the Makefile links SQLite
through `pkg-config`, and without both of those it leaves `dyna:net`'s `SQLite` class out of the
binary rather than failing the build. The preflight report says which way it went —

```
  - sqlite      3.53.4 — dyna:net SQLite included
```

— and `--with-deps` installs `pkg-config` and `sqlite` along with the required tools.

### By hand

```sh
git clone https://github.com/corporatepiyush/dynascript
cd dynascript

# Core build (the engine + the `std`/`os` modules), parallel:
make -j"$(getconf _NPROCESSORS_ONLN)"

# You now have a ./dynajs binary:
./dynajs -e 'print("hello from DynaJS")'
#   hello from DynaJS
```

That default build gives you the language and the two classic QuickJS system modules (`std`, `os`).
To get the **native standard library** — everything under the `dyna:` namespace that this book is
about — build with `CONFIG_NATIVE_MODULES=y`:

```sh
make CONFIG_NATIVE_MODULES=y -j"$(getconf _NPROCESSORS_ONLN)"

./dynajs -e 'import("dyna:hash").then(c => print(c.SHA256Hex("hi")))'
#   8f434346648f6b96df89dda901c5176b10a6d83961dd3c1ac88b59b2dc327aa4
```

> [!WARNING]
> **One flag, one rule.** A `-D` flag change (like toggling `CONFIG_NATIVE_MODULES`) does **not**
> re-trigger recompilation on its own — `make` tracks file timestamps, not flags. After changing a
> config flag, run `make clean` first, or you will link stale objects and see confusing symptoms
> like *"could not load module 'dyna:hash'"* from a binary you just built. Fresh clones are immune.

### Build variants

| Command | What you get |
|---|---|
| `make` | core engine + `std`/`os` |
| `make CONFIG_NATIVE_MODULES=y` | **+ the entire `dyna:*` standard library** |
| `make CONFIG_ASAN=y` | AddressSanitizer build (its own objdir) |
| `make CONFIG_UBSAN=y` | UndefinedBehaviorSanitizer build |
| `make CONFIG_LTO=y` | link-time optimization (smaller/faster, slower build) |
| `make CONFIG_NATIVE=y` | `-mcpu=native` tuning — for benchmarking, not for shipping |

### Verifying the build

| Command | What it checks |
|---|---|
| `make test` | the language suites (needs any build) |
| `make test-native` | every `dyna:*` module suite (needs a `CONFIG_NATIVE_MODULES=y` build) |
| `./dev.sh gate` | **the full proof**: zero-warning build → ASan → UBSan → `make test` → the `test262` baseline |

Run `make test` once to confirm a healthy build. If you're contributing, `./dev.sh gate` is the bar
every change has to clear.

---

## 2.2 The command-line interface

The three invocations you will use constantly:

```sh
dynajs script.js          # run a file (module vs script auto-detected)
dynajs -e 'EXPR'          # evaluate an expression and exit
dynajs -i                 # open the interactive REPL
```

The binary self-describes with `--help`:

```
DynaJS version 2026-06-04
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
-s                    strip all the debug info
    --strip-source    strip the source code
    --io-threads N    IO worker threads (default max(ncpu,4), 0 = auto)
-q  --quit         just instantiate the interpreter and quit
```

`--io-threads` sizes the process-wide pool that services operations which genuinely **block** — disk
reads and writes, `fsync`, name resolution. Sockets are not offloaded to it: the reactor already
knows when they are ready, and handing a ready socket to another thread costs more than the read. On
Linux with `CONFIG_IO_URING` the kernel does disk asynchronously and the pool is not used for it at
all. The default is `max(ncpu, 4)`; it also becomes the default for an `App`/`HTTPServer`'s
`workers`, so one flag sizes the process rather than two knobs disagreeing.

`dynajs -d` prints a memory-usage report on exit — the low-footprint story, in numbers you can check
yourself:

```sh
dynajs -d -e '1+1'
#   DynaJS memory usage -- 2026-06-04 version, 64-bit, malloc limit: -1
#   ...per-structure sizes, JSObject class histogram, allocation counts...
#   memory allocated           84   272640  (3245.7 per block)
```

---

## 2.3 Your first script

Create `hello.js`:

```js
// Top-level code just runs. `print` is a global; so is `console`.
print("Hello, DynaJS!");
console.log("console works too, and its methods are enumerable (WHATWG).");

const nums = [5, 3, 8, 1, 9, 2];
const evens = nums.filter(n => n % 2 === 0);
console.log("evens:", evens, "sum:", evens.reduce((a, b) => a + b, 0));
```

```sh
dynajs hello.js
#   Hello, DynaJS!
#   console works too, and its methods are enumerable (WHATWG).
#   evens: [ 8, 2 ] sum: 10
```

> [!TIP]
> Note `[ 8, 2 ]`, not `8,2` — `console.log` **inspects** its arguments (arrays and objects are
> formatted structurally), while `print` stringifies them. Reach for `console.log` when debugging and
> `print` when you want exact output.

Everything you know from modern JavaScript works: arrow functions, destructuring, spread,
`async`/`await`, generators, classes, `Map`/`Set`/`WeakMap`, typed arrays, `BigInt`, template
literals, optional chaining, nullish coalescing, `Array` grouping and `findLast`, and more.
[Chapter 3](03-language-and-runtime.md) covers the exact baseline.

---

## 2.4 ES modules and the `dyna:` namespace

DynaJS uses standard ES modules. Auto-detection treats a file that uses `import`/`export` as a
module; force it with `-m` if needed.

```js
// math-utils.js
export function mean(xs) {
  return xs.reduce((a, b) => a + b, 0) / xs.length;
}
```

```js
// main.js
import { mean } from "./math-utils.js";
import { v4 } from "dyna:uuid";

console.log("mean:", mean([2, 4, 6, 8]));   // mean: 5
console.log("id:", v4());                   // id: d2aab66f-885e-4262-b54b-f080facd51c9
```

```sh
dynajs main.js
```

### Three kinds of module specifier

| Specifier | Example | What it is |
|---|---|---|
| **Relative / absolute path** | `"./math-utils.js"` | Your own code. |
| **`dyna:*`** | `"dyna:hash"` | The native standard library, compiled into the binary (needs `CONFIG_NATIVE_MODULES=y`). |
| **`std` / `os`** | `"std"` | The classic QuickJS system modules (low-level I/O, process control). Enabled by `--std` or imported directly in a module. |

```js
import * as std from "std";
import * as os from "os";

const [dir, err] = os.readdir(".");
if (!err) console.log("entries:", dir.length);
std.gc();   // force a GC cycle (useful in tests / benchmarks)
```

> [!NOTE]
> **Namespacing is a feature.** Because the standard library lives under `dyna:`, there is never
> ambiguity about whether an import is yours, a third party's, or the runtime's — there *is* no third
> party. `dyna:` always means "native, in-binary, curated."

---

## 2.5 The REPL

`dynajs -i` gives you an interactive session with the full runtime, including dynamic imports and
top-level `await`:

```
$ dynajs -i
DynaJS - Type "\h" for help
dynajs > const { HexEncode } = await import("dyna:encoding")
undefined
dynajs > HexEncode(new Uint8Array([222, 173, 190, 239]))
"deadbeef"
dynajs > await import("dyna:mathx").then(m => m.gcd(48, 36))
12n
```

(`gcd` returns a `BigInt` — see [§3.5](03-language-and-runtime.md#35-bigint-and-numeric-range) for
why the integer helpers do.)

Inside a plain script (not a module), use dynamic `import()` to reach the standard library, as in the
one-liners throughout this book. Inside a module, prefer static `import`.

`_` holds the last result and `_err` the last error, so a failed expression can be inspected without
running it again:

```
dynajs > JSON.parse("{oops}")
SyntaxError: unexpected token: 'oops'
dynajs > _err.message
"unexpected token: 'oops'"
```

A result that is a promise is settled before it is printed, so `await` is optional at the prompt.

### History

The REPL keeps history in `~/.dynajs_history` (the last 1000 entries, written with mode `0600`
because a session routinely contains pasted secrets). Set `DYNAJS_HISTORY` to move the file, or to
an empty string to keep no history at all:

```bash
DYNAJS_HISTORY= dynajs -i
```

History is saved when you leave through `\q` or Ctrl-D.

### Editing keys

Multi-line pastes arrive as one unit, so pasting a function does not run it a line at a time.

| Key | |
|---|---|
| `Tab` | complete a global, a property, a `\directive`, or a path inside a string or after `\load` |
| `Ctrl-R` / `Ctrl-S` | incremental search backwards / forwards through history; `Ctrl-G` cancels |
| `Up` / `Down` | previous / next entry — `Down` past the end restores the line you were typing |
| `Ctrl-A` / `Ctrl-E` | start / end of line |
| `Ctrl-W` / `Ctrl-U` | delete the previous word / to the start of the line |
| `Ctrl-Y` | paste back what was deleted |
| `Ctrl-L` | clear the screen |
| `Ctrl-C` | discard the line being edited, or abandon a running evaluation |
| `Ctrl-D` | exit on an empty line |

### Directives

Type `\h` for the current list.

| | |
|---|---|
| `\h` | this help |
| `\load <file>` | load and evaluate a script |
| `\x` / `\d` | show numbers in hexadecimal / decimal |
| `\t` | toggle the timing display |
| `\clear` | clear the terminal |
| `\q` | exit |

---

## 2.6 A slightly bigger first program

A tiny content-addressed store: hash a payload, give it a time-ordered id, and report its
gzip-compressed size. Four native capabilities, four imports, zero dependencies, no build step.

```js
import { SHA256Hex } from "dyna:hash";
import { v7 } from "dyna:uuid";
import { gzip } from "dyna:compress";
import { Base64Encode } from "dyna:encoding";

function store(payload) {
  const id = v7();                      // time-ordered id (sorts by creation)
  const digest = SHA256Hex(payload);    // content hash
  const packed = gzip(payload);         // real DEFLATE, native
  return {
    id,
    digest,
    originalBytes: payload.length,
    packedBytes: packed.length,
    packedB64: Base64Encode(packed),
  };
}

const rec = store("the quick brown fox ".repeat(100));
console.log("id:        ", rec.id);
console.log("SHA256:    ", rec.digest);
console.log("orig/packed:", rec.originalBytes, "→", rec.packedBytes, "bytes");
```

```sh
dynajs bigger.js
#   id:         019f9a9e-92fe-7266-ba28-204c022bc14c   (a v7 UUID — yours will differ)
#   SHA256:     e706ce82e8497c352c70f15711f5c43daabb6adca7ea66dbb65b2fc8b2e8c2d2
#   orig/packed: 2000 → 56 bytes
```

---

## What you have now

- ✅ A working `dynajs` binary, with or without the native standard library
- ✅ The CLI, the REPL, and the three module-specifier kinds
- ✅ A feel for the shape of DynaJS code: standard JavaScript at the top, native capability one
  `import` away

[Chapter 3](03-language-and-runtime.md) covers the language baseline precisely — what's supported,
what's different, and the runtime idea that matters most: deterministic resource disposal.
[Chapter 4](04-standard-library.md) is the full standard-library tour.

---

<p align="center">
<b>Next:</b> <a href="03-language-and-runtime.md">Chapter 3 — The Language &amp; the Runtime →</a><br>
<sub><a href="README.md">← Back to the guide index</a></sub>
</p>
