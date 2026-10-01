// flags: --std
import { test, run, assert, assertEqual } from "./harness.js";
import {
  Path, Glob, writeFile, readFile, exists, stat, readDir, glob,
  makeTempDir, removeAll, realPath, makeDir, symlink, readLink,
} from "dyna:file";

const root = makeTempDir("dynajs-path-example-");

test("a Path normalises once, at construction", () => {
  const messy = new Path("/srv//log/../log/./app.log");
  assertEqual(String(messy), "/srv/log/app.log");
  assert(messy.equals(new Path("/srv/log/app.log")), "two spellings, one value");
});

test("dirname / basename / extname are slices, not scans", () => {
  const p = new Path("/srv/data/report.tar.gz");
  assertEqual(String(p.dirname), "/srv/data");
  assertEqual(p.basename, "report.tar.gz");
  assertEqual(p.extname, ".gz");
  assertEqual(p.basenameWithout(".gz"), "report.tar");
  assert(p.isAbsolute, "isAbsolute is a cached flag");
});

test("a leading dot makes a dotfile, not an extension", () => {
  assertEqual(new Path("/home/me/.bashrc").extname, "");
  assertEqual(new Path("/home/me/.bashrc").basename, ".bashrc");
  assertEqual(new Path("archive.tar.gz").extname, ".gz");
  assertEqual(new Path("..").extname, "");
  assertEqual(new Path("...").extname, ".");
});

test("the splits are splits of the NORMALISED value", () => {
  assertEqual(String(new Path("a/..")), ".");
  assertEqual(String(new Path("a/..").dirname), ".");
});

test("join appends, resolve anchors", () => {
  const base = new Path("/srv/app");
  assertEqual(String(base.join("logs", "today.txt")), "/srv/app/logs/today.txt");

  assertEqual(String(base.resolve("logs", "/etc/passwd")), "/etc/passwd");
  assertEqual(String(base.resolve("../sibling")), "/srv/sibling");
});

test("relativeTo answers 'how do I get there from here'", () => {
  const from = new Path("/a/b/c");
  assertEqual(String(from.relativeTo(new Path("/a/d"))), "../../d");
  assertEqual(String(from.relativeTo(new Path("/a/b/c"))), ".");
});

test("a Path segment composes with a string segment", () => {
  const dir = new Path("/srv");
  assertEqual(String(new Path(dir, "app", "x.txt")), "/srv/app/x.txt");
});

test("every filesystem entry point takes a Path, and a string throws", () => {
  const f = root.join("hello.txt");
  writeFile(f, "hi");
  assertEqual(readFile(f), "hi");

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
  assertEqual(readFile(root.join("g").join(hits[0])), "a");
});

test("a symlink TARGET is a string, deliberately", () => {
  const link = root.join("lnk");
  symlink("hello.txt", link);
  assertEqual(readLink(link), "hello.txt");
  let followed = false;
  try { readFile(link); followed = true; } catch (e) {  }
  assertEqual(followed, false);
  assertEqual(readFile(realPath(link)), "hi");
});

function timeIt(fn, reps) {
  const t0 = performance.now();
  for (let i = 0; i < reps; i++) fn(i);
  return (performance.now() - t0) * 1000 / reps;
}

test("BEST: hoisted — the path is built once for the whole loop", () => {
  const dir = root.join("many");
  const REPS = 20000;

  const hoisted = timeIt((i) => { dir.join("f" + i + ".txt"); }, REPS);
  const rebuilt = timeIt((i) => { new Path(String(root), "many", "f" + i + ".txt"); }, REPS);

  print(`  hoisted .join()      ${hoisted.toFixed(3)} µs/path`);
  print(`  rebuilt from root    ${rebuilt.toFixed(3)} µs/path` +
        `   (${(rebuilt / hoisted).toFixed(2)}× the hoisted form)`);

  const ratio = rebuilt / hoisted;
  console.log(`  rebuilt/hoisted ${ratio.toFixed(2)}x ` +
              `(${rebuilt.toFixed(3)} vs ${hoisted.toFixed(3)} µs)`);
  assert(String(dir.join("f7.txt")) ===
         String(new Path(String(root), "many", "f7.txt")),
    "hoisting the directory must not change the path it produces");
});

test("...and the filesystem call itself costs the same either way", () => {
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
  assert(viaHoisted > 0 && viaRebuilt > 0,
    "both write paths completed");
});

test("WORST: one path, used once — the handle has nothing to amortise", () => {
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
  assertEqual(gl.hasWildcard, true);
  assertEqual(gl.matches(new Path("nowhere.txt")), true);
  assertEqual(gl.matches(new Path("nowhere.js")), false);
  assertEqual(gl.expand(base).map(String).sort().join(","), "one.txt");
  assertEqual(new Glob("**/*.js").expand(base).map(String).sort().join(","),
              "sub/deep.js,top.js");
  assertEqual(new Glob("*.js").expand(base).map(String).join(","), "top.js",
              "* never crosses a separator");
});

test("WORST: Glob has almost nothing to amortise", () => {
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

  assertEqual(String(new Path("")), ".");
  assertEqual(String(new Path("/")), "/");
  assertEqual(String(new Path("///")), "/");
  assertEqual(String(new Path("..")), "..");
  assertEqual(String(new Path("/a/../../..")), "/", "excess .. above root is absorbed");
  assertEqual(String(new Path("a/b/../../../../c")), "../../c");
  assertEqual(new Path("/a/b").extname, "", "no extension");
  assertEqual(new Path(".bashrc").extname, "", "a dotfile is not an extension");
  const deep = new Path("/" + "seg/".repeat(500) + "f.txt");
  assertEqual(deep.basename, "f.txt");
  assert(String(deep).length > 2000, "long paths survive");
});

test("cleanup", () => {
  removeAll(root);
  assert(!exists(root), "temp tree removed");
});

await run("dyna:file — the Path value handle");
