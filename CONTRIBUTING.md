# Contributing

## The gate is the contract

```bash
make install-hooks    # once, per clone -- a clone has NO gate until you run this
```

That installs `make prepush` as a fail-closed `pre-push` hook: codegraph, a clean
`CONFIG_NATIVE_MODULES=y` build, TLS, fuzz-audit + link, import checks, `test`,
`test-native`, `test-api`, `test-security`, `test-repl`. It fails closed on purpose. To
push past a known-red gate, and only then: `git push --no-verify`.

`./dev.sh gate` is the fuller fan-out (sanitizer configurations in parallel, TSan on the
threaded HTTP cases). Run it before anything you would call a release.

## Before you open a change

1. **Run the code graph.** `python3 bench/codegraph.py .` — it parses source, needs no
   binary, and takes ~15s. It sees things no compiler reports: a struct that regained
   padding, a function whose complexity doubled, a hand-written link line your new
   dependency just broke. The first positional argument is the ROOT, so it is
   `codegraph.py .`, never `codegraph.py 27`.
2. **Make ALL your edits first, then build once.** A compile error in file three does not
   stop you writing file four, and finding four of them in one pass costs one build.
3. **Never edit source while a gate runs.** It rebuilds configurations underneath you and
   the symptom surfaces in a file you did not touch. Use a snapshot if you want to keep
   working — `dev.sh` builds in `/tmp/build$$`, not in the tree.

## Rules that are not style preferences

- **Comments are 3 lines maximum.** A comment carries a constraint or a non-obvious fact.
  Measurements, A/B numbers and "I tried X and it regressed" belong in the commit message.
- **Commit messages are 50 words maximum**, including the subject. State what changed and
  the one fact that justifies it.
- **Prove a check can fail.** Inject the fault it exists to catch and watch it fire. A test
  that has only ever been seen passing has told you nothing. Several checks in this
  codebase were found to be incapable of failing.
- **An optimisation is guilty until a benchmark proves it innocent**, with a CONTROL that
  must not move. Run the whole suite, not your micro-benchmark: a 20x micro win has
  repeatedly meant a third of the suite got slower.
- **Every commit builds on its own.** A change that deletes a source file and leaves its
  object in the build list is a broken bisect point even if the final tree is fine.
- **No AI attribution in commits.** Commits carry the repo's configured git identity only.

## Testing

Adversarial data lives in `tests/fuzzgen.js`; assertions live in the suite. The generator
never asserts, times or prints, so a pen test, a parametric test and a benchmark can share
one corpus.

Layers, and what each is worth:

- **surface / fuzz** — drives ~1193 of 1195 exported names. Proves nothing hangs or
  corrupts the heap. **This is not coverage.**
- **params / vectors** — asserts VALUES. ~19% of names. This is the layer that says an
  answer is RIGHT, and an expected value must come from OUTSIDE this engine: a published
  test vector, an independent implementation, or the specification. An uncited "expected"
  is a number recorded from the code under test, which freezes today's bugs.
- **pen tests** — assert REFUSAL. Establish *why* a green case is safe: if the dangerous
  bytes stopped short of the sink, keep that case and add one where they arrive intact.

## Platforms

macOS and Linux (glibc and musl), arm64 and x86-64. `docker/linux.sh [--amd64] <cmd>` runs
anything against this tree in a Linux container in ~2s of overhead. Under `--amd64` you are
in qemu: treat timings as meaningless (~35x inflation) and use it for compatibility, SHA
differentials and memory only.

Code for an ISA you cannot execute is **unrun, not tested**.

## Reporting security issues

See [SECURITY.md](SECURITY.md). Do not open a public issue for a suspected vulnerability.
