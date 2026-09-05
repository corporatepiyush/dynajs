// dynajs_path.js — the Path value handle in dyna:file.
//
// Path is a VALUE HANDLE, not a compiled capability: it is constructed with the
// data (`new Path("/var/log", name)`) rather than with configuration reused
// across unbounded inputs. That distinction sets what the object is allowed to
// cost. A capability may start behind and win back its construction over N
// uses; a handle has to be as cheap as the string it replaces, more or less
// immediately, because there is nothing to amortise.
//
// So this example is organised around when the handle helps and when it does
// not, and both halves are asserted rather than narrated:
//
//   BEST   — hoisted out of a loop. The path is normalised once at
//            construction and every filesystem call borrows the same bytes.
//   WORST  — reconstructed per iteration. Now you pay the constructor every
//            time AND get nothing back, because nothing is reused. This is the
//            shape a caller falls into by translating string code literally,
//            and it stays in this file permanently for that reason.
//
// Run: dynajs examples/js/dynajs_path.js
import { test, run, assert, assertEqual } from "./harness.js";
import {
  Path, Glob, writeFile, readFile, exists, stat, readDir, glob,
  makeTempDir, removeAll, realPath, makeDir, symlink, readLink,
} from "dyna:file";

// One temp tree for the whole suite, removed at the end even on failure.
const root = makeTempDir("dynajs-path-example-");

// ---------------------------------------------------------------------------
// Construction and the cached splits
// ---------------------------------------------------------------------------

test("a Path normalises once, at construction", () => {
  // Every spelling of the same path collapses to one value, so equality is a
  // byte comparison rather than a re-parse.
  // A prefix that does not exist on any host: construction resolves the
  // longest EXISTING prefix once (so later operations can refuse symlinks
  // strictly), and /var is itself a symlink on macOS -- which would make the
  // expected string host-dependent.
  const messy = new Path("/srv//log/../log/./app.log");
  assertEqual(String(messy), "/srv/log/app.log");
  assert(messy.equals(new Path("/srv/log/app.log")), "two spellings, one value");
});

test("dirname / basename / extname are slices, not scans", () => {
  const p = new Path("/srv/data/report.tar.gz");
  assertEqual(String(p.dirname), "/srv/data");
  assertEqual(p.basename, "report.tar.gz");
  assertEqual(p.extname, ".gz");               // the LAST dot of the final segment
  assertEqual(p.basenameWithout(".gz"), "report.tar");
  assert(p.isAbsolute, "isAbsolute is a cached flag");
});

test("a leading dot makes a dotfile, not an extension", () => {
  // The trap in every hand-rolled extension splitter.
  assertEqual(new Path("/home/me/.bashrc").extname, "");
  assertEqual(new Path("/home/me/.bashrc").basename, ".bashrc");
  assertEqual(new Path("archive.tar.gz").extname, ".gz");
  assertEqual(new Path("..").extname, "");
  assertEqual(new Path("...").extname, ".");
});

test("the splits are splits of the NORMALISED value", () => {
  // This is the one place a Path answers differently from a lexical
  // string function. `new Path("a/..")` IS ".", so its parent is ".".
  // A purely lexical dirname("a/..") would say "a" — of a path that does
  // not exist as written.
  assertEqual(String(new Path("a/..")), ".");
  assertEqual(String(new Path("a/..").dirname), ".");
});

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

test("join appends, resolve anchors", () => {
  const base = new Path("/srv/app");
  assertEqual(String(base.join("logs", "today.txt")), "/srv/app/logs/today.txt");

  // resolve walks right to left and stops at the rightmost absolute segment,
  // so a later absolute path wins outright.
  assertEqual(String(base.resolve("logs", "/etc/passwd")), "/etc/passwd");
  assertEqual(String(base.resolve("../sibling")), "/srv/sibling");
});

test("relativeTo answers 'how do I get there from here'", () => {
  const from = new Path("/a/b/c");
  assertEqual(String(from.relativeTo(new Path("/a/d"))), "../../d");
  // A Path is never empty, so "the same place" is ".", not "".
  assertEqual(String(from.relativeTo(new Path("/a/b/c"))), ".");
});

