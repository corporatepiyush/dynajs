// covers: contract dyna:file.* -- T5: every dyna:file entry point that takes a path accepts a string as well as a Path
// T5 (owner 11-26, TRAPS): a string path is converted internally exactly as new Path(s) would be, so f(s) and
// f(new Path(s)) give the same result and the same error; a value that is neither a string nor a Path keeps the TypeError
// it had before. One section per entry point. The leak section runs 20000 string-path calls of each converting kind and
// pins that RSS plateaus (the converted Path is released after every call, on success and on every error exit).
import {
    Path, File, FileReader, FileWriter, FileLock, Watcher, Glob,
    readFile, readBytes, writeFile, stat, lstat, exists, readDir, makeDir, remove, removeAll,
    rename, copyFile, move, sniffType, symlink, readLink, realPath, chmod, glob, makeTempDir,
} from "dyna:file";
import { rusage, Exec, args } from "dyna:sys";
import { sections } from "../lib/kit.js";

const S = sections("test_file_string_paths");
let n = 0, fails = 0;
const ok = (c, m, d) => { n++; if (!c) { fails++; print("  FAIL " + m + (d !== undefined ? "  [" + d + "]" : "")); } };
const show = (v) => v instanceof Path ? "Path(" + String(v) + ")" : v instanceof Uint8Array ? "u8[" + Array.from(v).join(",") + "]"
    : Array.isArray(v) ? "[" + v.map(show).join(",") + "]" : v && typeof v === "object" ? JSON.stringify(v, (k, x) => x instanceof Path ? "Path(" + String(x) + ")" : (k === "atimeMs" || k === "atime") ? 0 : x) : String(v);
function outcome(fn) { try { return "ok " + show(fn()); } catch (e) { return "throw " + (e && e.name) + ": " + (e && e.message); } }
function same(name, fnS, fnP) { const a = outcome(fnS), b = outcome(fnP); ok(a === b, name + ": string and Path agree", a + " vs " + b); return a; }
function refused(name, fn) { let e = null; try { fn(); } catch (x) { e = x; } ok(e instanceof TypeError && /Path/.test(String(e.message)), name + ": a number is a TypeError naming Path", e ? e.name + ": " + e.message : "no throw"); }

const root = makeTempDir("strpaths-");
const R = String(root);
const rel = (s) => R + "/" + s;
writeFile(root.join("a.txt"), "alpha\n");
writeFile(root.join("bin.dat"), new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0]));
makeDir(root.join("d"));
writeFile(root.join("d", "x.js"), "x");

S.section("readFile / readBytes / exists / stat / lstat / readDir", () => {
    ok(readFile(rel("a.txt")) === "alpha\n", "readFile(string) reads the file");
    same("readFile", () => readFile(rel("a.txt")), () => readFile(new Path(rel("a.txt"))));
    same("readFile bytes", () => readFile(rel("a.txt"), { bytes: true }), () => readFile(new Path(rel("a.txt")), { bytes: true }));
    same("readFile missing", () => readFile(rel("nope")), () => readFile(new Path(rel("nope"))));
    same("readFile relative", () => readFile("x/../" + rel("a.txt").slice(1)), () => readFile(new Path("x/../" + rel("a.txt").slice(1))));
    same("readFile NUL", () => readFile("a\u0000b"), () => readFile(new Path("a\u0000b")));
    same("readFile empty", () => readFile(""), () => readFile(new Path("")));
    same("readBytes", () => readBytes(rel("bin.dat")), () => readBytes(new Path(rel("bin.dat"))));
    same("exists", () => exists(rel("a.txt")), () => exists(new Path(rel("a.txt"))));
    same("exists missing", () => exists(rel("zz")), () => exists(new Path(rel("zz"))));
    same("stat size", () => stat(rel("a.txt")).size, () => stat(new Path(rel("a.txt"))).size);
    same("lstat isFile", () => lstat(rel("a.txt")).isFile, () => lstat(new Path(rel("a.txt"))).isFile);
    same("readDir", () => readDir(rel("d")), () => readDir(new Path(rel("d"))));
    for (const [nm, f] of [["readFile", readFile], ["readBytes", readBytes], ["exists", exists], ["stat", stat], ["lstat", lstat], ["readDir", readDir]]) refused(nm, () => f(42));
});

