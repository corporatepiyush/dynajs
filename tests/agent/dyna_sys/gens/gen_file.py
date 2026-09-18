#!/usr/bin/env python3
"""gen_file.py — dyna:file black-box probes. Oracle = POSIX semantics
(a stat's fields, O_APPEND ordering, ELOOP/EEXIST/ENOTDIR errnos)."""
from probe_lib import emit

IMP_FILE = '''import { Path, File, FileReader, FileWriter, FileLock, Glob,
         readFile, writeFile, readDir, makeDir, remove, removeAll, rename,
         copyFile, move, stat, lstat, exists, symlink, readLink, realPath,
         chmod, glob, sniffType, makeTempFile } from "dyna:file";
import { pid } from "dyna:sys";
'''

IMP_ASYNC = '''import { Path, readFile, writeFile, makeDir, removeAll,
         readFileAsync, writeFileAsync, copyFileAsync, asyncStats } from "dyna:file";
import { pid } from "dyna:sys";
'''

IMP_ERR = '''import { Path, File, readFile, writeFile, makeDir, removeAll, exists,
         stat, readDir, symlink, realPath, rename } from "dyna:file";
import { pid } from "dyna:sys";
'''

emit("file", "basic", IMP_FILE, r'''
const D = new Path(Path.cwd(), "scratch", "file_basic-" + pid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const jp = (...n) => new Path(D, ...n);

// write/read/append round-trip
assert_eq(writeFile(jp("a.txt"), "hello"), 5, "writeFile returns byte count");
assert_eq(readFile(jp("a.txt")), "hello", "readFile round-trip");
assert_eq(writeFile(jp("a.txt"), "world", { append: true }), 5, "append write");
assert_eq(readFile(jp("a.txt")), "helloworld", "append landed");
writeFile(jp("a.txt"), "xy");
assert_eq(readFile(jp("a.txt")), "xy", "default write truncates");

// 0-byte file
writeFile(jp("empty"), "");
assert_eq(stat(jp("empty")).size, 0, "0-byte file size");
assert_eq(readFile(jp("empty")), "", "0-byte read is empty string");
assert_true(exists(jp("empty")), "0-byte file exists");

// binary round-trip: all 256 byte values
const bin = new Uint8Array(256);
for (let i = 0; i < 256; i++) bin[i] = i;
writeFile(jp("bin"), bin);
const back = new File(jp("bin")).readBytes();
assert_true(back instanceof Uint8Array, "readBytes returns Uint8Array");
let binOk = back.length === 256;
for (let i = 0; i < 256; i++) if (back[i] !== i) binOk = false;
assert_true(binOk, "binary round-trip all 256 byte values intact");

// stat facts vs POSIX
const st = stat(jp("a.txt"));
assert_eq(st.size, 2, "stat.size");
assert_eq(st.isFile, true, "stat.isFile");
assert_eq(st.isDir, false, "stat.isDir");
assert_eq(st.isSymlink, false, "stat.isSymlink");
assert_true(st.nlink >= 1, "stat.nlink >= 1");
assert_true(st.ino > 0, "stat.ino > 0");
assert_true(st.mtimeMs > 1600000000000, "stat.mtimeMs plausible epoch ms, got " + st.mtimeMs);
assert_true(typeof st.uid === "number" && st.uid >= 0, "stat.uid present");
assert_true(typeof st.mode === "number", "stat.mode number");
assert_eq((st.mode & 0o170000) === 0o100000, true, "S_IFREG bits in mode");

// directories: mkdir -p, readDir, remove vs non-empty
makeDir(jp("deep", "a", "b"), { recursive: true });
writeFile(jp("deep", "a", "b", "leaf.txt"), "L");
assert_throws(() => makeDir(jp("deep2", "x")), null, "mkdir without recursive on missing parent throws");
const ents = readDir(D).map(e => e.name);
assert_true(ents.indexOf("a.txt") >= 0 && ents.indexOf("bin") >= 0 && ents.indexOf("deep") >= 0,
  "readDir lists entries, got " + JSON.stringify(ents));
for (const e of readDir(D)) {
  if (e.name === "deep") { assert_eq(e.isDir, true, "readDir isDir for dir"); assert_eq(e.isFile, false, "readDir not isFile for dir"); }
  if (e.name === "bin") { assert_eq(e.isFile, true, "readDir isFile for file"); }
}
assert_throws(() => remove(jp("deep")), null, "remove on non-empty dir throws");
remove(jp("deep", "a", "b", "leaf.txt"));
assert_eq(exists(jp("deep", "a", "b", "leaf.txt")), false, "remove unlinked leaf");

// rename + move
writeFile(jp("r1"), "R");
rename(jp("r1"), jp("r2"));
assert_eq(exists(jp("r1")), false, "rename removed source");
assert_eq(readFile(jp("r2")), "R", "rename kept content");
move(jp("r2"), jp("r3"));
assert_eq(readFile(jp("r3")), "R", "move kept content");

// copyFile: byte-identical, permission propagation, overwrite refusal
writeFile(jp("csrc"), "COPYME-123");
chmod(jp("csrc"), 0o600);
copyFile(jp("csrc"), jp("cdst"));
const c1 = new File(jp("csrc")).readBytes(), c2 = new File(jp("cdst")).readBytes();
assert_eq(c1.length, c2.length, "copyFile same length");
assert_eq(String(c1), String(c2), "copyFile byte-identical");
assert_eq((stat(jp("cdst")).mode & 0o777), 0o600, "copyFile dst gets src permission bits");
assert_throws(() => copyFile(jp("csrc"), jp("cdst")), null, "copyFile refuses existing dst");
assert_throws(() => copyFile(jp("cdst"), jp("cdst")), null, "copyFile src==dst refused");
copyFile(jp("csrc"), jp("cdst"), { overwrite: true });
assert_eq(readFile(jp("cdst")), "COPYME-123", "copyFile overwrite truncates");
assert_throws(() => copyFile(jp("missing-src"), jp("x")), null, "copyFile missing src throws");

// symlinks: create, readlink verbatim, dangling, stat vs lstat
symlink("a.txt", jp("ln.txt"));
assert_eq(readLink(jp("ln.txt")), "a.txt", "readLink verbatim target");
assert_eq(stat(jp("ln.txt")).isSymlink, false, "stat follows symlink (reports target)");
assert_eq(lstat(jp("ln.txt")).isSymlink, true, "lstat reports symlink");
assert_throws(() => readFile(jp("ln.txt")), null, "readFile through a symlink LEAF is strict-refused");
assert_eq(new File(realPath(jp("ln.txt"))).readText(), "xy", "read via realPath works");
symlink(jp("nowhere-at-all"), jp("dangling"));
assert_eq(exists(jp("dangling")), true, "exists uses lstat: dangling symlink is true");
assert_throws(() => readFile(jp("dangling")), null, "read dangling symlink throws");
assert_throws(() => stat(jp("dangling")), null, "stat dangling symlink throws");

// unicode filenames: NFC and NFD spellings (documented: on APFS the lookup is
// normalization-INSENSITIVE -- python os.listdir+open sees the same folding, so
// each spelling is verified in its own directory where it cannot alias).
makeDir(jp("unfc"), { recursive: true });
writeFile(new Path(jp("unfc"), "caf\u00e9"), "nfc");
assert_eq(readFile(new Path(jp("unfc"), "caf\u00e9")), "nfc", "NFC name round-trip");
makeDir(jp("unfd"), { recursive: true });
writeFile(new Path(jp("unfd"), "cafe\u0301"), "nfd");
assert_eq(readFile(new Path(jp("unfd"), "cafe\u0301")), "nfd", "NFD name round-trip");
assert_eq(readDir(jp("unfc")).length, 1, "NFC dir lists its entry");
assert_eq(readDir(jp("unfd")).length, 1, "NFD dir lists its entry");

// concurrent handles on one file: two appenders via O_APPEND
// flush per write so the syscall (O_APPEND) order, not buffer-drain order,
// is what the assertion sees
const wa = new FileWriter(jp("conc"), { append: true });
const wb = new FileWriter(jp("conc"), { append: true });
wa.write("A1\n"); wa.flush(); wb.write("B1\n"); wb.flush(); wa.write("A2\n"); wa.flush();
wa.close(); wb.close();
const conc = readFile(jp("conc"));
assert_eq(conc.length, 9, "both handles appended, total len 9, got " + conc.length);
assert_true(conc.indexOf("A1") === 0 && conc.indexOf("B1") === 3 && conc.indexOf("A2") === 6,
  "appends landed in write order via O_APPEND");

// FileReader: partial reads, readLine CRLF, EOF semantics
writeFile(jp("lines.txt"), "alpha\nbeta\ngamma\n");
const r = new FileReader(jp("lines.txt"));
assert_eq(r.readLine(), "alpha", "readLine 1");
assert_eq(r.readLine(), "beta", "readLine 2");
assert_eq(r.read(2), "ga", "read(n) partial from current offset");
assert_eq(r.readAll(), "mma\n", "readAll from offset");
r.close();
assert_eq(r.closed, true, "reader closed");
const r2 = new FileReader(jp("lines.txt"));
r2.readLine(); r2.readLine(); r2.readLine();
assert_eq(r2.readLine(), null, "readLine null at clean EOF");
r2.close();
writeFile(jp("crlf.txt"), "w1\r\nw2\r\n");
const r3 = new FileReader(jp("crlf.txt"));
assert_eq(r3.readLine(), "w1", "CRLF handled");
assert_eq(r3.readLine(), "w2", "CRLF line 2");
r3.close();
writeFile(jp("oneline.txt"), "no-newline-at-eof");
const r4 = new FileReader(jp("oneline.txt"));
assert_eq(r4.readLine(), "no-newline-at-eof", "final line without newline");
assert_eq(r4.readLine(), null, "then clean EOF");
r4.close();
const r5 = new FileReader(jp("oneline.txt"));
assert_eq(r5.read(1000).length, 17, "read(n) larger than file returns whole file");
r5.close();

// buffered writer flush + typed arrays are bytes, not decimal text
const w = new FileWriter(jp("out.bin"), { bufferSize: 4 });
w.write(new Uint8Array([1, 2, 255, 0]));
w.write("tail");
w.flush();
const wb2 = new File(jp("out.bin")).readBytes();
assert_eq(wb2.length, 8, "writer produced 8 bytes");
assert_eq(wb2[2], 255, "byte 255 survived (not '255' text)");
w.close();
writeFile(jp("u8.txt"), new Uint8Array([65, 66]));
assert_eq(readFile(jp("u8.txt")), "AB", "writeFile(Uint8Array) writes bytes not text");

// FileLock: withLock releases; closed lock refuses
const lock = new FileLock(jp("lockfile"));
assert_eq(lock.withLock(() => "critical"), "critical", "withLock returns fn value");
assert_eq(lock.closed, true, "withLock consumes the lock");
const lock2 = new FileLock(jp("lockfile2"));
lock2.close();
assert_throws(() => lock2.withLock(() => 1), null, "withLock on closed lock throws");

// glob
writeFile(jp("one.txt"), "1"); writeFile(jp("two.log"), "2");
makeDir(jp("nested"), { recursive: true });
writeFile(jp("nested", "three.txt"), "3");
writeFile(jp("nested", "four.log"), "4");
// 7 top-level *.txt: a, ln(symlink name matches), lines, crlf, oneline, u8, one
assert_eq(glob("**/*.txt", { cwd: D }).length, 8, "glob **/*.txt finds 8 (top 7 + nested/three)");
assert_eq(glob("*.txt", { cwd: D }).length, 7, "glob *.txt top-level only");
assert_eq(glob("on?.txt", { cwd: D }).length, 1, "glob ? matches one char");
writeFile(jp(".hidden.txt"), "h");
assert_eq(glob("*.txt", { cwd: D }).length, 7, "wildcard does not match leading dot");
assert_eq(glob(".*.txt", { cwd: D }).length, 1, "explicit dot pattern matches the dotfile");
assert_eq(glob("nonexistent-zz-*.txt", { cwd: D }).length, 0, "glob no match is empty");

// sniffType by magic bytes
assert_eq(sniffType(new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])), "image/png", "sniff PNG");
assert_eq(sniffType(new Uint8Array([0x1f, 0x8b, 0x08, 0x00])), "application/gzip", "sniff gzip");

// temp files
const tf = makeTempFile("dynasys-");
assert_eq(stat(tf).size, 0, "makeTempFile creates empty file");
remove(tf);

removeAll(D);
assert_eq(exists(D), false, "removeAll removed the tree");
summary("file.basic");
''')

