# Error-message convention

Every native module throws errors a user reads while stuck. This is the one
grammar they follow, the discipline around option bags, and the two cases
where going bare is allowed on purpose. It exists because a cross-module audit
(2026) found 5+ coexisting prefix families and 42% of 1,619 throw sites bare —
including a `Serializer` that kept explaining an API that no longer existed.
That last rot is caught mechanically: `make check-api` runs
`tools/check-error-identifiers.py`, which fails the build when a message names
an API the binary does not export.

## 1. Prefix every thrown message with where it came from

The default is `<module>.<context>: message`, with the context chosen at the
narrowest scope that is truthful:

| Tier | Shape | Use when | Example |
|---|---|---|---|
| entry point | `<mod>.<op>:` | the throw belongs to one JS-visible function/method | `csv.updateCell: 'row' is required` · `ml.fit: fit(X, y) requires two arguments` · `os.setTimeout: too many live timers (cap %d)` |
| class | `<Class>:` | a constructor or a whole class's surface | `Worker: cannot create a worker inside a worker` · `Decimal: division by zero` |
| bare module tag | `<registered name>:` | a shared validator serves many entry points and cannot honestly name one | `dyna:schema: unknown type "%.*s"`; `std: missing argument for conversion specifier` (printf + sprintf share one helper); `os: read/write array buffer overflow` |

The bare tag uses the module's registered name, verbatim: `dyna:schema` →
`dyna:schema:`, `std` → `std:`, `os` → `os:`. A message that already names its
op (`"trainTestSplit: needs at least 2 samples"`) upgrades by prefixing the
module — it becomes `ml.trainTestSplit: needs at least 2 samples`, never a
doubled `ml.trainTestSplit: trainTestSplit: ...`. (Inside a shared helper that
serves several entry points, an op-flavored body stays as the message text
after the bare tag — `dyna:ml rocAuc: needs both a positive and a negative
sample` — where promoting `rocAuc:` to the prefix would mislabel the twin
entry point `averagePrecision` that shares the same C function.)

Exemplar modules, in descending order of strictness:

- **`src/dyna-schema.c`** — `dyna:schema:` on all 12 throw sites, zero bare.
- **`src/dyna-semver.c`** — `dyna:semver:` on 9 of 10; also the model for
  quoting user data (below).
- **`src/dyna-csv.c`** — both tiers in one file: the `csv:` tag for
  module-level helpers and `csv.<op>:` (`csv.create:`, `csv.updateCell:`) for
  entry points. This is the sanctioned shape when a module needs both.

`JS_ThrowOutOfMemory` is always bare: it has no module context, everywhere.

## 2. Going bare requires a documented reason

Bare messages are a deliberate exception, not a default. A throw site may stay
bare only when one of these holds, and the reason lives in a comment (at the
site or in the module header):

- **Engine-builtin posture.** Globals that mirror WHATWG/ECMA builtins
  (`JSON`, `TextEncoder`/`TextDecoder`, `queueMicrotask`) keep unprefixed
  spec-shaped messages, exactly like every reference engine.
- **Import substrate.** Module-loader machinery (`js_module_loader*`, import
  attributes, `import.meta` realpath) is owned by no module API; the message
  names the failing file, which is the useful part.
- **Wrapped internal.** The error is always caught and re-thrown (prefixed) by
  the calling module before it reaches JS.

Everything else gets a prefix. Both engine substrates that host shared surfaces
document their split in the module header (`src/dyna-ml.c`, `src/dyna-libc.c`).

## 3. Option bags: refuse, don't coerce

When an option value has the wrong type, throw — do not coerce and do not fall
back to the default as if the option were absent. `new Logger({ level: 42 })`
must not quietly mean "info"; a numeric `seed` that fails ToInt64 must throw,
not become seed 0. Name the option and the accepted set in the message:
`classWeight must be "balanced" or absent`, `kernel must be "linear", "rbf" or
"poly"`. (The audit's X-2: `dyna:log` swallowed `{level: 42}`, `{level: null}`,
even `{toString(){throw}}` — every one silently became `"info"`.)

## 4. Throwing getters propagate

Reading properties from user-supplied objects (options bags, result objects)
can run a JS getter. If that getter throws, the exception propagates: check
the read and bail out. Never swallow a throwing read and continue with a
default — that was X-12, where `dyna:scrape` read `status` unguarded and
crawled on with `status = 0`, and X-2's `{buffer: numThrower}`. Three postures
exist in the tree; the standard is the first:

1. **Propagate** (the standard) — decimal, mathx, matcher options, time,
   crypto Argon2id, random: a throwing getter rejects the call.
2. **Strict refuse** (defensible) — type-check before coercion and throw a
   conventional message: structures, semver, validate, compress level, json,
   file, ml, crypto PBKDF2, decimal ctor.
3. **Silent swallow / fallback** — banned. Every existing instance needs a
   documented reason or a fix ticket; new ones fail review.

## 5. User data inside messages is capped and quoted

Unbounded `%s` of user-controlled strings turns a 1 MB column name into a 1 MB
error string. Cap identifiers with `%.*s` (64–128) and quote them:
`dyna:semver: invalid version "%.*s"` (dyna-semver.c is the model). Prefer the
bounded form in every new message; convert unbounded sites opportunistically.