S.section("writeFile / makeDir / remove / removeAll / chmod", () => {
    same("writeFile", () => writeFile(rel("w1.txt"), "hey"), () => writeFile(new Path(rel("w2.txt")), "hey"));
    ok(readFile(rel("w1.txt")) === "hey", "writeFile(string) wrote the file");
    same("writeFile append", () => writeFile(rel("w1.txt"), "!", { append: true }), () => writeFile(new Path(rel("w2.txt")), "!", { append: true }));
    same("writeFile atomic", () => writeFile(rel("w1.txt"), "at", { atomic: true }), () => writeFile(new Path(rel("w2.txt")), "at", { atomic: true }));
    same("makeDir", () => makeDir(rel("m1/x"), { recursive: true }), () => makeDir(new Path(rel("m2/x")), { recursive: true }));
    same("makeDir exists", () => makeDir(rel("m1/x")), () => makeDir(new Path(rel("m1/x"))));
    same("chmod", () => { chmod(rel("w1.txt"), 0o600); return stat(rel("w1.txt")).mode & 0o777; }, () => { chmod(new Path(rel("w2.txt")), 0o600); return stat(new Path(rel("w2.txt"))).mode & 0o777; });
    same("remove", () => remove(rel("w1.txt")), () => remove(new Path(rel("w2.txt"))));
    same("remove missing", () => remove(rel("w1.txt")), () => remove(new Path(rel("w1.txt"))));
    same("removeAll", () => removeAll(rel("m1")), () => removeAll(new Path(rel("m2"))));
    ok(!exists(rel("m1")) && !exists(rel("m2")), "removeAll(string) removed the tree");
    refused("writeFile", () => writeFile(42, "x"));
    refused("makeDir", () => makeDir(42));
    refused("remove", () => remove(42));
    refused("removeAll", () => removeAll(42));
    refused("chmod", () => chmod(42, 0o600));
});

S.section("rename / copyFile / move (both path arguments)", () => {
    writeFile(rel("r1"), "r"); writeFile(rel("r2"), "r");
    same("rename", () => rename(rel("r1"), rel("r1b")), () => rename(new Path(rel("r2")), new Path(rel("r2b"))));
    same("rename mixed", () => rename(new Path(rel("r1b")), rel("r1c")), () => rename(rel("r2b"), new Path(rel("r2c"))));
    same("copyFile", () => copyFile(rel("r1c"), rel("c1")), () => copyFile(new Path(rel("r2c")), new Path(rel("c2"))));
    same("copyFile no overwrite", () => copyFile(rel("r1c"), rel("c1")), () => copyFile(new Path(rel("r1c")), new Path(rel("c1"))));
    same("copyFile overwrite", () => copyFile(rel("r1c"), rel("c1"), { overwrite: true }), () => copyFile(new Path(rel("r2c")), new Path(rel("c2")), { overwrite: true }));
    same("move", () => move(rel("c1"), rel("mv1")), () => move(new Path(rel("c2")), new Path(rel("mv2"))));
    same("move missing", () => move(rel("c1"), rel("mv1")), () => move(new Path(rel("c1")), new Path(rel("mv1"))));
    ok(readFile(rel("mv1")) === "r", "move(string, string) moved the file");
    refused("rename from", () => rename(42, rel("q")));
    refused("rename to", () => rename(rel("mv1"), 42));
    refused("copyFile from", () => copyFile(42, rel("q")));
    refused("copyFile to", () => copyFile(rel("mv1"), 42));
    refused("move from", () => move(42, rel("q")));
    refused("move to", () => move(rel("mv1"), 42));
});

S.section("sniffType / symlink / readLink / realPath / glob cwd", () => {
    same("sniffType", () => sniffType(rel("bin.dat")), () => sniffType(new Path(rel("bin.dat"))));
    ok(sniffType(rel("bin.dat")) === sniffType(readBytes(rel("bin.dat"))), "sniffType(string) reads the file, not the string's bytes");
    same("sniffType missing", () => sniffType(rel("none")), () => sniffType(new Path(rel("none"))));
    let e = null; try { sniffType(42); } catch (x) { e = x; }
    ok(e instanceof TypeError && /Path or bytes/.test(e.message), "sniffType(42) keeps its TypeError", e && e.message);
    same("symlink", () => symlink("a.txt", rel("l1")), () => symlink("a.txt", new Path(rel("l2"))));
    same("symlink exists", () => symlink("a.txt", rel("l1")), () => symlink("a.txt", new Path(rel("l1"))));
    same("readLink", () => readLink(rel("l1")), () => readLink(new Path(rel("l1"))));
    same("realPath", () => realPath(rel("l1")), () => realPath(new Path(rel("l1"))));
    ok(realPath(rel("l1")) instanceof Path, "realPath(string) returns a Path");
    same("glob cwd", () => glob("**/*.js", { cwd: R }).map(String), () => glob("**/*.js", { cwd: new Path(R) }).map(String));
    refused("symlink linkpath", () => symlink("a.txt", 42));
    refused("readLink", () => readLink(42));
    refused("realPath", () => realPath(42));
    refused("glob cwd", () => glob("*", { cwd: 42 }));
});