emit("file", "async", IMP_ASYNC, r'''
const D = new Path(Path.cwd(), "scratch", "file_async-" + pid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const f = new Path(D, "t.txt");
writeFile(f, "hello");
(async () => {
  const before = asyncStats();
  assert_eq(await readFileAsync(f), "hello", "readFileAsync string");
  const b = await readFileAsync(f, { bytes: true });
  assert_eq(b.length, 5, "readFileAsync bytes length");
  assert_eq(b[0], 104, "readFileAsync bytes content");
  assert_eq(await writeFileAsync(f, "async", { append: true }), 5, "writeFileAsync resolves byte count");
  assert_eq(readFile(f), "helloasync", "writeFileAsync append landed");
  await copyFileAsync(f, new Path(D, "copy.txt"));
  assert_eq(readFile(new Path(D, "copy.txt")), "helloasync", "copyFileAsync content");
  let rejected = null;
  try { await copyFileAsync(f, new Path(D, "copy.txt")); } catch (e) { rejected = e; }
  assert_true(rejected !== null, "copyFileAsync refuses existing dst without overwrite");
  rejected = null;
  try { await copyFileAsync(new Path(D, "missing"), new Path(D, "x")); } catch (e) { rejected = e; }
  assert_true(rejected !== null, "copyFileAsync missing src rejects");

  // > 1 MiB payload offloads to the pool
  const big = new Uint8Array(1024 * 1024 + 4096);
  for (let i = 0; i < big.length; i++) big[i] = i & 0xff;
  await writeFileAsync(new Path(D, "big.bin"), big);
  const bb = await readFileAsync(new Path(D, "big.bin"), { bytes: true });
  assert_eq(bb.length, big.length, "big async read length");
  let bigOk = true;
  for (let i = 0; i < big.length; i += 4096) if (bb[i] !== (i & 0xff)) { bigOk = false; break; }
  assert_true(bigOk, "big async read content spot-check");
  const after = asyncStats();
  assert_true(after.inline >= before.inline, "inline counter monotonic");
  assert_eq(typeof after.offloaded, "number", "asyncStats.offloaded present");
  assert_eq(after.readMin, 1024 * 1024, "asyncStats.readMin is 1 MiB");

  removeAll(D);
  summary("file.async");
})().catch((e) => { removeAll(D); throw e; });
''')

