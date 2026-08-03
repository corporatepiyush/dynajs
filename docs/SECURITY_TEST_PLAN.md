# Adversarial test plan — pen tests and fuzzing for every module and extension method

Status: **plan + in-progress implementation.** Written 2026-08-02 against the measured tree, not
against recollection. Every number below came from a command that is named next to it, so the plan
can be re-derived rather than trusted.

---

## 1. Where we actually are

Three instruments were run to establish the baseline.

**Public API surface** — `dynajs tools/api-inventory.js`. Enumerated from the BINARY
(`getOwnPropertyNames` plus the prototype chain, because native members are non-enumerable and
`Object.keys` reports nothing), at METHOD granularity:

| | |
|---|---|
| `dyna:*` modules in a `CONFIG_NATIVE_MODULES=y` build | **31** |
| public names across modules + builtin extension hosts | **2354** |
| names referenced by no test, example or fuzz source | **97** |

**Dedicated adversarial suites** — `ls tests/ | grep -iE 'pentest|attack|traversal'`:

| Has one | Has none |
|---|---|
| `html`, `net` (incl. HTTP/App), plus `test_static_traversal.js` and `oracle_regexp_fuzz.js` | **the other 27 modules** |

**Fuzz targets** — `ls fuzz/fuzz_*.c`: 12 targets (`eval`, `compile`, `regexp`, `regexp_compile`,
`json`, `bytecode`, `stdlib`, `module_export`, `dyns`, `lz4`, `scram`, `net`).

