import { Path, File, FileReader, FileWriter, FileLock, readFile, writeFile, readDir, makeDir, remove, removeAll, rename,
         copyFile, move, stat, lstat, exists, symlink, readLink, realPath,
         chmod, glob, sniffType, makeTempFile } from "dyna:file";
import { pid } from "dyna:sys";
// h.js — portable assert harness for dyna_sys black-box probes (dynajs only;
// the dyna:* modules do not exist under node). Pattern follows
// tests/agent/strnum_bb/h.js: per-failure FAIL lines, SUMMARY line, then
// RESULT PASS / RESULT FAIL; any failure throws uncaught so the process exit
// code is nonzero.
var __out = (typeof print === "function") ? function (s) { print(s); }
                                          : function (s) { console.log(s); };
var __pass = 0, __fail = 0;
var __asyncPending = 0;
var __asyncFailed = false;

function __show(v) {
  var t = typeof v;
  if (t === "string") return JSON.stringify(v.length > 120 ? v.slice(0, 117) + "..." : v);
  if (t === "number") { if (v === 0 && 1 / v < 0) return "-0"; return String(v); }
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  if (t === "boolean") return String(v);
  if (t === "function") return "fn";
  if (t === "bigint") return v + "n";
  if (v instanceof Error) return v.name + ": " + v.message;
  try { var j = JSON.stringify(v); if (j && j.length > 200) j = j.slice(0, 197) + "..."; return j; } catch (e) { return "[unprintable]"; }
}

function __failLine(kind, msg, extra) {
  __fail++;
  __asyncFailed = true;
  __out("FAIL " + kind + " " + msg + (extra !== undefined ? " " + extra : ""));
}

function assert(cond, msg) {
  if (cond) { __pass++; return; }
  __failLine("assert", msg, "cond=" + __show(cond));
}
function assert_true(v, msg) {
  if (v === true) { __pass++; return; }
  __failLine("assert_true", msg, "got=" + __show(v));
}
function assert_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __failLine("assert_eq", msg, "got=" + a + " want=" + e);
}
function assert_ne(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (!(a === e && typeof actual === typeof expected)) { __pass++; return; }
  __failLine("assert_ne", msg, "both=" + a);
}
// async variant: assertion evaluated in a promise callback; counts immediately
function assert_a(cond, msg) {
  if (cond) { __pass++; return; }
  __failLine("assert", msg);
}
function assert_a_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __failLine("assert_eq", msg, "got=" + a + " want=" + e);
}
function assert_throws(fn, kind, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __failLine("assert_throws", msg, "no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  if (kind && name !== kind) { __failLine("assert_throws", msg, "threw=" + name + " want=" + kind); return; }
  __pass++;
}
function assert_throws_msg(fn, kind, msgSubstr, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __failLine("assert_throws_msg", msg, "no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  var m = (threw && typeof threw.message === "string") ? threw.message : "";
  if (kind && name !== kind) { __failLine("assert_throws_msg", msg, "threw=" + name + " want=" + kind); return; }
  if (msgSubstr && m.indexOf(msgSubstr) < 0) { __failLine("assert_throws_msg", msg, "msg=" + __show(m) + " want~" + msgSubstr); return; }
  __pass++;
}
// promise-rejection variant: p.then(...) style; checker receives error
function assert_rejects(p, kind, msg, checkMsgSubstr) {
  __asyncPending++;
  p.then(function (v) {
      __failLine("assert_rejects", msg, "resolved with " + __show(v));
      __asyncPending--;
      __maybeDone();
    }, function (e) {
      var name = (e && typeof e.name === "string") ? e.name : "?";
      var m = (e && typeof e.message === "string") ? e.message : "";
      if (kind && name !== kind) __failLine("assert_rejects", msg, "threw=" + name + " want=" + kind);
      else if (checkMsgSubstr && m.indexOf(checkMsgSubstr) < 0) __failLine("assert_rejects", msg, "msg=" + __show(m) + " want~" + checkMsgSubstr);
      else __pass++;
      __asyncPending--;
      __maybeDone();
    });
  return p;
}

// ---- ordered event trace (for event-order assertions) ----
var __trace = [];
function trace(ev) { __trace.push(ev); }
function trace_reset() { __trace = []; }
function assert_trace(expected, msg) {
  var a = JSON.stringify(__trace), e = JSON.stringify(expected);
  if (a === e) { __pass++; return; }
  __failLine("trace", msg, "got=" + a + " want=" + e);
}
function trace_slice() { return __trace.slice(); }

// ---- counter of async completions so summary waits for the loop ----
var __done = false;
var __doneFns = [];
function onDone(fn) { __doneFns.push(fn); }
function __maybeDone() {
  if (__done && __asyncPending === 0) {
    var fns = __doneFns; __doneFns = [];
    for (var i = 0; i < fns.length; i++) { try { fns[i](); } catch (e) { __out("FAIL onDone-handler " + e); __fail++; } }
  }
}
// finish() must be called by the probe when it has scheduled all work whose
// assertions count toward this run. summary() then waits for outstanding
// async assertions (up to timeoutMs) before emitting SUMMARY/RESULT.
var __waiters = [];
var __tag = "";
function waitAsync(ms) { __waiters.push(ms || 100); }
function summary(tag) {
  __tag = tag;
  var waitMs = 0;
  for (var i = 0; i < __waiters.length; i++) waitMs += __waiters[i];
  __done = true;
  if (__asyncPending > 0) {
    // setTimeout drives the loop until pending async assertions settle
    var step = function () {
      if (__asyncPending === 0) { __maybeDone(); finish(); return; }
      if (waitMs <= 0) {
        __out("FAIL async-timeout " + __asyncPending + " async assertion(s) never settled in " + __tag);
        __fail += __asyncPending;
        __asyncPending = 0;
        finish();
        return;
      }
      var chunk = waitMs < 50 ? waitMs : 50;
      waitMs -= chunk;
      setTimeout(step, chunk);
    };
    setTimeout(step, Math.min(50, waitMs || 50));
    return;
  }
  finish();
}
function finish() {
  __out("SUMMARY " + __tag + " pass=" + __pass + " fail=" + __fail);
  if (__fail > 0) {
    __out("RESULT FAIL");
    throw new Error("PROBE FAILED: " + __fail + " failure(s) in " + __tag);
  }
  __out("RESULT PASS");
}

// ---- generated probe file/basic ----

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