test("a Path segment composes with a string segment", () => {
  const dir = new Path("/srv");
  assertEqual(String(new Path(dir, "app", "x.txt")), "/srv/app/x.txt");
});

// ---------------------------------------------------------------------------
// Paths in, paths out
// ---------------------------------------------------------------------------

test("every filesystem entry point takes a Path, and a string throws", () => {
  const f = root.join("hello.txt");
  writeFile(f, "hi");
  assertEqual(readFile(f), "hi");

  // There is ONE path surface. Accepting a string as well would be the
  // redundancy the handle exists to remove, so it is a TypeError and the
  // message names the fix.
  let msg = "";
  try { readFile(String(f)); } catch (e) { msg = e.message; }
  assert(msg.includes("must be a Path"), "a string path is refused: " + msg);
});

test("anything that returns a path returns a Path", () => {
  const f = root.join("real.txt");
  writeFile(f, "x");
  assert(Path.isPath(realPath(f)), "realPath");
  assert(Path.isPath(makeTempDir("dynajs-path-ex2-")), "makeTempDir");
  assert(Path.isPath(Path.temp()), "Path.temp");

  makeDir(root.join("g"));
  writeFile(root.join("g", "a.txt"), "a");
  writeFile(root.join("g", "b.txt"), "b");
  const hits = glob("*.txt", { cwd: root.join("g") });
  assert(hits.length === 2 && hits.every(Path.isPath), "glob yields Paths");
  // ...so the result feeds straight back in with no re-wrapping.
  assertEqual(readFile(root.join("g").join(hits[0])), "a");
});

test("a symlink TARGET is a string, deliberately", () => {
  // The target is opaque bytes stored inside the link. It may be relative and
  // may name something that does not exist, so normalising it would silently
  // rewrite the link: symlink("a/../b", l) would store "b". Only the link
  // LOCATION is a Path.
  const link = root.join("lnk");
  symlink("hello.txt", link);
  assertEqual(readLink(link), "hello.txt");
  // Reading THROUGH the link is refused: every operation resolves with
  // O_NOFOLLOW, so a symlink swapped in after the check cannot redirect it.
  // Read the target by resolving it explicitly -- that is the deliberate act.
  let followed = false;
  try { readFile(link); followed = true; } catch (e) { /* refused, as designed */ }
  assertEqual(followed, false);
  assertEqual(readFile(realPath(link)), "hi");
});

// ---------------------------------------------------------------------------
// BEST CASE and WORST CASE, measured against each other
// ---------------------------------------------------------------------------

function timeIt(fn, reps) {
  const t0 = performance.now();
  for (let i = 0; i < reps; i++) fn(i);
  return (performance.now() - t0) * 1000 / reps;   // µs per iteration
}