emit("file", "errors", IMP_ERR, r'''
const D = new Path(Path.cwd(), "scratch", "file_err-" + pid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const jp = (...n) => new Path(D, ...n);

function throws_code(fn, wantCode, msg) {
  let e = null;
  try { fn(); } catch (err) { e = err; }
  if (e === null) { assert(false, msg + " (no throw)"); return; }
  if (wantCode === null) { assert(true, msg); return; }
  assert_eq(e.code, wantCode, msg + " (code " + e.code + ") want " + wantCode);
  assert_eq(typeof e.errno, "number", msg + " (.errno is a number)");
}

throws_code(() => readFile(jp("missing")), "ENOENT", "readFile missing -> ENOENT");
makeDir(jp("adir"), { recursive: true });
{
  // read-leg failures carry .code/.errno too (R2): reading a DIRECTORY is
  // refused by the slurp path with a real errno, never a bare InternalError
  let de = null;
  try { readFile(jp("adir")); } catch (e) { de = e; }
  assert_true(de !== null, "readFile on a directory throws");
  assert_eq(typeof de.code, "string", "readFile(dir) error has .code: " + de.code);
  assert_eq(typeof de.errno, "number", "readFile(dir) error has .errno");
  let be = null;
  try { new File(jp("adir")).readBytes(); } catch (e) { be = e; }
  assert_true(be !== null && typeof be.code === "string", "readBytes(dir) error has .code: " + (be && be.code));
}
throws_code(() => stat(jp("missing")), "ENOENT", "stat missing -> ENOENT");
throws_code(() => readDir(jp("missing")), "ENOENT", "readDir missing -> ENOENT");
throws_code(() => readFile(jp("no", "such", "path")), "ENOENT", "readFile deep missing -> ENOENT");
writeFile(jp("plain.txt"), "x");
throws_code(() => readDir(jp("plain.txt")), "ENOTDIR", "readDir on a FILE throws");
throws_code(() => remove(D), null, "remove non-empty dir throws");

// ELOOP: strict open refuses a symlink swapped in AFTER Path construction
writeFile(jp("real.txt"), "real");
const victimP = jp("victim.txt");              // constructed while absent
symlink("/etc/hostname", victimP);             // swap in a symlink
throws_code(() => writeFile(victimP, "pwned"), "ELOOP",
  "symlink swapped in after construction is refused (leaf)");
makeDir(jp("later"), { recursive: true });
const midP = new Path(jp("later"), "f.txt");   // constructed while 'later' is a real dir
rename(jp("later"), jp("later-real"));
symlink("later-real", jp("later"));            // swap the dir for a symlink
throws_code(() => writeFile(midP, "via"), "ELOOP",
  "symlink swapped in after construction is refused (intermediate)");
// A Path constructed while a symlink EXISTS resolves its prefix through it --
// documented ("resolves the longest existing prefix"), so this must succeed.
symlink("real.txt", jp("link.txt"));
assert_throws(() => readFile(jp("link.txt")), null, "readFile via symlink leaf throws (strict)");
const rp = realPath(jp("link.txt"));
assert_eq(readFile(rp), "real", "realPath resolves the link and read works");

let e1 = null;
try { readFile(jp("missing-2")); } catch (e) { e1 = e; }
assert_true(e1 instanceof Error, "fs errors are Errors");
assert_true(typeof e1.message === "string" && e1.message.length > 0, "error has message");

removeAll(D);
summary("file.errors");
''')

print("gen_file: 3 probes")
