// flags: --std
// bb_std_os.js -- black-box contract tests for the --std `std`/`os` compatibility layer.
//
// RUN WITH:  dynajs --std bb_std_os.js      (std/os are only exposed under --std)
//
// Contract: dynajs.d.ts lines 6687-6903 (the "std" and "os" module sections),
// plus the convention-difference table in the shared header (contract lines
// ~26-36), which pins:
//   * os.platform is a string PROPERTY, not a function
//   * the os.* filesystem door returns errno TUPLES, not throws
//   * os.exec/os.waitpid follow pid + negative-errno conventions
// Slice used: /tmp/dyna_contract/std_os_bench.d.ts (declares "std" and "os").
//
// DOC-TENSION (header prose vs slice declarations): the header table summarizes
// the tuples as "[null, value] success / [errno, null] failure", but every slice
// declaration pins the opposite order -- getcwd(): [string, number] "[cwd,
// errorcode]", stat(): [OsStat, number] "[stat, errorcode]".  These tests follow
// the slice (value first, errorcode second): the more specific reading.
//
// Filesystem writes happen ONLY inside a fresh temp dir, removed in `finally`.

import * as std from "std";
import * as os from "os";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); }

function runTable(tname, rows, fn) {
    for (const row of rows) {
        try { fn(row); }
        catch (e) { throw new Error("table " + tname + " row [" + row[0] + "]: " + (e && e.message ? e.message : String(e))); }
    }
}
async function runTableAsync(tname, rows, fn) {
    for (const row of rows) {
        try { await fn(row); }
        catch (e) { throw new Error("table " + tname + " row [" + row[0] + "]: " + (e && e.message ? e.message : String(e))); }
    }
}

// ------------------------------------------------------------------
// temp dir: the only place this file writes
// ------------------------------------------------------------------
const ORIG_CWD = os.getcwd()[0];
const DIR = (std.getenv("TMPDIR") || "/tmp") + "/bb_std_os_" + Date.now() + "_" + os.getpid();
{
    const r = os.mkdir(DIR, 0o700);
    assert(r === 0 || r < 0, "mkdir(temp dir) returns 0 or negative errno, got " + r);
    assertEq(os.stat(DIR)[1], 0, "temp dir exists after mkdir");
}
const created = [];
function p(name) { return DIR + "/" + name; }
function track(name) { created.push(p(name)); return p(name); }