test("BEST: hoisted — the path is built once for the whole loop", () => {
  // MEASURE THE PATH OPERATION, NOT THE SYSCALL. The first version of this
  // test timed writeFile() in both loops and reported the reconstructed form
  // as 0.62× the hoisted one — i.e. faster, which is impossible. The reason
  // was nothing to do with Path: the first loop CREATED 400 files and the
  // second OVERWROTE them, and creation is far more expensive than
  // overwriting. It was measuring filesystem state.
  //
  // A write is ~50 µs and a path composition is ~1 µs, so the syscall would
  // swamp the difference even with the loops balanced. The honest comparison
  // is the composition alone.
  const dir = root.join("many");
  const REPS = 20000;

  const hoisted = timeIt((i) => { dir.join("f" + i + ".txt"); }, REPS);
  const rebuilt = timeIt((i) => { new Path(String(root), "many", "f" + i + ".txt"); }, REPS);

  print(`  hoisted .join()      ${hoisted.toFixed(3)} µs/path`);
  print(`  rebuilt from root    ${rebuilt.toFixed(3)} µs/path` +
        `   (${(rebuilt / hoisted).toFixed(2)}× the hoisted form)`);

  // MEASURED, over three runs: 1.02-1.05×. Hoisting the directory buys about
  // three percent, not a step change — both forms still normalise a similar
  // number of bytes and allocate one record, so the constructor is not where
  // the cost is. Saying so is the point of measuring it.
  //
  // MEASURED and WRONG the first time. This comment used to claim the
  // handle's real win was at the C boundary -- that the string API had to
  // scan, malloc and copy the path into UTF-8 on every filesystem call, and
  // that a borrowed Path removed it. That was asserted, never measured, and
  // it is FALSE. An A/B against the actual pre-Path binary
  // (tests/bench_path_ab.js) says:
  //
  //      exists    1.527 -> 1.514 us      stat  2.234 -> 2.201 us
  //      readFile 11.99  -> 11.86  us
  //
  // ...i.e. about one percent, inside the noise. The syscall dominates
  // utterly; coercing a 60-byte path is tens of nanoseconds against a
  // microsecond of lstat.
  //
  // So Path is an API-DESIGN change that costs nothing, which is exactly the
  // bar the programme sets ("negligible, not faster"). It is not a speed-up,
  // and claiming a win against a baseline that had already been deleted is
  // the specific error CLAUDE.md sec.14 records.
  //
  // The timing is REPORTED, not asserted. A loose bound was tried -- 0.9x --
  // and it still failed under emulation, where the spread between two forms
  // that differ by a few percent is far wider than any tolerance worth
  // setting. A test that asserts durations fails for reasons that are not
  // bugs; what this example is really claiming is that the two forms are
  // interchangeable, so assert THAT and print the numbers.
  const ratio = rebuilt / hoisted;
  console.log(`  rebuilt/hoisted ${ratio.toFixed(2)}x ` +
              `(${rebuilt.toFixed(3)} vs ${hoisted.toFixed(3)} µs)`);
  assert(String(dir.join("f7.txt")) ===
         String(new Path(String(root), "many", "f7.txt")),
    "hoisting the directory must not change the path it produces");
});

test("...and the filesystem call itself costs the same either way", () => {
  // Which is the point. Hoisting saves path construction; it does not and
  // cannot make a write faster. Both loops here create the same number of
  // fresh files, so the comparison is balanced this time.
  const a = root.join("wa"), b = root.join("wb");
  makeDir(a); makeDir(b);
  const REPS = 300;

  const viaHoisted = timeIt((i) => { writeFile(a.join("f" + i), "x"); }, REPS);
  const viaRebuilt = timeIt(
    (i) => { writeFile(new Path(String(root), "wb", "f" + i), "x"); }, REPS);

  print(`  write via hoisted    ${viaHoisted.toFixed(2)} µs/write`);
  print(`  write via rebuilt    ${viaRebuilt.toFixed(2)} µs/write`);
  assertEqual(readDir(a).length, REPS);
  assertEqual(readDir(b).length, REPS);
  // The syscall dominates, so these land within noise of each other. Assert
  // only that neither is pathological — a tighter bound here would be
  // asserting the machine's disk timing, not the API's cost.
  // Reported, not asserted. Even a 3x bound is a bound on the machine's disk
  // timing, and under emulation it does not hold -- which is how this example
  // took down every amd64 container run.
  assert(viaHoisted > 0 && viaRebuilt > 0,
    "both write paths completed");
});

test("WORST: one path, used once — the handle has nothing to amortise", () => {
  // Being honest about where this shape does NOT help. Constructing a Path to
  // make a single call does strictly more work than the call needs: it
  // normalises, allocates a record, and is then discarded. It is still the
  // right thing to write, because the alternative is not "a cheaper call" but
  // "two path surfaces" — but the cost is real and it belongs here.
  const p = new Path(String(root), "once.txt");
  writeFile(p, "single");
  assertEqual(readFile(p), "single");
  assert(exists(p) && stat(p).size === 6, "the one-shot case still works");
});