S.section("File: constructor, moveTo, copyTo", () => {
    writeFile(rel("f1"), "F"); writeFile(rel("f2"), "F");
    const a = new File(rel("f1")), b = new File(new Path(rel("f2")));
    ok(a.path instanceof Path && String(a.path) === rel("f1"), "new File(string).path is a Path");
    same("File.readText", () => a.readText(), () => b.readText());
    same("File.moveTo", () => String(a.moveTo(rel("f1m")).path), () => String(b.moveTo(new Path(rel("f2m"))).path).replace("f2m", "f1m"));
    ok(a.path instanceof Path, "moveTo(string) keeps path a Path");
    same("File.copyTo", () => a.copyTo(rel("f1c")).readText(), () => b.copyTo(new Path(rel("f2c"))).readText());
    ok(a.copyTo(rel("f1d")).path instanceof Path, "copyTo(string) returns a File over a Path");
    same("File.copyTo existing", () => a.copyTo(rel("f1c")), () => b.copyTo(new Path(rel("f1c"))));
    let e = null; try { new File(42); } catch (x) { e = x; }
    ok(e instanceof TypeError && /must be a Path or a string/.test(e.message), "new File(42) keeps its TypeError", e && e.message);
    refused("File.moveTo", () => a.moveTo(42));
    refused("File.copyTo", () => a.copyTo(42));
});

S.section("FileReader / FileWriter / FileLock / Watcher constructors", () => {
    same("FileReader", () => new FileReader(rel("a.txt")).readAll(), () => new FileReader(new Path(rel("a.txt"))).readAll());
    same("FileReader missing", () => new FileReader(rel("none")), () => new FileReader(new Path(rel("none"))));
    same("FileWriter", () => { const w = new FileWriter(rel("fw1")); w.write("w"); w.close(); return readFile(rel("fw1")); },
        () => { const w = new FileWriter(new Path(rel("fw2"))); w.write("w"); w.close(); return readFile(rel("fw2")); });
    same("FileLock", () => { const l = new FileLock(rel("lk1")); l.release(); return 1; }, () => { const l = new FileLock(new Path(rel("lk2"))); l.release(); return 1; });
    same("Watcher", () => { const w = new Watcher(R, { recursive: false }); w.stop(); return 1; }, () => { const w = new Watcher(new Path(R), { recursive: false }); w.stop(); return 1; });
    same("Watcher bad options", () => new Watcher(R, 5), () => new Watcher(new Path(R), 5));
    refused("FileReader", () => new FileReader(42));
    refused("FileWriter", () => new FileWriter(42));
    refused("Watcher", () => new Watcher(42));
    let e = null; try { new FileLock(42); } catch (x) { e = x; }
    ok(e instanceof TypeError && /Path or a string/.test(e.message), "new FileLock(42) keeps its TypeError", e && e.message);
});

S.section("Glob.matches / expand / filter and Path.relativeTo", () => {
    const g = new Glob("**/*.txt");
    same("matches", () => g.matches("a/b.txt"), () => g.matches(new Path("a/b.txt")));
    same("matches normalizes like Path", () => g.matches("a//./b.txt"), () => g.matches(new Path("a//./b.txt")));
    same("expand cwd", () => new Glob("**/*.js").expand(R).map(String), () => new Glob("**/*.js").expand(new Path(R)).map(String));
    const mixed = g.filter(["a/x.txt", new Path("b/y.txt"), "c.js"]);
    ok(mixed.length === 2 && mixed[0] === "a/x.txt" && mixed[1] instanceof Path, "filter keeps the matching elements as given", show(mixed));
    ok(Glob.match("a//b.txt", "a//b.txt") === true, "Glob.match keeps a string verbatim (unchanged)");
    same("relativeTo", () => new Path("/a/b/c").relativeTo("/a/x"), () => new Path("/a/b/c").relativeTo(new Path("/a/x")));
    let e = null; try { new Path("/a").relativeTo(42); } catch (x) { e = x; }
    ok(e instanceof TypeError && /must be a Path/.test(e.message), "relativeTo(42) keeps its TypeError", e && e.message);
    refused("matches", () => g.matches(42));
    refused("filter element", () => g.filter([42]));
    refused("expand cwd", () => g.expand(42));
});

S.section("converted paths are released: RSS plateaus over 20000 string-path calls of each kind", () => {
    const kinds = [() => readFile(rel("a.txt")), () => exists(rel("zz")), () => { try { readFile(rel("none")); } catch (e) { } },
        () => new Glob("*.txt").matches("q.txt"), () => { try { rename(rel("none"), rel("none2")); } catch (e) { } }, () => stat(rel("a.txt"))];
    for (let i = 0; i < 2000; i++) for (const k of kinds) k();
    const r0 = rusage().maxrss;
    for (let i = 0; i < 20000; i++) for (const k of kinds) k();
    const grew = (rusage().maxrss - r0) / 1048576;
    const sanBuild = String(Exec(args()[0], ["-e", "1"], { timeoutMs: 20000, encoding: "utf8", env: { DYNA_TEARDOWN_TRACE: "1" } }).stderr).includes("teardown: precise (build)");
    ok(sanBuild || grew < 4, "RSS growth over 120000 converted-path calls stays under 4 MB (sanitizer builds quarantine frees, so there teardown leak checks stand in)", grew.toFixed(1) + " MB");
});

removeAll(root);
S.end();
if (fails) throw new Error("test_file_string_paths: " + fails + " FAILED of " + n);
print("test_file_string_paths: " + n + " checks passed");