try {

    // --------------------------------------------------------------
    // T1: platform / identity / constants (contract lines ~58-63, 61,
    // header table row "Platform: os.platform (string PROPERTY)")
    // --------------------------------------------------------------
    const PLAT_SET = ["linux", "darwin", "macos", "windows", "win32", "freebsd", "openbsd", "netbsd", "sunos", "solaris", "android", "unknown"];
    runTable("identity", [
        ["os.platform is a string", () => assertEq(typeof os.platform, "string", "typeof os.platform")],
        ["os.platform non-empty", () => assert(os.platform.length > 0, "os.platform non-empty")],
        // DOC-TENSION: the slice says only "The OS name"; the set of plausible values is
        // not enumerated anywhere in the slice, so this row accepts the common names.
        ["os.platform is a plausible OS name", () => assert(PLAT_SET.indexOf(os.platform) >= 0, "os.platform |" + os.platform + "| in documented-plausible set")],
        ["os.platform is NOT a function (header convention pin)", () => assert(typeof os.platform !== "function", "typeof os.platform !== function")],
        ["os.getpid() is a positive integer", () => assert(Number.isInteger(os.getpid()) && os.getpid() > 0, "getpid positive integer")],
        ["O_* constants are numbers", () => { for (const k of ["O_RDONLY", "O_WRONLY", "O_RDWR", "O_APPEND", "O_CREAT", "O_EXCL", "O_TRUNC"]) assertEq(typeof os[k], "number", "typeof os." + k); }],
        // Slice: "Windows only; absent on POSIX builds."
        ["O_BINARY/O_TEXT absent on POSIX", () => { if (os.platform.indexOf("win") !== 0) { assertEq(os.O_BINARY, undefined, "os.O_BINARY on POSIX"); assertEq(os.O_TEXT, undefined, "os.O_TEXT on POSIX"); } else { assertEq(typeof os.O_BINARY, "number", "os.O_BINARY on Windows"); } }],
        ["S_IF* constants are numbers", () => { for (const k of ["S_IFMT", "S_IFREG", "S_IFDIR", "S_IFLNK"]) assertEq(typeof os[k], "number", "typeof os." + k); }],
        ["WNOHANG/SIGTERM constants are numbers", () => { assertEq(typeof os.WNOHANG, "number", "typeof os.WNOHANG"); assertEq(typeof os.SIGTERM, "number", "typeof os.SIGTERM"); }],
        ["std.SEEK_SET/CUR/END are distinct numbers", () => { for (const k of ["SEEK_SET", "SEEK_CUR", "SEEK_END"]) assertEq(typeof std[k], "number", "typeof std." + k); assert(std.SEEK_SET !== std.SEEK_CUR && std.SEEK_CUR !== std.SEEK_END && std.SEEK_SET !== std.SEEK_END, "SEEK_* constants distinct"); }],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T2: clocks + blocking sleep (slice: os.now ms since arbitrary
    // point; os.sleep blocking ms)
    // --------------------------------------------------------------
    runTable("clocks", [
        ["now() is a finite number", () => assertEq(typeof os.now(), "number", "typeof os.now()") && assert(Number.isFinite(os.now()), "os.now() finite")],
        ["now() is monotonic non-decreasing over two calls", () => { const t1 = os.now(); const t2 = os.now(); assert(t2 >= t1, "t2 >= t1 (" + t2 + " >= " + t1 + ")"); }],
        // Boundary row: exactly one blocking sleep in this file.
        ["sleep(10) blocks at least 10ms", () => { const t1 = os.now(); os.sleep(10); const el = os.now() - t1; assert(el >= 10, "elapsed " + el + " >= 10ms"); }],
        // "arbitrary origin": os.now() must NOT be assumed epoch-anchored;
        // we only pin that it is number-valued (no Date.now() comparison row).
    ], row => row[1]());

    // --------------------------------------------------------------
    // T3: environment (slice: std.getenv/setenv/unsetenv/getenviron;
    // header row "Environment: std.getenv/setenv/getenviron")
    // --------------------------------------------------------------
    const UNIQ = "BB_STD_OS_UNIQUE_" + os.getpid();
    runTable("env", [
        ["getenv(missing unique var) === undefined", () => assertEq(std.getenv(UNIQ), undefined, "getenv before set")],
        ["setenv/getenv round trip", () => { std.setenv(UNIQ, "v1"); assertEq(std.getenv(UNIQ), "v1", "getenv after setenv v1"); }],
        ["setenv overwrites", () => { std.setenv(UNIQ, "v2"); assertEq(std.getenv(UNIQ), "v2", "getenv after overwrite v2"); }],
        ["getenviron reflects setenv", () => { const env = std.getenviron(); assertEq(typeof env, "object", "typeof getenviron()"); assertEq(env[UNIQ], "v2", "getenviron()[UNIQ]"); }],
        ["unsetenv restores absence", () => { std.unsetenv(UNIQ); assertEq(std.getenv(UNIQ), undefined, "getenv after unsetenv"); }],
        ["getenviron drops unset var", () => { const env = std.getenviron(); assert(!(UNIQ in env), "UNIQ absent from getenviron() after unsetenv"); }],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T4: std.sprintf exact rows (slice: "Format to a string")
    // --------------------------------------------------------------
    runTable("sprintf", [
        ["%d of 42", "%d", [42], "42"],
        ["%d of -42", "%d", [-42], "-42"],
        ["%s of abc", "%s", ["abc"], "abc"],
        ["%05d pads to width 5", "%05d", [42], "00042"],
        ["%x of 255", "%x", [255], "ff"],
        ["%f of 1.5 is six decimals", "%f", [1.5], "1.500000"],
        ["%% is a literal percent", "%%", [], "%"],
    ], row => { assertEq(std.sprintf(row[1], ...row[2]), row[3], row[0]); });

    // --------------------------------------------------------------
    // T5: std.printf / std.puts / std.gc (structural: documented,
    // fire-and-forget; output content is not asserted)
    // --------------------------------------------------------------
    runTable("print-gc", [
        ["printf formats without throwing", () => { std.printf("%s=%d\n", "bb_std_os", 1); assert(true, "printf executed"); }],
        ["puts without throwing", () => { std.puts(""); assert(true, "puts executed"); }],
        ["gc() without throwing", () => { std.gc(); assert(true, "gc executed"); }],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T6: std.evalScript (slice: "Evaluates the given script in global
    // scope")
    // --------------------------------------------------------------
    runTable("evalScript", [
        ["evalScript('1+1') === 2", "1+1", 2],
        ["evalScript('a+b') string concat", "'a'+'b'", "ab"],
        ["evalScript global side effect", "(globalThis.__bbEvalMarker = 9)", 9],
    ], row => { assertEq(std.evalScript(row[1]), row[2], row[0]); });
    assertEq(globalThis.__bbEvalMarker, 9, "evalScript ran in global scope");

    // --------------------------------------------------------------
    // T7: std.loadScript / std.loadFile (temp files only)
    // --------------------------------------------------------------
    runTable("loadScript-loadFile", [
        ["loadScript evaluates the file and returns completion", () => {
            const f = track("ls.js");
            const w = std.open(f, "w");
            assert(w !== null, "open(ls.js, w) non-null");
            w.writeStr("globalThis.__bbLSMarker = 41; 41");
            w.close();
            assertEq(std.loadScript(f), 41, "loadScript completion value");
            assertEq(globalThis.__bbLSMarker, 41, "loadScript global side effect");
        }],
        ["loadFile reads whole file as string", () => {
            const f = track("lf.txt");
            const w = std.open(f, "w");
            w.writeStr("loadfile-content");
            w.close();
            assertEq(std.loadFile(f), "loadfile-content", "loadFile content");
        }],
        ["loadFile(missing) === null", () => assertEq(std.loadFile(p("no_such_lf_zz")), null, "loadFile missing returns null")],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T8: std.parseExtJSON + std.strerror
    // --------------------------------------------------------------
    runTable("parseExtJSON-strerror", [
        ["parseExtJSON plain object", () => assertDeepEq(std.parseExtJSON('{"a":1,"b":[1,2]}'), { a: 1, b: [1, 2] }, "parseExtJSON basic")],
        ["parseExtJSON scalars", () => assertDeepEq(std.parseExtJSON('{"s":"x","n":1.5,"t":true,"z":null}'), { s: "x", n: 1.5, t: true, z: null }, "parseExtJSON scalars")],
        ["strerror returns a non-empty string", () => { const s = std.strerror(2); assertEq(typeof s, "string", "typeof strerror(2)"); assert(s.length > 0, "strerror(2) non-empty"); }],
        ["strerror(0) is a string", () => assertEq(typeof std.strerror(0), "string", "typeof strerror(0)")],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T9: StdFile round trip inside temp dir (slice lines 2-18: eof
    // and error are METHODS, getByte/putByte/readBytes/readAsString/
    // writeBytes/writeStr/getline/seek/tell/close)
    // --------------------------------------------------------------
    const SF = track("sf.txt");
    const SF_TEXT = "hello dyna\nsecond line\n";
    runTable("std.open-write", [
        ["open(w) returns a handle", () => { const f = std.open(SF, "w"); assert(f !== null && f !== undefined, "std.open(SF, 'w') non-null"); f.writeStr(SF_TEXT); f.writeBytes(new Uint8Array([0x21])); assertEq(f.error(), false, "error() false after writes"); f.close(); }],
        ["putByte appends one byte", () => { const f = std.open(SF, "a"); assert(f !== null, "open(SF, a) non-null"); f.putByte(0x2E); f.close(); }],
    ], row => row[1]());
    // TEST-FIX: the file was written as SF_TEXT + 0x21 ('!' via writeBytes) + 0x2E ('.' via
    // putByte), so the full content ends in "!.".
    const SF_FULL = SF_TEXT + "!.";
    runTable("std.open-read", [
        ["eof is a METHOD, not a property (contract pin)", () => { const f = std.open(SF, "r"); assertEq(typeof f.eof, "function", "typeof f.eof"); f.close(); }],
        ["readAsString(max) reads the whole file", () => { const f = std.open(SF, "r"); assertEq(f.readAsString(1000), SF_FULL, "readAsString full content"); assertEq(f.eof(), true, "eof() true at end"); assertEq(f.error(), false, "error() false after read"); f.close(); }],
        // TEST-FIX: the file has a THIRD line "!." (from writeBytes/putByte) with no trailing
        // newline, so getline yields it before the clean-EOF null.
        ["getline yields lines without newline then null", () => { const f = std.open(SF, "r"); assertEq(f.getline(), "hello dyna", "getline 1"); assertEq(f.getline(), "second line", "getline 2"); assertEq(f.getline(), "!.", "getline 3 (unterminated last line)"); assertEq(f.getline(), null, "getline at clean EOF"); assertEq(f.eof(), true, "eof after getline null"); f.close(); }],
        ["getByte reads back putByte bytes", () => { const f = std.open(SF, "r"); assertEq(f.getByte(), 0x68, "getByte 'h'"); assertEq(f.getByte(), 0x65, "getByte 'e'"); f.close(); }],
        ["readBytes(max) exact bytes", () => { const f = std.open(SF, "r"); const b = f.readBytes(4); assert(b instanceof Uint8Array, "readBytes returns Uint8Array"); assertDeepEq(Array.from(b), [0x68, 0x65, 0x6c, 0x6c], "readBytes 'hell'"); f.close(); }],
        ["fileno() is a number", () => { const f = std.open(SF, "r"); assertEq(typeof f.fileno(), "number", "typeof fileno()"); f.close(); }],
        ["open(missing, r) === null (no throw: errno door)", () => { const err = new Error("open"); assertEq(std.open(p("no_such_sf_zz"), "r", err), null, "open missing returns null"); }],
        ["tmpfile() round trip write+seek+read", () => { const t = std.tmpfile(); t.writeStr("tmp-data"); t.seek(0, std.SEEK_SET); assertEq(t.readAsString(100), "tmp-data", "tmpfile round trip"); t.close(); }],
        ["std.in/out/err expose StdFile methods", () => { assertEq(typeof std.in.getByte, "function", "std.in.getByte"); assertEq(typeof std.out.writeStr, "function", "std.out.writeStr"); assertEq(typeof std.err.writeStr, "function", "std.err.writeStr"); }],
    ], row => row[1]());
    runTable("std.seek-tell", [
        ["seek(2, SEEK_SET) then tell() === 2", () => { const f = std.open(SF, "r"); f.seek(2, std.SEEK_SET); assertEq(f.tell(), 2, "tell after SEEK_SET 2"); assertEq(f.getByte(), 0x6c, "byte at offset 2 is 'l'"); f.close(); }],
        // TEST-FIX: "hello dyna..." has the space at offset 5 ('o' is offset 4), and the file
        // ends "!." so seek(-2, SEEK_END) lands on '!' (0x21).
        ["seek(+3, SEEK_CUR) moves from current", () => { const f = std.open(SF, "r"); f.seek(2, std.SEEK_SET); f.seek(3, std.SEEK_CUR); assertEq(f.tell(), 5, "tell after SEEK_CUR +3"); assertEq(f.getByte(), 0x20, "byte at offset 5 is the space after 'hello'"); f.close(); }],
        ["seek(-2, SEEK_END) lands two from end", () => { const f = std.open(SF, "r"); f.seek(-2, std.SEEK_END); assertEq(f.getByte(), 0x21, "byte two from end is '!'"); f.close(); }],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T10: os errno-tuple filesystem door (slice lines 58-94; header
    // row "Filesystem: errno tuples").  Value-first tuples per slice.
    // --------------------------------------------------------------
    const F1 = track("a.txt");
    {
        const w = std.open(F1, "w");
        w.writeStr("hello stat"); // 10 bytes
        w.close();
    }
    runTable("os.fs-tuples", [
        ["getcwd() === [dir, 0], absolute", () => { const r = os.getcwd(); assert(Array.isArray(r), "getcwd returns a tuple"); assertEq(r[1], 0, "getcwd errorcode 0"); assertEq(typeof r[0], "string", "getcwd cwd is string"); assert(r[0][0] === "/", "cwd is absolute"); }],
        ["chdir(tempdir) === 0", () => assertEq(os.chdir(DIR), 0, "chdir into temp dir")],
        ["chdir(back) === 0", () => assertEq(os.chdir(ORIG_CWD), 0, "chdir back")],
        ["chdir(missing) is a negative errno", () => assert(os.chdir(p("no_such_dir_zz")) < 0, "chdir missing negative")],
        ["stat(file) err 0, size === byte length", () => { const r = os.stat(F1); assertEq(r[1], 0, "stat errorcode 0"); assertEq(typeof r[0], "object", "stat object present"); assertEq(r[0].size, 10, "stat size 10"); }],
        ["stat(file) exposes all 12 numeric fields", () => { const st = os.stat(F1)[0]; for (const k of ["dev", "ino", "mode", "nlink", "uid", "gid", "rdev", "size", "blocks", "atime", "mtime", "ctime"]) assertEq(typeof st[k], "number", "stat." + k + " is number"); }],
        ["stat(file) mode & S_IFMT === S_IFREG", () => { const st = os.stat(F1)[0]; assertEq(st.mode & os.S_IFMT, os.S_IFREG, "regular file mode mask"); }],
        ["stat times are ms since epoch (plausible window)", () => { const st = os.stat(F1)[0]; assert(st.mtime > 1000000000000 && st.mtime <= Date.now() + 60000, "mtime " + st.mtime + " in epoch-ms window"); }],
        ["stat(dir) mode & S_IFMT === S_IFDIR", () => { const r = os.stat(DIR); assertEq(r[1], 0, "stat dir err 0"); assertEq(r[0].mode & os.S_IFMT, os.S_IFDIR, "dir mode mask"); }],
        ["stat(missing) is a failed tuple", () => { const r = os.stat(p("no_such_stat_zz")); assert(Array.isArray(r), "stat returns a tuple"); assert(r[1] !== 0, "stat missing errorcode nonzero"); }],
        ["mkdir(new) === 0", () => assertEq(os.mkdir(p("sub"), 0o700), 0, "mkdir sub")],
        ["mkdir(existing) is a negative errno (EEXIST)", () => assert(os.mkdir(p("sub"), 0o700) < 0, "mkdir existing negative")],
        ["readdir(dir) === [names, 0] including entries", () => { const r = os.readdir(DIR); assertEq(r[1], 0, "readdir err 0"); assert(Array.isArray(r[0]), "readdir names array"); assert(r[0].indexOf("a.txt") >= 0, "readdir includes a.txt"); assert(r[0].indexOf("sub") >= 0, "readdir includes sub"); }],
        ["readdir(missing) fails the tuple", () => { const r = os.readdir(p("no_such_dir_zz")); assert(r[1] !== 0, "readdir missing errorcode nonzero"); }],
        ["symlink(target, link) === 0", () => assertEq(os.symlink("a.txt", p("a.lnk")), 0, "symlink created")],
        ["symlink(existing link) is a negative errno", () => assert(os.symlink("a.txt", p("a.lnk")) < 0, "symlink EEXIST negative")],
        ["readlink(link) === [target, 0]", () => { const r = os.readlink(p("a.lnk")); assertDeepEq(r, ["a.txt", 0], "readlink target+err"); }],
        ["lstat(link) is the symlink (S_IFLNK)", () => { const r = os.lstat(p("a.lnk")); assertEq(r[1], 0, "lstat err 0"); assertEq(r[0].mode & os.S_IFMT, os.S_IFLNK, "lstat does not follow"); }],
        ["stat(link) follows to the regular file", () => { const r = os.stat(p("a.lnk")); assertEq(r[1], 0, "stat(link) err 0"); assertEq(r[0].mode & os.S_IFMT, os.S_IFREG, "stat follows symlink"); }],
        // TEST-FIX: realpath(3) resolves ALL symlinks, so on macOS (/var -> /private/var) the
        // resolved form of an absolute temp path differs textually; assert err 0 + absolute +
        // the file's name as the resolved suffix instead of verbatim equality.
        ["realpath(file) === [abs path, 0]", () => { const r = os.realpath(F1); assertEq(r[1], 0, "realpath err 0"); assert(r[0][0] === "/", "realpath absolute"); assert(r[0].endsWith("/a.txt"), "realpath resolves to the file, got " + r[0]); }],
        ["realpath(missing) fails the tuple", () => { const r = os.realpath(p("no_such_rp_zz")); assert(r[1] !== 0, "realpath missing errorcode nonzero"); }],
        ["utimes(path, atime, mtime) === 0 and stat reflects it", () => { assertEq(os.utimes(F1, 1000, 2000), 0, "utimes return 0"); const st = os.stat(F1)[0]; assertEq(st.atime, 1000, "stat.atime === 1000"); assertEq(st.mtime, 2000, "stat.mtime === 2000"); }],
        ["rename(a, b) === 0 moves the file", () => { const b = p("b.txt"); created.push(b); assertEq(os.rename(F1, b), 0, "rename a.txt b.txt"); assert(os.stat(F1)[1] !== 0, "old path gone"); assertEq(os.stat(b)[1], 0, "new path present"); assertEq(os.stat(b)[0].size, 10, "renamed file keeps size"); }],
        ["rename(missing, x) is a negative errno", () => assert(os.rename(p("no_such_mv_zz"), p("mv-target")) < 0, "rename missing negative")],
        ["remove(file) === 0, second remove is negative", () => { const b = p("b.txt"); assertEq(os.remove(b), 0, "remove b.txt"); assert(os.remove(b) < 0, "remove missing negative"); }],
        ["remove(empty dir) === 0", () => assertEq(os.remove(p("sub")), 0, "remove empty dir sub")],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T11: os raw-fd door (slice lines 94-112: open/read/write/seek/
    // dup/pipe/isatty/close with negative-errno convention)
    // --------------------------------------------------------------
    const FD = track("fd.txt");
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const abOf = s => enc.encode(s).buffer; // fresh ArrayBuffer per call
    runTable("os.fd", [
        ["open(path, O_RDWR|O_CREAT|O_TRUNC, mode) -> fd >= 0", () => { const fd = os.open(FD, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644); assert(Number.isInteger(fd) && fd >= 0, "fd non-negative integer, got " + fd); created.fd = fd; }],
        ["write(fd, ab, 0, 5) === 5", () => { assertEq(os.write(created.fd, abOf("01234"), 0, 5), 5, "full write byte count"); }],
        ["seek(fd, 0, SEEK_SET) === 0 (new offset)", () => assertEq(os.seek(created.fd, 0, std.SEEK_SET), 0, "seek returns new offset 0")],
        ["read(fd, ab, 0, 5) === 5 with exact bytes", () => { const ab = new ArrayBuffer(8); const got = os.read(created.fd, ab, 0, 5); assertEq(got, 5, "read byte count"); assertEq(dec.decode(new Uint8Array(ab, 0, 5)), "01234", "read bytes decode"); }],
        ["write with position arg writes at offset", () => { assertEq(os.write(created.fd, abOf("AB"), 0, 2, 1), 2, "positioned write count"); assertEq(os.seek(created.fd, 0, std.SEEK_SET), 0, "rewind"); const ab = new ArrayBuffer(8); assertEq(os.read(created.fd, ab, 0, 5), 5, "read back 5"); assertEq(dec.decode(new Uint8Array(ab, 0, 5)), "0AB34", "positioned overwrite result"); }],
        ["read at EOF returns 0", () => { assertEq(os.seek(created.fd, 5, std.SEEK_SET), 5, "seek to EOF"); assertEq(os.read(created.fd, new ArrayBuffer(4), 0, 4), 0, "EOF read is 0 bytes"); }],
        ["isatty(regular file fd) === false", () => assertEq(os.isatty(created.fd), false, "isatty on a regular file")],
        ["dup(fd) -> new distinct fd", () => { const d = os.dup(created.fd); assert(Number.isInteger(d) && d >= 0 && d !== created.fd, "dup new fd"); created.dup = d; }],
        ["close(dup) === 0", () => assertEq(os.close(created.dup), 0, "close dup")],
        ["close(fd) === 0", () => assertEq(os.close(created.fd), 0, "close fd")],
        ["read on closed fd is a negative errno", () => assert(os.read(created.fd, new ArrayBuffer(4), 0, 4) < 0, "read after close negative")],
        ["close on closed fd is a negative errno", () => assert(os.close(created.fd) < 0, "double close negative")],
        ["open(missing, O_RDONLY) is a negative errno", () => assert(os.open(p("no_such_fd_zz"), os.O_RDONLY) < 0, "open missing negative")],
        ["pipe() -> [readFd, writeFd], bytes flow", () => { const pp = os.pipe(); assert(pp !== null && Array.isArray(pp) && pp.length === 2, "pipe returns a fd pair"); const rfd = pp[0], wfd = pp[1]; assertEq(os.write(wfd, abOf("ping"), 0, 4), 4, "pipe write 4"); const ab = new ArrayBuffer(4); assertEq(os.read(rfd, ab, 0, 4), 4, "pipe read 4"); assertEq(dec.decode(ab), "ping", "pipe bytes"); assertEq(os.close(wfd), 0, "close pipe w"); assertEq(os.close(rfd), 0, "close pipe r"); }],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T12: os.exec / os.waitpid (header row "Spawn ... pid + negative-
    // errno conventions"; slice lines 113-119).  Structural only; no
    // shell pipelines; only a /usr/bin/true-style no-op child.
    // --------------------------------------------------------------
    runTable("os.exec-waitpid", [
        ['exec(["true"]) === 0 (exit status)', ["true"], null, "zero"],
        // refusal row: usePath:false with a bare name cannot resolve
        ["exec with usePath:false and bare name fails nonzero", ["true"], { usePath: false }, "nonzero"],
        // refusal row: DOC-TENSION -- the DynaJS header documents a negative-errno
        // convention for spawn failure, while upstream quickjs exits 127 when the
        // child's execvpe fails; both readings agree the status is NONZERO.
        ["exec of a missing binary fails nonzero", ["/bb_nonexistent_zz_9x"], null, "nonzero"],
        ["exec blocking:false returns a pid, waitpid reaps [pid, 0]", ["true"], { blocking: false }, "pid"],
        ["waitpid(-1, WNOHANG) with no children -> [-1, errno]", () => {
            const r = os.waitpid(-1, os.WNOHANG);
            assert(Array.isArray(r) && r.length === 2, "waitpid returns a pair");
            // TEST-FIX: this os door's documented convention is pid + NEGATIVE-ERRNO
            // (d.ts header: "os.exec + os.waitpid (pid + negative-errno conventions)"), so a
            // no-children waitpid returns [-ECHILD, ECHILD] (e.g. -10 on macOS), not the POSIX
            // literal -1.
            assert(r[0] < 0, "waitpid no-children returns a negative errno, got " + r[0]);
            assertEq(typeof r[1], "number", "waitpid errno is a number");
        }],
    ], row => {
        const [label, args, opts, kind] = row;
        if (kind === "zero" || kind === "nonzero") {
            const r = opts === null ? os.exec(args) : os.exec(args, opts);
            assertEq(typeof r, "number", "exec returns a number status");
            if (kind === "zero") assertEq(r, 0, "exit status of true");
            else assert(r !== 0, "failure status nonzero, got " + r);
        } else if (kind === "pid") {
            const pid = os.exec(args, opts);
            assert(Number.isInteger(pid) && pid > 0, "non-blocking exec returns a pid, got " + pid);
            const wr = os.waitpid(pid);
            assert(Array.isArray(wr) && wr.length === 2, "waitpid returns a pair");
            assertEq(wr[0], pid, "waitpid reaped pid");
            assertEq(wr[1], 0, "waitpid status 0 for exit code 0");
        } else {
            row[1]();
        }
    });

    // signal registration is structural only (no signal is ever sent)
    runTable("os.signal", [
        ["signal(SIGUSR2, handler) registers without throwing", () => { os.signal(os.SIGUSR2, function () {}); assert(true, "signal handler registered"); }],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T13: os timers + sleepAsync (async rows; event-loop driven)
    // --------------------------------------------------------------
    await runTableAsync("os.timers", [
        ["setTimeout(1ms) resolves via the event loop", async () => { let ran = false; await new Promise(res => { os.setTimeout(() => { ran = true; res(); }, 1); }); assert(ran, "setTimeout callback ran"); }],
        ["clearTimeout after fire is a harmless no-op", async () => { const id = os.setTimeout(() => {}, 1); await new Promise(res => os.setTimeout(res, 5)); os.clearTimeout(id); assert(true, "clearTimeout no-op"); }],
        ["cleared setInterval never fires", async () => { let fired = false; const id = os.setInterval(() => { fired = true; }, 1); os.clearInterval(id); await new Promise(res => os.setTimeout(res, 5)); assert(!fired, "cleared interval did not fire"); }],
        ["sleepAsync(1) resolves", async () => { await os.sleepAsync(1); assert(true, "sleepAsync resolved"); }],
    ], row => row[1]());

    // --------------------------------------------------------------
    // T14: os.Worker postMessage transfer validation (d.ts: "Optional
    // transfer list of ArrayBuffers/ArrayBufferView ... A DataView entry
    // transfers its underlying buffer; a SharedArrayBuffer is refused
    // (it cannot be detached)")
    // --------------------------------------------------------------
    {
        const wf = track("worker_bb_empty.mjs");
        const wfd = os.open(wf, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644);
        os.close(wfd);
        const w = new os.Worker(wf);
        let viewErr = null, dvErr = null, sabErr = null, sabViewErr = null;
        try { w.postMessage({ n: 1 }, [new Uint8Array(4)]); } catch (e) { viewErr = e; }
        try { w.postMessage({ n: 2 }, [new DataView(new ArrayBuffer(8))]); } catch (e) { dvErr = e; }
        const sab = new SharedArrayBuffer(8);
        try { w.postMessage({ n: 3 }, [sab]); } catch (e) { sabErr = e; }
        try { w.postMessage({ n: 4 }, [new Uint8Array(sab)]); } catch (e) { sabViewErr = e; }
        assert(viewErr === null, "os.Worker [Uint8Array in the transfer list is accepted — got |" + viewErr + "|]");
        assert(dvErr === null, "os.Worker [DataView in the transfer list is accepted — got |" + dvErr + "|]");
        assert(sabErr !== null, "os.Worker [a bare SharedArrayBuffer is refused]");
        assert(/SharedArrayBuffer/.test(String(sabErr)), "os.Worker [the SAB refusal names SharedArrayBuffer — got |" + sabErr + "|]");
        assert(sabViewErr !== null, "os.Worker [a SAB-backed TypedArray is refused]");
        assert(/SharedArrayBuffer/.test(String(sabViewErr)), "os.Worker [the SAB-backed view refusal names SharedArrayBuffer — got |" + sabViewErr + "|]");
        w.onmessage = null;
    }

} finally {
    // restore cwd, then remove everything we created (files first, dir last)
    try { os.chdir(ORIG_CWD); } catch (e) {}
    for (let i = created.length - 1; i >= 0; i--) { try { os.remove(created[i]); } catch (e) {} }
    try { os.remove(DIR); } catch (e) {}
}

print("bb_std_os: all tests passed (" + n + " assertions)");