**The frontier the fuzzers do not reach** — `python3 bench/codegraph.py . 27` ("parses attacker
bytes AND no fuzz target can reach it"), ranked by hazard × complexity. Top rows, which are the
work list for §4:

```
dyn_scram_server_first   src/core/dyn-scram.c:115      cyclo 36, 23 mem, 8 int
dyn_http_perform         src/dyna-http.c:909           cyclo 40
dyn_crypto_jwt_verify    src/dyna-crypto.c:1420        cyclo 39, 9 int
dyn_crypto_jwt_sign      src/dyna-crypto.c:1323
csv_parse                src/dyna-csv.c:96             cyclo 27
sel_compile              src/dyna-html.c:1017          CSS selector compiler
jp_compile               src/dyna-jsonpath.inc.c:555   JSONPath compiler
b32_decode_tab           src/core/dyn-codec.c:193      16 integer ops
pg_column_binary         src/dyna-net-pg.c:804         PostgreSQL wire decode
dyn_trie_keys_with_prefix src/dyna-structures.c:1968   12 int
dyn_app_upload_start     src/dyna-http.c:4050
dyn_app_serve_static     src/dyna-http.c:3608
dyn_fs_sniff_type        src/dyna-filecopy.inc.c:271
df_pack_bits             src/dyna-dataframe.c:1034     10 int
dyn_forest_learn / dyn_tree_build / dyn_kmeans_train   src/dyna-ml.c
```

Q27's own caveat applies and is not decoration: **reachability is static and blind through function
pointers**, so anything reached only via a callback is listed wrongly. Cross-check before writing a
target. `dyn_scram_server_first` topping the list while `fuzz_scram` exists is exactly this shape and
is the first thing to check, not the first thing to fix.

---

## 2. The instrument comes before the findings

The first draft of this section asserted, from memory, that the fuzz targets link libFuzzer but not
ASan. **That was checked and it is false** — the claim was true historically and has since been
fixed. It is recorded here rather than quietly deleted, because the plan was nearly built on it.

Measured state instead:

- `FUZZ_DEFAULT_SAN = -fsanitize=address,undefined -fno-omit-frame-pointer` is in the committed
  Makefile. ASan is live at **runtime**, not merely linked: `ASAN_OPTIONS=verbosity=1
  ./fuzz_bytecode -runs=200` prints the shadow map and `redzone=16`.
- **`CONFIG_ASAN=y` is strictly WEAKER than a plain `make` here.** An `ifndef CONFIG_ASAN` guard
  blanks `FUZZ_DEFAULT_SAN`, so the flag buys `address` and loses `undefined`. Measured:
  `fuzz_stdlib` built that way has 14234 asan symbols and **0** `__ubsan_handle_*`, against 9 for
  every default-built target. `fuzz/README` currently recommends the weaker command — fix the README.

The gaps that are real:

- **`fuzz/fuzz_regexp_compile.c` has no Makefile rule at all — never built, never executed.** `VPATH`
  excludes `fuzz/`, so make's built-in `%: %.c` cannot reach it either. **There are 11 targets, not
  12.** It contains an uninitialised `char valid_flags[16]` that `strchr` scans on the first
  iteration, and it sets no memory limit and no interrupt handler, so gating it as-is buys a hang.
  Fix the source before adding the rule.
- **`make fuzz-audit` cannot catch that.** It diffs rules → `FUZZ_TARGETS` in one direction, so a
  source file with no rule is invisible to it. Make it bidirectional and prove it fires.
- **`fuzz_net.c:154` hides its own bug class.** `char srv[DYN_SCRAM_MAX_MSG]` with the input capped
  *below* the buffer puts an overread in spare capacity. `fuzz_scram.c` parses the same thing with an
  exact `malloc` and gets it right — same code, two targets, one blind.
- **The corpora predate the sanitizer binaries and have never been re-run under them.**
  `corpus_robots` is 435 files; `corpus_ml`'s 413 files begin with the `DYNS` magic, so despite the
  name it seeds `fuzz_dyns`. **Nine of the eleven targets start cold.**

Under ASan, execution is 2–3× slower — budget by `-runs=`, not `-max_total_time`, when comparing —
and the 256 MB quarantine means **RSS is not a leak instrument** in these builds.

One correction to a hazard we assumed: **`CONFIG_SIG` does not fire for fuzz goals.** `CFG_CHECK`
filters `MAKECMDGOALS` against `all $(PROGS) libdynajs.a`, so building a fuzz target will not wipe
objects or disturb a `CONFIG_NATIVE_MODULES=y` `./dynajs`.

Two further ways a target can be structurally unable to find its own bug class, both to be audited
per target:

- **A fixed-size input buffer hides the class.** `uint8_t buf[4096]` puts a one-past-the-end read
  inside spare capacity, so a removed bounds check still reports clean forever. A target must
  allocate the input's EXACT length and hand that pointer in. This applies to every scratch buffer
  the target builds, not only the primary one.
- **A cold start is not a null result.** A fault-injection run that begins with no corpus can spend
  its whole budget failing to reach the code, which reads as "the target does not catch it". Seed
  fault-injection runs from the corpus saved by the clean run.

**Verification rule for this whole plan:** a check is worthless until it has been seen to fail.
Every new target and every new pen-test case must be proved by injecting the fault it exists to
catch and watching it fire — with the injected read assigned through a `volatile` sink so the
optimiser cannot delete it, and with the injection asserted to have actually landed before any
"not caught" conclusion is believed.

---

## 3. Attack classes, and which modules each applies to

The per-module work in §4 is the cross-product of this table with the module list. A module's plan
is "for each class that applies, one case that would fail if the defence were removed".

| # | Class | Instrument | Applies to |
|---|---|---|---|
| A | Memory safety — OOB read/write, UAF, double free | ASan | every native module |
| B | Integer overflow / length confusion | UBSan + explicit cases | every parser; `b32_*`, `df_pack_bits`, `dyn_trie_*` scored highest |
| C | Resource exhaustion — bombs, quadratic blowup, unbounded recursion | wall-clock + CPU timeout | `yaml` (anchors), `xml` (entities), `html`, `json`, `decimal`, `structures`, `regexp` |
| D | Injection — formula, header, path, SQL, log | value-level assertions | `csv` (formula), `net` (headers/smuggling), `file` (traversal), `log`, `net-pg`/`net-sqlite` |
| E | Reentrancy — user code runs during argument coercion or in a callback | attack tests | every native class with a handle; the rule is *coerce all arguments to C locals first, then resolve the native handle* |
| F | Type/shape confusion — wrong dtype, detached buffer, hostile length | explicit cases | `dataframe`, `ml`, `simd`, `bytes`, `structures` |
| G | Protocol confusion — reply/request desync, forged identity | mock peer | `net` (Redis, PG, DNS, HTTP client) |

Two rules from this codebase's own history are load-bearing here and are not general advice:

- **A mock peer is the stronger oracle for a protocol client, not the weaker one.** The replies that
  decide correctness are the ones a *correct* server never sends. The real service cannot be made to
  produce them.
- **A security test that passes may never have reached the sink.** When an attack case goes green,
  establish *why* it is safe. If the dangerous bytes stopped short — a delimiter ended the field, a
  length cap fired, the parse failed earlier — keep that case AND add one where they arrive intact.

---

## 4. The work list, in priority order

Priority is attack surface × current coverage, not module size.

### Tier 0 — fix the instrument
1. Establish which targets carry ASan today; prove the gap by injection.
2. Make ASan the default for fuzz targets (or add a `fuzz-all` target that builds them correctly).
3. Audit all 12 targets for the fixed-size-buffer defect; convert to exact-length allocation.
4. Re-run every existing corpus under ASan and triage what falls out.

### Tier 1 — parsers of untrusted bytes, currently with zero adversarial tests
Each gets a `tests/test_<mod>_pentest.js` **and** a fuzz target where the parser is reachable from C.

| Module | The cases that matter |
|---|---|
| `yaml` | alias/anchor expansion bomb (billion laughs), deep nesting → stack, duplicate keys, `__proto__` as a key, huge scalars |
| `xml` | entity expansion, external entity (XXE) refusal, deep nesting, unclosed tags, encoding confusion |
| `csv` | **formula injection** (`=`, `+`, `-`, `@`, tab/CR leading), embedded quotes/newlines, ragged rows, huge single field, `csv_parse` (Q27) |
| `decimal` | precision/scale overflow, huge exponents, `NaN`/`Infinity` strings, division edge cases, round-trip |
| `url` | scheme confusion, userinfo/host splitting, percent-decoding, IDN/unicode, path traversal in the path |
| `semver` | range grammar blowup, huge version parts, prerelease ordering |
| `encoding` | `b32_decode_tab`/`b32_encode_tab` (Q27, 16 integer ops), base64 padding, hex odd length, UTF-8 boundary and surrogates |
| `validate`, `config`, `serialize` | schema recursion, hostile shapes; `validate` has **0** adversarial references today |
| `crypto` | `dyn_crypto_jwt_verify`/`_sign` (Q27) — **`alg: none`, algorithm confusion (HS256 signed with an RS256 public key), tampered segments, unpadded base64url** |

### Tier 2 — protocol and filesystem (remote or semi-trusted attacker)
| Area | The cases that matter |
|---|---|
| `net` — **CLOSED** | Slowloris (CWE-400) is fixed and now ASSERTED, not merely reported. One repeating sweep for all connections; the idle clock is stamped on protocol PROGRESS (bytes consumed), never on byte arrival — stamping in the read callback would make the attacker look permanently active while the defence appeared implemented. Proved failable: `idleTimeoutMs: 0` holds the dribbling connection the full 8 s and the case fails. |
| `net` — PG/Redis/DNS | mock peer sending what a correct server never sends: reply to a command nobody issued, declared length no message could hold, TLS record on a plaintext port, forged DNS reply with the right ID and wrong everything else |
| `net` — `pg_column_binary` | Q27; hostile column lengths and type oids |
| `file` | traversal is fixed for `App.static` (realpath containment); sweep the rest — `dyn_fs_sniff_type` (Q27), symlink races, `File.chmod` (referenced by nothing) |
| `scrape` | robots parsing, redirect loops, decompression bombs |

### Tier 3 — memory safety on hostile shapes
`dataframe` (`df_pack_bits`), `ml` (`dyn_forest_learn`, `dyn_tree_build`, `dyn_kmeans_train` — model
files are untrusted input), `simd`, `structures` (`dyn_trie_keys_with_prefix`), `bytes`.

Known and unfixed, from memory and **not to be rediscovered**: the shared SIMD kernel table has a
heap-OOB read in `max/min/argmax/argmin` for `0 < n < vector-width` on every accelerated ISA, plus
NEON `silu` double-negation and inverted `topk_indices`. These are worked around in the `dyna:simd`
binding but remain wrong for any other caller.

### Tier 4 — extension methods on builtins
`String`, `Array`, `Object`, `Number`, `Date`, `Math`, `RegExp`, `Map`, `Set`. The 97 unreferenced
names concentrate here; after excluding ES-standard names (which test262 covers, detected from its
directory layout) the real gaps are the project's own additions — the `Date.prototype`
`millisecondsSince`/`secondsAgo`/`addMilliseconds` family and `isTuesday`-style predicates, and
`dyna:mathx` `bits.*` 16/8-bit helpers.

Hostile inputs for this tier: lone surrogates, huge lengths, negative and fractional indices,
`Proxy` receivers, getters that mutate or detach during iteration, frozen targets.

---

## 5. Build integration

| Target | Does |
|---|---|
| **`make prepush`** | **the mandatory gate — everything below, in one target** |
| `make install-hooks` | installs `prepush` as `.git/hooks/pre-push` |
| `make test-api` | the five API layers (see §5.1) |
| `make test-repl` | the pty REPL harness (**landed**, 24 cases, ~24 s) |
| `make test-security` | every `tests/test_*_pentest.js` + traversal + reentrancy suites |
| `make fuzz-all` | build all 12+ targets **with ASan**, fail if any lacks it |
| `make fuzz-smoke` | short bounded run of every target over its seed corpus — the CI-affordable form |
| `make fuzz-verify` | the injection proof: plant a fault, assert the target fires, revert |

`prepush` **builds its own binary from `clean` with `CONFIG_NATIVE_MODULES=y`** before running
anything. That is not caution: half these suites SKIP their `dyna:*` sections against a
default-config binary, and a run of all-skips prints zero failures. It then probes that the binary
can actually load `dyna:mathx` and fails if it cannot, so "the gate was green" cannot mean "the gate
tested nothing".

The hook fails **closed** — it does not exit 0 when it cannot run, because a hook that passes when
broken is worse than no hook. `git push --no-verify` is the deliberate override. `make install-hooks`
refuses to overwrite a `pre-push` it did not write (proved by injecting a foreign hook), and is
idempotent (proved by running it twice: the upgrade path is a different program from the install path
and is the one that ships unrun). `make test` prints a NOTICE when the hook is absent, because a
gate nobody installed is a gate nobody runs.

### 5.1 The five API layers, weakest oracle first

Each answers a question the one before it cannot. The order is also the failure-reporting order: a
name that is not even *total* should be reported before one whose digits are wrong.

| Layer | File | Oracle | What it can prove |
|---|---|---|---|
| surface | `test_api_surface.js` | none | every name is **total** — no crash, no hang, on a fixed hostile matrix |
| params | `test_api_params.js` | hand tables | the values a human sat down and wrote out |
| differential | `test_api_differential.js` | naive JS re-implementation | the native path agrees with an obvious one |
| round trip | `test_api_roundtrip.js` | the inverse operation | a pair is self-consistent; identities and algebra hold |
| **vectors** | `test_api_vectors.js` | **a standards document** | **the definition itself is right** |

The last layer exists because the four above it are all blind in the same direction: if the encoder
and the decoder both implement the wrong standard, every round trip passes, and if the native path
and the naive reference share a misreading, the differential agrees. Only an external authority
settles that. Sources currently pinned: FIPS 180-4 and 202, RFC 1321, RFC 2202/4231, RFC 4648 §10,
RFC 4122 appendix C, RFC 9562 §5.10, RFC 6238 appendix B, the ITU-T V.42 CRC check value, RFC 7693
appendix A, DLMF/Abramowitz–Stegun tables 7.1 and 9.1, and semver.org item 11.4.

**Rule for adding a vector: cite the document in the comment.** A vector with no provenance is a
value somebody recorded from this engine, which freezes today's behaviour including its bugs —
precisely what the layer exists to prevent. If it cannot be cited, it belongs in one of the other
four files as a property.

`test_api_fuzz.js` runs last and separately: it is seeded and randomised, so it is the only suite
whose failure set changes between runs, and its report is a shrunk reproducer rather than a case name.
Every suite that needs adversarial input draws it from `tests/fuzzgen.js` — **data generation only**,
no assertions and no timing, so the same corpus feeds the pen tests and the parametric tests without
either one's expectations leaking into the generator.

Rules for these targets, each paid for by a specific failure in this repo:

- **No `dynajs` prerequisite on a test target.** It rebuilds in the DEFAULT configuration and
  silently replaces a `CONFIG_NATIVE_MODULES=y` binary; the tests still pass, which is what makes it
  silent. Probe the binary and print how to build it instead. (This exact trap was caught in
  `test-repl` before it shipped.)
- **Every case needs both a CPU-time and a wall-clock bound.** A spin burns CPU so a CPU limit
  catches it; a process blocked on a socket burns none and only wall clock catches it. Report a
  timeout DISTINCTLY from a failure.
- **A skip must print itself.** A missing `python3`, a missing pty, an absent optional dependency
  must say so loudly — a lower case count with zero failures reads as green.
- **Artifacts:** every fuzz invocation gets `-artifact_prefix=` outside the repo. A fuzzer writes a
  reproducer on every finding, including for deliberately injected faults, and `git add -A` followed
  by a "tree is clean" check is self-confirming and would commit them.

---

## 6. Explicit non-goals

- **Not** chasing the 97 unreferenced names to zero. Most are ES-standard names covered by test262;
  a name-existence grep over an API is mostly noise and the noise has a shape. The executable checks
  are what find defects.
- **Not** unifying the sync and async HTTP pumps to share test scaffolding. They differ in control
  flow, and this codebase's rule is that code differing in control flow does not get unified.
- **Not** adding these to `make test`. The pen suites spawn processes and sockets; they belong in
  `test-security` so that a slow or flaky case cannot be mistaken for a hung default build.

---

## 7. Resolved: `Money.add()` returned JS_EXCEPTION without throwing

`tests/test_api_fuzz.js` aborted with `ReferenceError: threw is not initialized`
on a binding that is provably initialized — and the name it accused belonged to
the CALLER, not to any frame the fault was in.

The cause was one line in `dyn_money_op` (`src/dyna-decimal.c`):

```c
if (argc < 1 || money_pair(ctx, this_val, argv[0], &a, &b) < 0)
    return JS_EXCEPTION;
```

`argc < 1` short-circuits, so `argv[0]` was never read out of bounds — but the
function returns `JS_EXCEPTION` **without calling `JS_Throw*`**. `JS_EXCEPTION`
only asserts that an exception is *pending*; with none raised, the engine
propagates the uninitialized sentinel. That value is not an object (`e.constructor`
is `undefined`), and when the interpreter touches it, the diagnostic it prints
names whichever local it landed next to.

**Why this is worth a section.** The symptom points at the wrong file, the wrong
language feature, and the wrong frame. It reads as a TDZ bug in the engine's
`let` handling — a known-weak fast path here, which makes the wrong hypothesis
the plausible one. Only a bisect down to `m.add()` with **zero arguments** named
it, and the fix is `JS_ThrowTypeError` before the short-circuit.

A sweep for the same shape (`argc < N ||` followed by a bare `return JS_EXCEPTION`
with no throw on either line) found this as the **only** occurrence in the tree.
Worth re-running after any new native method lands; it is four lines of Python
and the failure mode is this misleading.

**Rule:** `return JS_EXCEPTION` is a claim that an exception is already pending.
Every path that returns it must either have thrown, or be propagating a callee
that did. A guard clause that refuses an argument has thrown nothing yet.

---

## 8. Outstanding: the corpus reaches four suites, not ten

`make prepush` stage 3 (`tools/check-unused-imports.py`) exists because eight of
ten suites imported the shared corpus and used **none** of it. The import line
was there; the data reached no assertion. Nothing else in the gate can see this
— these are scripts, so no compiler warns, no linter runs, and the suite's own
pass count is identical either way. It survived a full session of review.

Those imports have been **removed**, not quietly left in place, because an
import that claims wiring it does not have is worse than an absent one: it reads
as coverage to the next person. The check is now green and enforced, so the
class cannot come back silently.

**What that means for coverage, stated plainly.** The 516-string corpus is
genuinely driven by four files:

| Suite | Uses the corpus |
|---|---|
| `test_api_roundtrip.js` | yes — codec pairs over 552 encodable strings |
| `test_api_differential.js` | yes |
| `test_api_fuzz.js` | yes — `STRINGS` via the generator |
| `test_http_params.js` | yes — 56 corpus cases added |
| `test_api_params.js`, `test_api_surface.js` | **no** — own fixed matrices |
| the four `*_pentest.js` suites | **no** — own hand-written payloads |

The four pen suites are the ones that most want it, and wiring them is real
remaining work, not a formality: each has its own assertion helper and its own
notion of a passing case, so the corpus has to be fed to the module entry points
that suite already drives rather than bolted on.

Two further gaps found in the same pass and NOT fixed:

- **70 of `test_module_pentest.js`'s 371 assertions are survival-only** — they
  assert "it did not crash", which says nothing about whether the defence ran.
  A payload neutralised by an unrelated length cap passes identically.
- **No pen-suite refusal has been proved to fail for the reason it claims.**
  That needs per-guard injection, one guard at a time.
