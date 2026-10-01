// Black-box contract test for dyna:file, generated from dynajs.d.ts lines 1596-1896. Engine sources not consulted.
// Table-driven: CASES tables are rows of [label, expected, ...args] (refusal tables carry an
// error class + pattern) driven through ONE loop whose failure message names the row.
// Every filesystem case lives inside one fresh mkdtemp directory, cleaned in a finally via
// removeAll ("Recursive, symlink-safe removal; a missing path is a no-op").

import {
    Path, File, FileReader, FileWriter, Glob, FileLock, Watcher,
    readFile, readBytes, writeFile, readFileAsync, writeFileAsync, copyFileAsync, asyncStats,
    stat, lstat, exists, readDir, makeDir, remove, removeAll, rename, copyFile, move,
    sniffType, symlink, readLink, realPath, chmod, glob,
    tempDir, makeTempDir, makeTempFile,
} from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) {
    n++;
    const ok = Object.is(actual, expected) ||
        (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertDeepEq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b))
        throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType))
        throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern)))
        throw new Error("wrong error message |" + e + "|: " + msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
function u8(...bytes) { return new Uint8Array(bytes); }

// Table drivers: rows are [label, expected, ...args]; the failure names the row.
function rows(cases, fn, mod) {
    for (const [label, expected, ...args] of cases) assertEq(fn(...args), expected, mod + " [" + label + "]");
}
function deepRows(cases, fn, mod) {
    for (const [label, expected, ...args] of cases) assertDeepEq(fn(...args), expected, mod + " [" + label + "]");
}
// Refusal rows: [label, thunk, ErrType-or-null, pattern-or-null]
function throwRows(cases, mod) {
    for (const [label, fn, ErrType, pattern] of cases)
        assertThrows(fn, mod + " [" + label + "]", ErrType, pattern);
}

const ROOT = makeTempDir("bbfile-");
try {

    /* ==================== Path string algebra (pure, no fs) ==================== */

    // "Joins and normalizes the segments" (ctor); API.md pins new Path("/a").join("b","c.txt") === "/a/b/c.txt"
    rows([
        ["absolute two-seg", "/a/b", "/a", "b"],
        ["three-seg (pinned example)", "/a/b/c.txt", "/a", "b", "c.txt"],
        ["relative", "a/b", "a", "b"],
        ["Path segment accepted", "/a/b", new Path("/a"), "b"],
        ["single segment", "a", "a"],
    ], (base, ...rest) => String(new Path(base, ...rest)), "Path.ctor");

    // "join that resolves `.`/`..`; an absolute argument rebases" — API.md pins
    // new Path("/a/b/c.txt").resolve("..", "d") === "/a/b/d"
    rows([
        ["dotdot up-down (pinned example)", "/a/b/d", "/a/b/c.txt", "..", "d"],
        ["inner dotdot collapse", "/x/a/c", "/x/y", "../a/b/../c"],
        ["absolute arg rebases", "/c/d", "/a/b", "/c/d"],
        ["no args keeps base", "/a/b", "/a/b"],
        ["dot arg resolves away", "/a/b", "/a/b", "."],
    ], (base, ...rest) => String(new Path(base).resolve(...rest)), "Path.resolve");

    // "The relative path from `this` to `other`"; "the empty result normalises to `.`" (API.md)
    rows([
        // DOC-TENSION (resolved): API.md's example prints rel = "../../c.txt" for
        // new Path("/a/b/c.txt").relativeTo(new Path("/a")), but the prose pins "the relative
        // path from `this` to `other`" -- c.txt is part of the SOURCE, so the standard relpath
        // algorithm (Node path.relative, Python os.path.relpath, and the sibling/child rows
        // below) answers "../..". We follow the prose and the algorithm.
        ["from a file up to its grandparent dir", "../..", "/a/b/c.txt", "/a"],
        ["self normalizes to dot", ".", "/a", "/a"],
        ["sibling", "../c", "/a/b", "/a/c"],
        ["child", "b/c.txt", "/a", "/a/b/c.txt"],
    ], (from, to) => String(new Path(from).relativeTo(new Path(to))), "Path.relativeTo");

    // "The final component as a string"
    rows([
        ["file with dir", "c.txt", "/a/b/c.txt"],
        ["bare name", "c.txt", "c.txt"],
        ["plain dir", "b", "/a/b"],
    ], (p) => new Path(p).basename, "Path.basename");

    // "The extension, including the dot"
    rows([
        ["single ext", ".txt", "/a/b/c.txt"],
        ["final ext only", ".gz", "archive.tar.gz"],
        ["no ext", "", "noext"],
    ], (p) => new Path(p).extname, "Path.extname");

    // "Basename with the given suffix removed"
    rows([
        ["strips suffix", "file", "file.txt", ".txt"],
        ["absent suffix no-op", "file.txt", "file.txt", ".md"],
        ["suffix without dot", "file.", "file.txt", "txt"],
    ], (p, s) => new Path(p).basenameWithout(s), "Path.basenameWithout");

    // "Byte equality of the normalized forms" (API.md: "a//b" equals "a/b"); "false for a non-Path argument"
    rows([
        // rows are [label, expected, thisPath, otherValue, passRaw] — passRaw hands `otherValue`
        // to equals() as-is (the d.ts: "false for a non-Path argument").
        ["normalized equality (pinned)", true, "a//b", "a/b", false],
        ["identical", true, "a/b", "a/b", false],
        ["different", false, "a/c", "a/b", false],
        ["non-Path argument", false, "a/b", "a/b", true],
    ], (a, b, raw) => (raw ? new Path(a).equals(b) : new Path(a).equals(new Path(b))), "Path.equals");

    // "toString()/toJSON() -> the normalised path string"
    rows([
        ["toJSON", "/a/b", "/a/b"],
        ["JSON.stringify", '"/a/b"', "/a/b", true],
    ], (p, raw) => raw ? JSON.stringify(new Path(p)) : new Path(p).toJSON(), "Path.json");

    // "True when `v` is a Path"
    rows([
        ["Path value", true, new Path("a")],
        ["string value", false, "a"],
        ["null", false, null],
    ], (v) => Path.isPath(v), "Path.isPath");

    // sep/delimiter pinned to "/" and ":" on this platform; API.md pins Path.temp() === tempDir() strings
    rows([
        // rows carry the key as the third element: the runner hands fn the row's third slot.
        ["sep", "/", "sep"],
        ["delimiter", ":", "delimiter"],
        ["temp stringifies as tempDir()", String(tempDir()), "temp"],
    ], (k) => {
        if (k === "sep") return Path.sep;
        if (k === "delimiter") return Path.delimiter;
        return String(Path.temp());
    }, "Path.statics");

    /* ==================== refusal table (strict bags, NUL, strict open) ==================== */

    throwRows([
        ["NUL-bearing Path segment", () => new Path("a\0b"), TypeError, null],
        ["writeFile unknown option", () => writeFile(new Path(ROOT, "o.txt"), "x", { bogus: 1 }), TypeError, /bogus/],
        ["readFile unknown option", () => readFile(new Path(ROOT, "o.txt"), { bogus: 1 }), TypeError, /bogus/],
        ["makeDir unknown option", () => makeDir(new Path(ROOT, "o"), { bogus: 1 }), TypeError, /bogus/],
        ["copyFile unknown option", () => copyFile(new Path(ROOT, "o.txt"), new Path(ROOT, "o2.txt"), { bogus: 1 }), TypeError, /bogus/],
        ["glob unknown option (no followSymlinks)", () => glob("x", { cwd: ROOT, followSymlinks: true }), TypeError, null],
        ["readFile missing file (strict open first)", () => readFile(new Path(ROOT, "definitely-missing")), Error, null],
        ["makeDir missing parent without recursive", () => makeDir(new Path(ROOT, "mk2", "x")), Error, null],
    ], "dyna:file.refusals");

    /* ==================== temp dirs and temp files ==================== */

    {
        // "mkdtemp under the temp dir" / "mkstemp; returns the path of the empty file"
        const d = makeTempDir("bbfile-t-");
        rows([
            ["under tempDir", true, String(d).startsWith(String(tempDir()))],
            ["is a directory", true, stat(d).isDir],
            ["removeAll cleans up", false, (removeAll(d), exists(d))],
        ], (x) => x, "makeTempDir");

        const tf = makeTempFile("bbfile-f-");
        rows([
            ["created", true, exists(tf)],
            ["empty (mkstemp)", 0, stat(tf).size],
            ["reads empty", "", readFile(tf)],
        ], (x) => x, "makeTempFile");
        remove(tf);
    }

    /* ==================== write/read round-trip table (handle + free function) ==================== */

    // Rows: write payload -> byte count, readBytes, readText. "returns the byte count";
    // "invalid UTF-8 becomes U+FFFD" on the string path only; readBytes never corrupts.
    const RT = [
        ["ascii", "hello", [104, 101, 108, 108, 111], "hello"],
        ["empty file", "", [], ""],
        ["2-byte UTF-8", "é", [0xC3, 0xA9], "é"],
        ["4-byte UTF-8", "\u{1F600}", [0xF0, 0x9F, 0x98, 0x80], "\u{1F600}"],
        ["invalid UTF-8 bytes", u8(0, 255, 0x80, 1), [0, 255, 0x80, 1], "\u0000\uFFFD\uFFFD\u0001"],
        ["CRLF kept", "a\r\nb", [97, 13, 10, 98], "a\r\nb"],
        ["NUL byte kept", "a\0b", [97, 0, 98], "a\0b"],
    ];
    for (const [label, payload, bytes, text] of RT) {
        const p = new Path(ROOT, "rt-" + label.replace(/[^a-z0-9]+/gi, "-") + ".bin");
        const f = new File(p);
        assertEq(f.writeText(payload), bytes.length, "round-trip [" + label + "] writeText byte count");
        assert(eqArr(f.readBytes(), bytes), "round-trip [" + label + "] readBytes");
        assertEq(f.readText(), text, "round-trip [" + label + "] readText (U+FFFD repair per doc)");
        // the same rows through the free functions ("Identical to File.readBytes")
        assertEq(writeFile(p, payload), bytes.length, "round-trip [" + label + "] writeFile count");
        assert(eqArr(readBytes(p), bytes), "round-trip [" + label + "] readBytes free fn");
        assertEq(readFile(p), text, "round-trip [" + label + "] readFile");
    }

    /* ==================== append vs truncate table ==================== */

    {
        // "O_CREAT, truncate unless append"; "append(data): writeText with append supplied"
        const p = new Path(ROOT, "append.txt");
        const f = new File(p);
        const SEQ = [
            ["first write", () => f.writeText("xy"), "xy"],
            ["second write truncates", () => f.writeText("z"), "z"],
            ["append()", () => f.append("!"), "z!"],
            ["writeText {append:true}", () => f.writeText("?", { append: true }), "z!?"],
            ["writeBytes alias appends", () => f.writeBytes(u8(65), { append: true }), "z!?A"],
        ];
        for (const [label, fn, after] of SEQ) {
            const count = fn();
            assert(count > 0, "append-seq [" + label + "] returned a positive byte count");
            assertEq(f.readText(), after, "append-seq [" + label + "] content");
        }
        // free-function append
        writeFile(p, "1");
        writeFile(p, "2", { append: true });
        assertEq(readFile(p), "12", "append-seq [free writeFile append] content");
    }

    /* ==================== stat / lstat field tables ==================== */

    {
        const p = new Path(ROOT, "st.txt");
        writeFile(p, "12345");
        const st = stat(p);
        // Stat interface: one value row per pinned field, one type row per typed field
        // rows carry the field name as the third element: the runner hands fn the row's third slot.
        rows([
            ["size", 5, "size"],
            ["isDir", false, "isDir"],
            ["isFile", true, "isFile"],
            ["isSymlink", false, "isSymlink"],
            ["nlink", 1, "nlink"], // a fresh file has one hard link (POSIX)
        ], (k) => st[k], "stat.fields");
        rows([
            ["mode", "number", "mode"],
            ["mtimeMs", "number", "mtimeMs"],
            ["atimeMs", "number", "atimeMs"],
            ["ctimeMs", "number", "ctimeMs"],
            ["uid", "number", "uid"],
            ["gid", "number", "gid"],
            ["ino", "number", "ino"],
        ], (k) => typeof st[k], "stat.field-types");
        assert(st.mtimeMs > 0 && Math.abs(st.mtimeMs - Date.now()) < 60000, "stat.fields [mtimeMs positive and close to now]");

        const d = new Path(ROOT, "st.d");
        makeDir(d);
        rows([
            ["dir isDir", true, "isDir"],
            ["dir not isFile", false, "isFile"],
        ], (i) => stat(d)[i], "stat.dir");
        remove(d); // "Unlink a file or an empty directory"

        // stat follows the final component; lstat does not (d.ts one-liners)
        symlink("st.txt", new Path(ROOT, "st.ln"));
        rows([
            ["stat follows link", true, stat(new Path(ROOT, "st.ln")).isFile],
            ["lstat sees symlink", true, lstat(new Path(ROOT, "st.ln")).isSymlink],
            ["lstat not isFile", false, lstat(new Path(ROOT, "st.ln")).isFile],
            ["lstat size is target length (POSIX)", 6, lstat(new Path(ROOT, "st.ln")).size],
        ], (x) => x, "stat.symlink");
    }

    /* ==================== exists table ==================== */

    {
        // "Boolean; never throws; uses lstat, so a dangling symlink reports true"
        symlink("no-such-target", new Path(ROOT, "dangling.ln"));
        rows([
            ["missing path", false, new Path(ROOT, "no-such-thing")],
            ["dangling symlink", true, new Path(ROOT, "dangling.ln")],
            ["real file", true, new Path(ROOT, "st.txt")],
        ], (p) => exists(p), "exists");
        assertThrows(() => stat(new Path(ROOT, "dangling.ln")),
            "exists [stat of a dangling symlink throws: follows the final component]");
        assertEq(lstat(new Path(ROOT, "dangling.ln")).isSymlink, true, "exists [lstat still classifies the dangling link]");
    }

    /* ==================== rename / move / copy tables ==================== */

    {
        // "rename(2)" — POSIX rename replaces an existing destination atomically
        const a = new Path(ROOT, "mv.a");
        const b = new Path(ROOT, "mv.b");
        writeFile(a, "AAA");
        writeFile(b, "BBB");
        rename(a, b);
        rows([
            ["source gone", false, exists(a)],
            ["dest replaced atomically (POSIX rename)", true, readFile(b) === "AAA"],
        ], (x) => x, "rename");

        // "rename(2), falling back to copy-then-unlink across filesystems" — same fs here
        const c = new Path(ROOT, "mv.c");
        move(b, c);
        rows([
            ["old location gone", false, exists(b)],
            ["new location present", true, exists(c)],
        ], (x) => x, "move");

        // "refusing an existing destination is the default. Returns the source byte count";
        // "The destination gets the source's permission bits" (documented)
        const src = new Path(ROOT, "cp.src");
        writeFile(src, "copy-me");
        chmod(src, 0o600);
        rows([
            ["fresh copy count", 7, copyFile(src, new Path(ROOT, "cp.1"))],
            ["fresh copy content", "copy-me", readFile(new Path(ROOT, "cp.1"))],
            ["dest inherits source perms (documented)", 0o600, stat(new Path(ROOT, "cp.1")).mode & 0o777],
            ["overwrite returns count", 7, copyFile(src, new Path(ROOT, "cp.2"), { overwrite: true })],
            ["overwrite content fresh", "copy-me", readFile(new Path(ROOT, "cp.2"))],
        ], (x) => x, "copyFile");
        let eex = null;
        try { copyFile(src, new Path(ROOT, "cp.2")); } catch (e) { eex = e; }
        assert(eex !== null, "copyFile [refuses existing destination]");
        assert(eex.code === "EEXIST" || /EEXIST/.test(String(eex)), "copyFile [refusal is EEXIST (documented)]");
        assertEq(readFile(new Path(ROOT, "cp.2")), "copy-me", "copyFile [refused copy left dest untouched: no partial file]");

        // File.copyTo: "same contract as the free copyFile"; moveTo "returns the handle itself"
        const f = new File(new Path(ROOT, "h.src"));
        f.writeText("handle");
        rows([
            ["copyTo lands content", "handle", f.copyTo(new Path(ROOT, "h.copy2")).readText()],
            ["copyTo source untouched", "handle", f.readText()],
            ["overwrite copy returns File", true, f.copyTo(new Path(ROOT, "h.copy2"), { overwrite: true }) instanceof File],
        ], (x) => x, "File.copyTo");
        // TEST-FIX: the contract ("same contract as the free copyFile") pins the EEXIST refusal,
        // which arrives as e.code === "EEXIST"; the message text is strerror's "File exists".
        {
            let ce = null;
            try { f.copyTo(new Path(ROOT, "h.copy2")); } catch (e) { ce = e; }
            assert(ce !== null, "File.copyTo [refuses existing without the flag]");
            assert(ce.code === "EEXIST" || /EEXIST/.test(String(ce)), "File.copyTo [refusal is EEXIST (documented)]");
        }
        const m = f.moveTo(new Path(ROOT, "h.moved"));
        assert(m === f, "File.moveTo [returns the handle itself (documented return-this)]");
        assert(String(f.path).endsWith("h.moved"), "File.moveTo [handle names the new location]");
        rows([
            ["old location gone", false, exists(new Path(ROOT, "h.src"))],
            ["moved handle still reads", "handle", f.readText()],
        ], (x) => x, "File.moveTo");
    }

    /* ==================== makeDir / remove / removeAll ==================== */

    {
        // "recursive creates missing parents"; "an existing directory is success"
        const deep = new Path(ROOT, "mk", "a", "b");
        makeDir(deep, { recursive: true });
        makeDir(deep, { recursive: true });
        rows([
            ["chain created", true, stat(deep).isDir],
            ["idempotent under recursive", true, stat(deep).isDir],
        ], (x) => x, "makeDir.recursive");

        assertThrows(() => remove(new Path(ROOT, "mk")), "remove [refuses a non-empty dir]");
        removeAll(new Path(ROOT, "mk"));
        removeAll(new Path(ROOT, "never-existed")); // "a missing path is a no-op"
        rows([
            ["tree removed", false, exists(new Path(ROOT, "mk"))],
            ["missing path no-op", false, exists(new Path(ROOT, "never-existed"))],
        ], (x) => x, "removeAll");

        // "Recursive, symlink-safe removal: never descends through a symlink"
        const victim = new Path(ROOT, "victim");
        makeDir(victim);
        writeFile(new Path(victim, "inside.txt"), "x");
        symlink(String(victim), new Path(ROOT, "victim.ln"));
        removeAll(new Path(ROOT, "victim.ln"));
        rows([
            ["link itself removed", false, exists(new Path(ROOT, "victim.ln"))],
            ["target survived (symlink-safe)", true, exists(new Path(victim, "inside.txt"))],
        ], (x) => x, "removeAll.symlink-safe");
        removeAll(victim);
    }

    /* ==================== readDir entry table ==================== */

    {
        // "Sorted entries; `.`/`..` excluded"; d_type flags
        const d = new Path(ROOT, "rd");
        makeDir(d);
        writeFile(new Path(d, "a.txt"), "1");
        writeFile(new Path(d, "z.log"), "2");
        makeDir(new Path(d, "m"));
        symlink("a.txt", new Path(d, "ln"));
        const entries = readDir(d);
        deepRows([
            ["sorted names, no dot entries", ["a.txt", "ln", "m", "z.log"], entries],
        ], (e) => e.map(x => x.name), "readDir");
        const byName = {};
        for (const e of entries) byName[e.name] = e;
        deepRows([
            ["a.txt flags", [false, true, false], byName["a.txt"]],
            ["z.log flags", [false, true, false], byName["z.log"]],
            ["m flags", [true, false, false], byName["m"]],
            // TEST-FIX: readDir reports lstat/readdir-d_type flags per entry (and must not
            // follow links -- a dangling symlink would otherwise make readDir throw). "ln" is a
            // symlink to the FILE a.txt, so [isDir=false, isFile=false, isSymlink=true]; the
            // previous row expected isDir=true, incoherent under either semantics.
            ["ln flags", [false, false, true], byName["ln"]],
        ], (e) => [e.isDir, e.isFile, e.isSymlink], "readDir.types");
        removeAll(d);
    }

    /* ==================== symlink / readLink / realPath / chmod ==================== */

    {
        writeFile(new Path(ROOT, "orig.txt"), "x");
        // "The stored target verbatim, as a string"
        symlink("orig.txt", new Path(ROOT, "ln.txt"));
        rows([
            ["verbatim target", "orig.txt", new Path(ROOT, "ln.txt")],
        ], readLink, "readLink");
        // "The fully resolved path as a Path": through the link and across ".."
        rows([
            ["resolves through the link", String(realPath(new Path(ROOT, "orig.txt"))), new Path(ROOT, "ln.txt")],
            // TEST-FIX: the d.ts member is `get dirname(): Path` ("the parent directory as a
            // new Path"); there is no `parent` member, so the previous row passed undefined.
            ["resolves interior dotdot", String(realPath(new Path(ROOT).dirname)), new Path(ROOT, "..")],
        ], (p) => String(realPath(p)), "realPath");
        // "Change permissions" round trip
        chmod(new Path(ROOT, "orig.txt"), 0o600);
        rows([
            ["0o600 sticks", 0o600, stat(new Path(ROOT, "orig.txt")).mode & 0o777],
        ], (x) => x, "chmod");
        chmod(new Path(ROOT, "orig.txt"), 0o644);
        assertEq(stat(new Path(ROOT, "orig.txt")).mode & 0o777, 0o644, "chmod [second mode]");
    }

    /* ==================== FileReader: string family vs byte family ==================== */

    {
        const linesP = new Path(ROOT, "lines.txt");
        writeFile(linesP, "alpha\nbeta\r\ngamma\nlast");
        // "The next line without its trailing newline; null at a clean EOF"; "CRLF handled"
        const r = new FileReader(linesP);
        const LINE_ROWS = [
            ["line 1", "alpha"], ["line 2 (CRLF)", "beta"], ["line 3", "gamma"],
            ["final line without newline", "last"], ["clean EOF", null],
        ];
        for (const [label, expected] of LINE_ROWS) assertEq(r.readLine(), expected, "readLine [" + label + "]");
        assertEq(r.closed, false, "readLine [open before close]");
        r.close();
        assertEq(r.closed, true, "readLine [close marks closed]");
        // DynResource contract: [Symbol.dispose] present, disposing closes (file header: "`using` is honored")
        const rd = new FileReader(linesP);
        assert(typeof rd[Symbol.dispose] === "function", "readLine [FileReader carries Symbol.dispose]");
        rd[Symbol.dispose]();
        assertEq(rd.closed, true, "readLine [Symbol.dispose releases]");

        // "Up to `n` bytes as a string, '' at EOF"; "The rest of the file"
        const r2 = new FileReader(linesP);
        deepRows([
            ["read(2) prefix", "al", r2.read(2)],
            ["readAll rest", "pha\nbeta\r\ngamma\nlast", r2.readAll()],
            ["read at EOF", "", r2.read()],
        ], (x) => x, "read/readAll");
        r2.close();

        // BYTE reads: "raw file bytes, no decode, no repair — a multi-byte sequence split at a
        // buffer edge arrives split" — é = [0xC3, 0xA9] read one byte at a time
        const utfP = new Path(ROOT, "utf8.txt");
        writeFile(utfP, u8(0xC3, 0xA9));
        const r3 = new FileReader(utfP);
        const one = u8(0);
        const INTO_ROWS = [
            ["first raw byte", [1, 0xC3]],
            ["split multibyte arrives split", [1, 0xA9]],
            ["EOF", [0, null]],
        ];
        for (const [label, [count, byte]] of INTO_ROWS) {
            assertEq(r3.readInto(one), count, "readInto [" + label + "] count");
            if (byte !== null) assertEq(one[0], byte, "readInto [" + label + "] byte");
        }
        assertEq(r3.readInto(new Uint8Array(0)), 0, "readInto [zero-length buffer reports 0 (documented)]");
        r3.close();

        // readBytes: "a fresh Uint8Array of up to n raw bytes, the empty array at EOF"
        const r4 = new FileReader(utfP);
        deepRows([
            ["readBytes(1) length", 1, r4.readBytes(1).length],
            // TEST-FIX: the readBytes(1) row already consumed the leading 0xC3, so the drain
            // returns the remaining 0xA9 only.
            ["readBytes() drains the rest", [0xA9], Array.from(r4.readBytes())],
            ["readBytes at EOF empty", [], Array.from(r4.readBytes())],
        ], (x) => x, "readBytes");
        r4.close();

        // "its own view bounds honored" — a DataView window
        const r5 = new FileReader(utfP);
        const ab = new ArrayBuffer(4);
        const win = new DataView(ab, 1, 2);
        assertEq(r5.readInto(win), 2, "readInto [DataView window count]");
        assert(eqArr(new Uint8Array(ab, 1, 2), [0xC3, 0xA9]), "readInto [DataView window bytes]");
        assertEq(new Uint8Array(ab)[0], 0, "readInto [byte before window untouched]");
        r5.close();

        // string path repairs, byte path never does
        const badP = new Path(ROOT, "bad.txt");
        writeFile(badP, u8(0xFF, 0xFE, 65));
        const r6 = new FileReader(badP);
        assertEq(r6.readAll(), "\uFFFD\uFFFDA", "readAll [invalid UTF-8 becomes U+FFFD (documented)]");
        r6.close();
        const r7 = new FileReader(badP);
        assert(eqArr(r7.readBytes(), [0xFF, 0xFE, 65]), "readBytes [byte path never repairs]");
        r7.close();

        // File handle surfaces: "f.reader([options]) / f.writer([options])"
        const hf = new File(new Path(ROOT, "w.handle"));
        const hw = hf.writer();
        hw.write("via-handle");
        hw.close();
        assertEq(hf.readText(), "via-handle", "FileWriter [File.writer() handle works]");
        const hr = hf.reader({ bufferSize: 4 }); // "loops internal fills" when buf exceeds the read buffer
        const big = new Uint8Array(10);
        rows([
            ["looped fills total", 10, hr.readInto(big)],
            ["looped fill content", "via-handle", String.fromCharCode(...big)],
        ], (x) => x, "reader.bufferSize");
        hr.close();
    }

    /* ==================== FileWriter: write flavors, modes, durability ==================== */

    {
        const outP = new Path(ROOT, "w.out");
        const w = new FileWriter(outP);
        // "Accepts a string, ArrayBuffer, or any TypedArray/DataView; returns bytes accepted"
        const FLAVORS = [
            ["string", "ab", 2],
            ["Uint8Array raw bytes", u8(0, 255), 2],
            ["ArrayBuffer", new ArrayBuffer(2), 2],
        ];
        for (const [label, data, count] of FLAVORS)
            assertEq(w.write(data), count, "FileWriter.write [" + label + "] count");
        const dvw = new DataView(new ArrayBuffer(4), 1, 2);
        new Uint8Array(dvw.buffer).set([9, 8], 1);
        assertEq(w.write(dvw), 2, "FileWriter.write [DataView window] count");
        w.flush();           // "Push buffered bytes to the fd"
        w.sync();            // "Flush then durable-sync"
        await w.syncAsync(); // "The same durability off the loop; returns a Promise"
        w.close();
        rows([
            ["closed flag", true, w.closed],
            ["all flavors byte-exact", true, eqArr(readBytes(outP), [97, 98, 0, 255, 0, 0, 9, 8])],
        ], (x) => x, "FileWriter");

        const w2 = new FileWriter(outP, { append: true }); // "{append: true} appends"
        w2.write("!");
        w2.close();
        assertEq(readFile(outP).length, 9, "FileWriter [append mode preserved prior bytes]");
        const preP = new Path(ROOT, "w.pre");
        const w3 = new FileWriter(preP, { preallocate: 64 });
        w3.write("tiny");
        w3.close();
        rows([
            ["preallocate content exact", "tiny", readFile(preP)],
        ], (x) => x, "FileWriter.preallocate");
    }

    /* ==================== glob: walk table + lexical matcher table ==================== */

    {
        const d = new Path(ROOT, "gl");
        makeDir(d, { recursive: true });
        writeFile(new Path(d, "one.txt"), "1");
        writeFile(new Path(d, "two.log"), "2");
        writeFile(new Path(d, ".hid.txt"), "3");
        makeDir(new Path(d, "nested"));
        writeFile(new Path(d, "nested", "three.txt"), "4");

        // Walk table: "Wildcards never match a leading `.` (minimatch rule)"; "** spans zero or
        // more segments"; "** skips dotfile entries outright"; last-segment ** emits directories;
        // results sorted. cwd is the only option; an empty pattern matches nothing.
        // TEST-FIX: with {cwd} the results come back as cwd-RELATIVE Paths ("one.txt"), the
        // pattern-relative form; the previous rows sliced an assumed absolute prefix off them.
        deepRows([
            ["dotfile skipped", ["one.txt"], Array.from(glob("*.txt", { cwd: d }), String)],
            ["both levels, sorted", ["nested/three.txt", "one.txt"], Array.from(glob("**/*.txt", { cwd: d }), String)],
            ["** emits dirs, skips dotfiles", ["nested", "nested/three.txt", "one.txt", "two.log"], Array.from(glob("**", { cwd: d }), String)],
            ["literal dot segment reaches dotfile", [".hid.txt"], Array.from(glob(".hid.txt", { cwd: d }), String)],
            ["empty pattern matches nothing", [], Array.from(glob("", { cwd: d }), String)],
        ], (x) => x, "glob.walk");

        // Lexical matcher: "purely LEXICAL ... '/' is an ordinary byte ... a star run may match the
        // EMPTY string ... NO leading-dot rule ... backslash is a literal". API.md pins a/**/c vs
        // a/c === false, x**y vs xy === true, *idden vs .hidden === true, **/b vs b === false.
        const LEX = [
            ["a/**/c vs a/c (pinned)", false, "a/c", "a/**/c"],
            ["a/**/c spans", true, "a/x/y/c", "a/**/c"],
            ["x**y vs xy (pinned empty run)", true, "xy", "x**y"],
            ["x**y spans slash", true, "x/y", "x**y"],
            ["no leading-dot rule (pinned)", true, ".hidden", "*idden"],
            ["literal", true, "abc", "abc"],
            ["literal mismatch", false, "abd", "abc"],
            ["**/b needs its slash (pinned)", false, "b", "**/b"],
            ["**/b with slash", true, "x/b", "**/b"],
            ["? is one char", true, "a.c", "a?c"],
            // TEST-FIX: "abc" DOES match "a?c" (? takes the 'b'); the zero-width refusal needs
            // a two-char input, which is the row that follows.
            ["? not zero chars", false, "ac", "a?c"],
            ["? not two chars", false, "axyc", "a?c"],
            ["[dl] class", true, "dog", "[dl]og"],
            ["[dl] class miss", false, "cog", "[dl]og"],
            ["[!dl] negation", false, "dog", "[!dl]og"],
            ["[a-c] range", true, "b", "[a-c]"],
        ];
        for (const [label, expected, path, pattern] of LEX)
            assertEq(Glob.match(path, pattern), expected, "Glob.lexical [" + label + "]");
        rows([
            ["hasWildcard plain", false, new Glob("plain").hasWildcard],
            ["hasWildcard star", true, new Glob("a*b").hasWildcard],
            ["filter keeps matches", 1, new Glob("*.bin").filter([new Path(d, "one.txt"), new Path(d, "two.log"), new Path(d, "x.bin")]).length],
            ["expand runs the walk", 1, new Glob("*.log").expand(d).length],
            ["pattern readonly", "*.log", new Glob("*.log").pattern],
        ], (x) => x, "Glob.props");
        removeAll(d);
    }

    /* ==================== sniffType table ==================== */

    {
        // "MIME type from magic bytes, not the extension"
        const SNIFF = [
            ["PNG magic", u8(0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a), "image/png"],
            ["JPEG magic", u8(0xFF, 0xD8, 0xFF, 0xE0), "image/jpeg"],
        ];
        for (const [label, bytes, mime] of SNIFF)
            assertEq(sniffType(bytes), mime, "sniffType [" + label + "]");
        const fake = new Path(ROOT, "fake.png");
        writeFile(fake, "just text, honestly");
        rows([
            ["extension never decides", true, sniffType(fake) !== "image/png"],
            ["path and bytes agree", true, sniffType(fake) === sniffType(readBytes(fake))],
        ], (x) => x, "sniffType.magic-not-extension");
    }

    /* ==================== async pair + asyncStats table ==================== */

    {
        const p = new Path(ROOT, "async.txt");
        const dst = new Path(ROOT, "async-copy.bin");
        const ASYNC_ROWS = [
            ["writeFileAsync count", () => writeFileAsync(p, "async-payload"), 13],
            ["readFileAsync text", () => readFileAsync(p), "async-payload"],
            ["writeFileAsync append count", () => writeFileAsync(p, "!", { append: true }), 1],
            ["append landed", () => readFile(p), "async-payload!"],
            ["copyFileAsync count", () => copyFileAsync(p, dst), 14],
            ["copyFileAsync overwrite", () => copyFileAsync(p, dst, { overwrite: true }), 14],
        ];
        for (const [label, fn, expected] of ASYNC_ROWS)
            assertEq(await fn(), expected, "async [" + label + "]");
        const raw = await readFileAsync(p, { bytes: true });
        assert(raw instanceof Uint8Array && raw.length === 14, "async [readFileAsync {bytes:true} resolves a Uint8Array]");
        let rejected = false;
        try { await copyFileAsync(p, dst); } catch { rejected = true; }
        assert(rejected, "async [copyFileAsync rejects on existing destination (documented refusal)]");

        // "The inline/offloaded counters and the 1 MiB thresholds"
        const s = asyncStats();
        rows([
            ["readMin", 1048576, s.readMin],
            ["writeMin", 1048576, s.writeMin],
            ["copyMin", 1048576, s.copyMin],
            ["inline counter type", "number", typeof s.inline],
            ["offloaded counter type", "number", typeof s.offloaded],
        ], (x) => x, "asyncStats");
    }

    /* ==================== File constructor from a string ==================== */

    {
        // "Accepts a Path or a string" (File only — free functions take Path)
        const fstr = new File(String(new Path(ROOT, "ctor-string.txt")));
        rows([
            ["path is a Path", true, fstr.path instanceof Path],
            ["write works", 1, fstr.writeText("s")],
            ["reads back", "s", fstr.readText()],
            ["toString is path string", String(fstr.path), fstr.toString()],
            ["toJSON is path string", String(fstr.path), fstr.toJSON()],
        ], (x) => x, "File.string-ctor");
        remove(fstr.path);
    }

    /* ==================== FileLock: contention + withLock release (d.ts:1735-1748) ==================== */

    {
        // "An advisory exclusive lock via flock(2)" — per-fd, so a second
        // FileLock IN THIS PROCESS contends too (per test_file_lock doctrine);
        // {retry: 0} refuses instead of waiting, which makes "the lock is
        // held" OBSERVABLE through the second constructor's refusal.
        const LP = new Path(ROOT, "fl.guard.lock");
        const holder = new FileLock(LP, { retry: 0 });
        // retry/retryMs arm the blocking retry loop; a product over the 24h
        // runaway cap refuses up front (d.ts: opts {retry?, retryMs?})
        let capped = null;
        try { new FileLock(new Path(ROOT, "fl.cap.lock"), { retry: 1e9, retryMs: 86400000 }); } catch (e) { capped = e; }
        assert(capped instanceof RangeError, "FileLock [retry*retryMs over the 24h cap refuses with RangeError — got |" + capped + "|]");
        let refused = null;
        try { new FileLock(LP, { retry: 0 }); } catch (e) { refused = e; }
        assert(refused !== null, "FileLock [second {retry:0} ctor refuses while the path is held]");
        assert(refused.code === "EWOULDBLOCK" || /temporarily unavailable/i.test(String(refused)),
            "FileLock [the refusal names the flock EWOULDBLOCK condition — got |" + refused + "|]");
        holder.close();
        const again = new FileLock(LP, { retry: 0 });
        assertEq(again.closed, false, "FileLock [the path is free again once the holder releases]");
        again.close();

        // "Calls fn, then releases the lock no matter what fn did; the lock
        // is consumed."
        const L2 = new Path(ROOT, "fl.fn.lock");
        const l = new FileLock(L2, { retry: 0 });
        assertEq(l.withLock(() => 42), 42, "FileLock [withLock returns fn's value]");
        assertEq(l.closed, true, "FileLock [withLock consumed the lock]");
        const l2 = new FileLock(L2, { retry: 0 });
        let propagated = null;
        try { l2.withLock(() => { throw new Error("boom"); }); } catch (e) { propagated = e; }
        assert(propagated !== null && propagated.message === "boom",
            "FileLock [withLock propagates fn's throw]");
        assertEq(l2.closed, true, "FileLock [withLock released even though fn threw]");
        // the release is observable: with {retry:0} the SAME path locks again
        const l3 = new FileLock(L2, { retry: 0 });
        l3.close();
    }

    /* ==================== Watcher: stop-then-iterate drain (d.ts:1749-1783) ==================== */

    {
        // stop(): "halt the watch and end the event stream with the Channel
        // close contract -- events ALREADY buffered still drain through
        // next() in arrival order, then { done: true }, so stop-then-iterate
        // always terminates. Idempotent; does not close, and the watcher can
        // start() again." Events are "delivered to BOTH surfaces -- the
        // start() callback and the async iteration", so the callback records
        // the ground-truth arrival order the drain must reproduce (the
        // classifier's per-scan emission order, not the write order).
        const withDeadline = (p, ms, what) =>
            Promise.race([p, sleep(ms).then(() => { throw new Error("deadline exceeded: " + what); })]);
        const wd = new Path(ROOT, "watchdrain");
        makeDir(wd);
        const w = new Watcher(wd, { debounceMs: 10 });
        const arrived = [];
        w.start((ev) => arrived.push(ev.path));
        // activate the iteration surface with one parked pull (seed 1 goes to
        // BOTH surfaces: the callback and the resolving pull)
        const firstPull = withDeadline(w.next(), 5000, "Watcher first pull");
        await sleep(80);
        writeFile(new Path(wd, "seed-1.txt"), "1");
        const first = await firstPull;
        assertEq(first.done, false, "Watcher [the parked pull resolves with the first event]");
        assertEq(first.value.path, "seed-1.txt", "Watcher [the first event names its file]");
        // three more files: buffered for the iteration, ALSO seen by the callback
        const SEED = ["seed-2.txt", "seed-3.txt", "seed-4.txt"];
        for (const name of SEED) writeFile(new Path(wd, name), "1");
        const t0 = Date.now();
        while (SEED.some((s) => arrived.indexOf(s) < 0) && Date.now() - t0 < 5000) await sleep(20);
        assert(SEED.every((s) => arrived.indexOf(s) >= 0),
            "Watcher [the callback surface saw all buffered seeds: " + JSON.stringify(arrived) + "]");
        const bufferedArrival = arrived.slice(arrived.indexOf(SEED[0])); // callback surface's view
        w.stop();
        w.stop(); // "Idempotent"
        const drained = [];
        for (;;) {
            const r = await withDeadline(w.next(), 5000, "Watcher drain next()");
            if (r.done) break;
            drained.push(r.value);
            assert(drained.length <= 64, "Watcher.stop-then-iterate [drain bounded — exceeded 64 events]");
        }
        // "in arrival order" is the iteration surface's OWN FIFO order (the
        // Channel close contract it names). It has no same-surface oracle --
        // pulling to observe it would consume the buffer -- and the callback
        // surface's delivery order is independently timed (probed: the two
        // surfaces can legitimately interleave scans differently), so the
        // rows pin the observable core: the buffered SET drains exactly once,
        // with nothing extra, and the stream then ends with { done: true }.
        rows([
            ["the buffered events drain after stop(), each exactly once", true,
             drained.length === SEED.length &&
             JSON.stringify(drained.map((ev) => ev.path).slice().sort()) === JSON.stringify(SEED.slice().sort())],
            ["every drained event is exactly { path, kind }", true,
             drained.every((ev) => JSON.stringify(Object.keys(ev).sort()) === JSON.stringify(["kind", "path"]))],
            ["every drained kind is one of the five", true,
             drained.every((ev) => ["change", "add", "addDir", "unlink", "unlinkDir"].indexOf(ev.kind) >= 0)],
            ["event path is a plain string (d.ts start() note)", true,
             drained.every((ev) => typeof ev.path === "string")],
        ], (x) => x, "Watcher.stop-then-iterate");
        const tail = await withDeadline(w.next(), 5000, "Watcher post-drain next()");
        assertEq(tail.done, true, "Watcher [stop-then-iterate terminates stably: next() after the drain is done]");
        assertEq(w.closed, false, "Watcher [stop() does not close]");
        w.close();
        assertEq(w.closed, true, "Watcher [close() closes]");
        removeAll(wd);
    }

} finally {
    removeAll(ROOT);
}

print("bb_file: all tests passed (" + n + " assertions)");