test("Glob: compiled once, matched against unbounded paths", () => {
  makeDir(root.join("gl"));
  makeDir(root.join("gl", "sub"));
  writeFile(root.join("gl", "one.txt"), "1");
  writeFile(root.join("gl", "top.js"), "2");
  writeFile(root.join("gl", "sub", "deep.js"), "3");
  const base = root.join("gl");

  const gl = new Glob("*.txt");
  assertEqual(gl.pattern, "*.txt");
  assertEqual(gl.hasWildcard, true);              // computed once, at construction
  // matches() is PURELY LEXICAL -- it never touches the disk, so it works on a
  // path that does not exist. That is what makes it usable as a filter.
  assertEqual(gl.matches(new Path("nowhere.txt")), true);
  assertEqual(gl.matches(new Path("nowhere.js")), false);
  assertEqual(gl.expand(base).map(String).sort().join(","), "one.txt");
  // ** matches ZERO or more directories, which is the whole reason it exists.
  assertEqual(new Glob("**/*.js").expand(base).map(String).sort().join(","),
              "sub/deep.js,top.js");
  assertEqual(new Glob("*.js").expand(base).map(String).join(","), "top.js",
              "* never crosses a separator");
});

test("WORST: Glob has almost nothing to amortise", () => {
  // Compiling a glob is a copy and one wildcard scan, so unlike Range or
  // Compressor there is no expensive configuration parse to hoist. MEASURED at
  // ~1.05x -- it exists for the API, not for speed, and saying so beats
  // implying a win it does not have.
  const paths = [];
  for (let i = 0; i < 200; i++) paths.push(new Path("f" + i + (i % 3 ? ".txt" : ".js")));
  const gl = new Glob("*.txt");
  const t0 = performance.now();
  for (let r = 0; r < 50; r++) gl.filter(paths);
  const hoisted = performance.now() - t0;
  const t1 = performance.now();
  for (let r = 0; r < 50; r++) new Glob("*.txt").filter(paths);
  const perCall = performance.now() - t1;
  print(`  Glob hoisted ${hoisted.toFixed(2)} ms vs per-call ${perCall.toFixed(2)} ms` +
        `  (${(perCall / hoisted).toFixed(2)}x)`);
  // Same: printed above, not gated. A single-use Glob has nothing to
  // amortise, which is the POINT of this case -- the numbers say so without
  // an assertion that fails on a busy or emulated machine.
  assert(hoisted > 0 && perCall > 0, "both Glob paths completed");
});

test("abuse: hostile paths and arguments", () => {
  const throws = (fn) => { try { fn(); return false; } catch { return true; } };
  assert(throws(() => new Path()), "no segments");
  assert(throws(() => new Path({})), "a plain object is not a segment");
  assert(throws(() => new Path(42)), "nor a number");
  assert(throws(() => readFile("/etc/hosts")), "a string path is refused");
  assert(throws(() => new Glob()), "the pattern is required");
  assert(throws(() => new Glob(42)), "a non-string pattern");
  assert(throws(() => new Glob("*").matches("x")), "matches wants a Path");
  assert(throws(() => new Glob("*").filter("nope")), "filter wants an array");

  // Degenerate paths that must normalise rather than crash.
  assertEqual(String(new Path("")), ".");
  assertEqual(String(new Path("/")), "/");
  assertEqual(String(new Path("///")), "/");
  assertEqual(String(new Path("..")), "..");
  assertEqual(String(new Path("/a/../../..")), "/", "excess .. above root is absorbed");
  assertEqual(String(new Path("a/b/../../../../c")), "../../c");
  assertEqual(new Path("/a/b").extname, "", "no extension");
  assertEqual(new Path(".bashrc").extname, "", "a dotfile is not an extension");
  // A very long path must not overflow the sizing.
  const deep = new Path("/" + "seg/".repeat(500) + "f.txt");
  assertEqual(deep.basename, "f.txt");
  assert(String(deep).length > 2000, "long paths survive");
});

test("cleanup", () => {
  removeAll(root);
  assert(!exists(root), "temp tree removed");
});

await run("dyna:file — the Path value handle");
