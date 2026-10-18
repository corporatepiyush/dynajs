// flags: --std
import * as std from "std";
import { Path, FileReader, FileWriter, FileLock, readFile, readBytes, writeFile, copyFile, makeDir, File, exists, remove, removeAll, glob } from "dyna:file";
import * as os from "os";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_file.${Date.now() % 10000000}.txt`);

const body = "line one\nline two\nline three\n" + "x".repeat(200000);
const n = writeFile(path, body);
assert(n === body.length, "writeFile returns byte count");
assert(readFile(path) === body, "readFile roundtrips writeFile");

{
    const w = new FileWriter(path, { bufferSize: 4096, preallocate: 1 << 20 });
    let expect = "";
    for (let i = 0; i < 5000; i++) { const s = "row " + i + "\n"; w.write(s); expect += s; }
    const ab = new Uint8Array([65, 66, 67, 10]).buffer;
    w.write(ab); expect += "ABC\n";
    w.sync();
    w.close();
    assert(readFile(path) === expect, "buffered writer + ArrayBuffer roundtrips");

    const r = new FileReader(path, { bufferSize: 64 });
    assert(r.readLine() === "row 0", "first line");
    assert(r.readLine() === "row 1", "second line");
    let count = 2;
    let line;
    while ((line = r.readLine()) !== null) count++;
    assert(count === 5001, "read every line back (" + count + ")");
    assert(r.readLine() === null, "readLine returns null at EOF");
    r.close();

    const r2 = new FileReader(path);
    const first6 = r2.read(6);
    assert(first6 === "row 0\n", "read(6) returns exactly 6 bytes");
    const rest = r2.readAll();
    assert(first6 + rest === expect, "read(6)+readAll reconstructs the file");
    r2.close();
}

writeFile(path, "A");
writeFile(path, "B", { append: true });
{
    const w = new FileWriter(path, { append: true });
    w.write("C"); w.close();
}
assert(readFile(path) === "ABC", "append mode concatenates");

{
    const w = new FileWriter(path);
    let threw = false;
    try {
        w.write({ toString() { w.close(); return "boom"; } });
    } catch (e) { threw = true; }
    assert(threw || w.closed, "write() coerces before resolving the handle");
}
{
    writeFile(path, "hello world");
    const r = new FileReader(path);
    let ok = true;
    try { r.read({ valueOf() { r.close(); return 3; } }); }
    catch (e) { ok = true; }
    assert(r.closed, "read() coerces the count before resolving the handle");
}

{
    const w = new FileWriter(path);
    w.close();
    let threw = false;
    try { w.write("x"); } catch (e) { threw = true; }
    assert(threw, "writing a closed FileWriter throws");
}

{
    const bin = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_file.${Date.now() % 10000000}.bin`);
    const u8 = new Uint8Array([0, 1, 2, 255, 65]);

    assert(writeFile(bin, u8) === 5, "writeFile(Uint8Array) returns the byte count");
    assert(readFile(bin).length === 5, "writeFile(Uint8Array) wrote 5 bytes, not a stringification");

    assert(writeFile(bin, u8.buffer) === 5, "writeFile(ArrayBuffer) returns the byte count");
    assert(readFile(bin).length === 5, "writeFile(ArrayBuffer) wrote 5 bytes");

    assert(writeFile(bin, u8.subarray(1, 4)) === 3, "writeFile(subarray) returns the view length");
    assert(readFile(bin).length === 3, "writeFile(subarray) wrote 3 bytes from the right offset");

    assert(writeFile(bin, new Int32Array([1, 2]))=== 8, "writeFile(Int32Array) writes 8 raw bytes");
    assert(readFile(bin).length === 8, "a non-uint8 TypedArray writes its raw backing bytes");

    {
        const w = new FileWriter(bin);
        assert(w.write(u8) === 5, "FileWriter.write(Uint8Array) returns the byte count");
        w.close();
        assert(readFile(bin).length === 5, "FileWriter.write(Uint8Array) wrote bytes, not text");
    }
}

{
    function refNormalizeString(path, allowAboveRoot) {
        let res = "", lastSegmentLength = 0, lastSlash = -1, dots = 0, code = 0;
        for (let i = 0; i <= path.length; ++i) {
            if (i < path.length) code = path.charCodeAt(i);
            else if (code === 47) break;
            else code = 47;
            if (code === 47) {
                if (lastSlash === i - 1 || dots === 1) {
                } else if (dots === 2) {
                    if (res.length < 2 || lastSegmentLength !== 2 ||
                        res.charCodeAt(res.length - 1) !== 46 ||
                        res.charCodeAt(res.length - 2) !== 46) {
                        if (res.length > 2) {
                            const idx = res.lastIndexOf("/");
                            if (idx !== res.length - 1) {
                                if (idx === -1) { res = ""; lastSegmentLength = 0; }
                                else {
                                    res = res.slice(0, idx);
                                    lastSegmentLength = res.length - 1 - res.lastIndexOf("/");
                                }
                                lastSlash = i; dots = 0; continue;
                            }
                        } else if (res.length === 2 || res.length === 1) {
                            res = ""; lastSegmentLength = 0; lastSlash = i; dots = 0; continue;
                        }
                    }
                    if (allowAboveRoot) {
                        res += res.length > 0 ? "/.." : "..";
                        lastSegmentLength = 2;
                    }
                } else {
                    const seg = path.slice(lastSlash + 1, i);
                    res += res.length > 0 ? "/" + seg : seg;
                    lastSegmentLength = seg.length;
                }
                lastSlash = i; dots = 0;
            } else if (code === 46 && dots !== -1) { ++dots; }
            else { dots = -1; }
        }
        return res;
    }

    function refNormalize(p) {
        if (p.length === 0) return ".";
        const isAbs = p.charCodeAt(0) === 47;
        const trailing = p.charCodeAt(p.length - 1) === 47;
        let core = refNormalizeString(p, !isAbs);
        if (core.length === 0) return isAbs ? "/" : (trailing ? "./" : ".");
        return (isAbs ? "/" : "") + core + (trailing ? "/" : "");
    }

    function refJoin(parts) {
        const kept = parts.filter(x => x.length > 0);
        if (kept.length === 0) return ".";
        return refNormalize(kept.join("/"));
    }

    function refResolve(parts) {
        let root = -1;
        for (let i = parts.length - 1; i >= 0; i--)
            if (parts[i].length > 0 && parts[i].charCodeAt(0) === 47) { root = i; break; }
        let raw, start;
        if (root === -1) { raw = "/"; start = 0; }
        else { raw = ""; start = root; }
        const pieces = [];
        if (root === -1) pieces.push("");
        for (let i = start; i < parts.length; i++)
            if (parts[i].length > 0) pieces.push(parts[i]);
        raw = (root === -1 ? "/" : "") + pieces.filter(x => x.length).join("/");
        if (root === -1) raw = "/" + pieces.filter(x => x.length).join("/");
        return "/" + refNormalizeString(raw, false);
    }

    function refDirname(p) {
        if (p.length === 0) return ".";
        const hasRoot = p.charCodeAt(0) === 47;
        let end = -1, matched = true;
        for (let i = p.length - 1; i >= 1; --i) {
            if (p.charCodeAt(i) === 47) { if (!matched) { end = i; break; } }
            else matched = false;
        }
        if (end === -1) return hasRoot ? "/" : ".";
        if (hasRoot && end === 1) return "//";
        return p.slice(0, end);
    }

    function refBasename(p) {
        let start = 0, end = -1, matched = true;
        for (let i = p.length - 1; i >= 0; --i) {
            if (p.charCodeAt(i) === 47) { if (!matched) { start = i + 1; break; } }
            else if (end === -1) { matched = false; end = i + 1; }
        }
        return end === -1 ? "" : p.slice(start, end);
    }

    function refExtname(p) {
        let startDot = -1, startPart = 0, end = -1, matched = true, preDot = 0;
        for (let i = p.length - 1; i >= 0; --i) {
            const c = p.charCodeAt(i);
            if (c === 47) { if (!matched) { startPart = i + 1; break; } continue; }
            if (end === -1) { matched = false; end = i + 1; }
            if (c === 46) { if (startDot === -1) startDot = i; else if (preDot !== 1) preDot = 1; }
            else if (startDot !== -1) preDot = -1;
        }
        if (startDot === -1 || end === -1 || preDot === 0 ||
            (preDot === 1 && startDot === end - 1 && startDot === startPart + 1))
            return "";
        return p.slice(startDot, end);
    }

    function refRelative(from, to) {
        const f = refResolve([from]), t = refResolve([to]);
        if (f === t) return "";
        const fl = f.length - 1, tl = t.length - 1;
        const smallest = Math.min(fl, tl);
        let lastCommon = -1, i = 0;
        for (; i < smallest; i++) {
            const fc = f.charCodeAt(1 + i);
            if (fc !== t.charCodeAt(1 + i)) break;
            if (fc === 47) lastCommon = i;
        }
        if (i === smallest) {
            if (tl > smallest) {
                if (t.charCodeAt(1 + i) === 47) return t.slice(1 + i + 1);
                if (i === 0) return t.slice(1 + i);
            } else if (fl > smallest) {
                if (f.charCodeAt(1 + i) === 47) lastCommon = i;
                else if (i === 0) lastCommon = 0;
            }
        }
        let out = "";
        for (let k = 1 + lastCommon + 1; k <= f.length; ++k)
            if (k === f.length || f.charCodeAt(k) === 47)
                out += out.length === 0 ? ".." : "/..";
        return out + t.slice(1 + lastCommon);
    }

    const ALPHA = ["a", "b", ".", "/"];
    function every(len, fn) {
        const total = Math.pow(4, len);
        const buf = new Array(len);
        for (let i = 0; i < total; i++) {
            let v = i;
            for (let k = 0; k < len; k++) { buf[k] = ALPHA[v & 3]; v >>= 2; }
            fn(buf.join(""));
        }
    }

    const EDGE = ["", "/", "//", "///", ".", "..", "...", "./", "../", "/.", "/..",
                  "a/..", "a/../..", "/a/../../..", "....", ".bashrc", "a.b.c",
                  "/foo/.html", "foo/bar//baz", "foo/bar/./baz/", "/////a/////b/////",
                  "a/b/../../../../c", "////", "/a//b//c//", ".hidden/", "x/.y/.z"];

    let cases = 0;
    function checkOne(p) {
        cases++;
        const norm = refJoin([p]);
        const h = new Path(p);
        assert(String(h) === norm, "Path(" + JSON.stringify(p) + ") = " +
               JSON.stringify(String(h)) + " want " + JSON.stringify(norm));
        assert(String(h.dirname) === refJoin([refDirname(norm)]),
               "dirname " + JSON.stringify(p));
        assert(h.basename === refBasename(norm), "basename " + JSON.stringify(p));
        assert(h.extname === refExtname(norm), "extname " + JSON.stringify(p));
        assert(h.isAbsolute === (norm.charCodeAt(0) === 47),
               "isAbsolute " + JSON.stringify(p));
        for (let k = 0; k <= norm.length && k <= 6; k++) {
            const suf = norm.slice(norm.length - k);
            const want = (suf.length > 0 && suf.length <= norm.length && suf === norm)
                ? "" : refBasenameSuffix(norm, suf);
            assert(h.basenameWithout(suf) === want,
                   "basenameWithout(" + JSON.stringify(suf) + ") on " + JSON.stringify(norm));
        }
    }

    function refBasenameSuffix(p, suf) {
        if (suf.length === 0 || suf.length > p.length) return refBasename(p);
        if (suf === p) return "";
        const base = refBasename(p);
        if (base.length > suf.length && base.endsWith(suf))
            return base.slice(0, base.length - suf.length);
        return base;
    }

    function checkPair(a, b) {
        cases++;
        const na = refJoin([a]);
        assert(String(new Path(a, b)) === refJoin([a, b]),
               "join " + JSON.stringify([a, b]));
        assert(String(new Path(a).join(b)) === refJoin([na, b]),
               ".join " + JSON.stringify([a, b]));
        assert(String(new Path(a).resolve(b)) === refResolve([na, b]),
               ".resolve " + JSON.stringify([a, b]));
        const want = refRelative(na, refJoin([b])) || ".";
        assert(String(new Path(a).relativeTo(new Path(b))) === want,
               ".relativeTo " + JSON.stringify([a, b]) + " want " + JSON.stringify(want));
    }

    for (let L = 0; L <= 6; L++) every(L, checkOne);
    for (const e of EDGE) checkOne(e);
    for (let La = 0; La <= 3; La++)
        for (let Lb = 0; Lb <= 3; Lb++)
            every(La, a => every(Lb, b => checkPair(a, b)));
    for (const a of EDGE) for (const b of EDGE) checkPair(a, b);

    assert(new Path("a//b/./c").equals(new Path("a/b/c")),
           "two spellings of one path are equal, because both are normalised");
    assert(!new Path("a/b").equals(new Path("a/c")), "different paths are not equal");
    assert(new Path("a/b").equals("a/b") === false,
           "a string is not a Path, so it is not equal to one");
    assert(String(new Path(new Path("x/y"))) === "x/y",
           "new Path(aPath) shares rather than re-normalises");
    assert(Path.isPath(new Path("x")) && !Path.isPath("x") && !Path.isPath(null),
           "Path.isPath discriminates");
    assert(Path.sep === "/" && Path.delimiter === ":", "sep and delimiter");
    assert(Path.cwd().isAbsolute && Path.home().isAbsolute && Path.temp().isAbsolute,
           "the OS-derived statics are absolute");
    assert(JSON.stringify({ p: new Path("/a/b") }) === '{"p":"/a/b"}',
           "toJSON makes a Path serialise as its string");
    assert(`${new Path("/a/b")}` === "/a/b", "template interpolation goes through toString");
    assert(new Path("/a//b")[Symbol.toPrimitive]("string") === "/a/b",
           "@@toPrimitive is explicit, so every hint yields the normalised path");
    assert(new Path("/a//b")[Symbol.toPrimitive]("number") === "/a/b",
           "...including the number hint, which would otherwise route via NaN");

    assert(String(new Path("/a/b").relativeTo(new Path("/a/b"))) === ".",
           "relativeTo(self) is '.', because a Path is never empty");

    print("  Path differential: " + cases + " cases against an independent JS reference");
}

{
    const cap = 1 << 30;
    const big = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_cap_over.${Date.now() % 10000000}.bin`);
    const ok  = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_cap_ok.${Date.now() % 10000000}.bin`);

    let ran = false;
    try {
        const trunc = (path, size) =>
            os.exec(["truncate", "-s", String(size), String(path)]);

        if (trunc(ok, 8 * 1024 * 1024) === 0) {
            const b = new File(ok).readBytes();
            assert(b.length === 8 * 1024 * 1024, "an under-cap file reads fully");
            assert(b.every(x => x === 0), "the under-cap bytes are intact");
            ran = true;
        }

        if (trunc(big, cap + 1) === 0) {
            const t0 = Date.now();
            let threw = false, msg = "";
            try { new File(big).readBytes(); }
            catch (e) { threw = true; msg = String(e.message); }
            const ms = Date.now() - t0;
            assert(threw, "a file over the cap is refused, not read");
            assert(/exceeds|range/i.test(msg),
                   "the refusal names the cap (got: " + msg + ")");
            assert(ms < 1000, "refusal is fast, not a 2 GiB malloc/read (" + ms + "ms)");
            ran = true;
        }
    } finally {
        if (exists(big)) remove(big);
        if (exists(ok)) remove(ok);
    }
    if (!ran)
        print("  (P1b-2 cap proof skipped: truncate(1) unavailable)");
}

{
    const dvPath = new Path(String(path) + ".dv");
    const w = new FileWriter(dvPath);
    w.write("A");
    w.write(new DataView(new Uint8Array([66, 67]).buffer));
    w.close();
    assert(readFile(dvPath) === "ABC",
           "a DataView writes its bytes, not '[object DataView]'");
    remove(dvPath);
}


{
    const bin = new Path(`${String(path)}.fl1.bin`);
    const raw = new Uint8Array([0, 1, 2, 254, 255, 0, 13, 10, 200, 100]);
    writeFile(bin, raw);

    const rb = readFile(bin, { bytes: true });
    assert(rb instanceof Uint8Array, "readFile {bytes:true} returns a Uint8Array");
    assert(rb.length === raw.length, "readFile {bytes:true} keeps the length");
    assert(rb.every((v, i) => v === raw[i]), "readFile {bytes:true} round-trips invalid-UTF-8 bytes");

    const asText = readFile(bin);
    assert(asText.includes("\uFFFD"), "the string form mangles invalid bytes to U+FFFD");
    assert(!rb.includes(0xef && 0xff) || rb[4] === 255, "the bytes form keeps the raw 0xFF");

    const free = readBytes(bin);
    assert(free instanceof Uint8Array && free.length === 10 && free[3] === 254,
           "free readBytes(path) matches readFile {bytes:true}");

    const txt = new Path(`${String(path)}.fl1.txt`);
    writeFile(txt, "héllo");
    assert(readFile(txt) === "héllo", "bare readFile stays a string");
    assert(readFile(txt, {}) === "héllo", "empty options bag = no options");
    assert(readFile(txt, null) === "héllo", "null options = no options");
    assert(readFile(txt, { bytes: false }) === "héllo", "bytes:false is the string form");

    assert(new File(bin).readBytes().every((v, i) => v === raw[i]),
           "File.readBytes equals free readBytes");
    let threw = false;
    try { readBytes(new File(bin)); } catch (e) { threw = /must be a Path/.test(e.message); }
    assert(threw, "the module stays Path-only: a File is not a Path");
    remove(bin); remove(txt);
}

{
    const dir = new Path(Path.temp(), "dj_fl3." + (Date.now() % 10000000));
    makeDir(dir, { recursive: true });
    const src = new File(new Path(dir, "src.txt"));
    src.writeText("v1");
    const dst = new Path(dir, "dst.txt");
    const c1 = src.copyTo(dst);
    assert(c1 instanceof File, "copyTo returns a new File");
    assert(readFile(dst) === "v1", "copyTo copies");
    assert(String(c1.path) === String(dst), "copyTo's File names the destination");

    let threw = false;
    try { src.copyTo(dst); } catch (e) { threw = e.code === "EEXIST" || /exists/i.test(e.message); }
    assert(threw, "copyTo refuses an existing destination by default (EEXIST)");

    const c2 = src.copyTo(dst, { overwrite: true });
    assert(c2 instanceof File && readFile(dst) === "v1", "copyTo {overwrite:true} replaces");
    src.writeText("a-much-longer-v2-payload");
    src.copyTo(dst, { overwrite: true });
    assert(readFile(dst) === "a-much-longer-v2-payload", "overwrite truncates the old contents fully");
    assert(readFile(new Path(dir, "src.txt")) === "a-much-longer-v2-payload", "copyTo leaves the source");

    const moved = src.moveTo(new Path(dir, "moved.txt"));
    assert(moved === src, "moveTo returns this");
    assert(String(src.path).endsWith("moved.txt"), "moveTo rebinds the handle");
    removeAll(dir);
}

{
    const t = new Path(`${String(path)}.strict.txt`);
    writeFile(t, "x");
    const dir = new Path(Path.temp(), "dj_strict." + (Date.now() % 10000000));
    makeDir(dir, { recursive: true });

    function throwsStrict(fn, key, valid, what) {
        let msg = "";
        try { fn(); } catch (e) {
            msg = String(e.message);
            assert(e instanceof TypeError, what + ": a TypeError");
            assert(msg.includes(`unknown option "${key}"`), what + `: names the key (${msg})`);
            assert(msg.includes(`(valid: ${valid})`), what + `: names the valid keys (${msg})`);
            return;
        }
        assert(false, what + ": must throw, did not");
    }

    throwsStrict(() => readFile(t, { byte: true }), "byte", "bytes", "readFile bag");
    throwsStrict(() => readFile(t, { bytes: true,encoding: "x" }), "encoding", "bytes", "readFile bag (2nd key)");
    throwsStrict(() => new FileReader(t, { buffersize: 16 }).close(), "buffersize", "bufferSize", "FileReader ctor bag");
    throwsStrict(() => new FileWriter(t, { prealloca: 8 }).close(), "prealloca", "bufferSize, preallocate, append", "FileWriter ctor bag");
    throwsStrict(() => writeFile(t, "x", { appends: true }), "appends", "append", "writeFile bag");
    throwsStrict(() => makeDir(dir, { recursiv: true }), "recursiv", "recursive, mode", "makeDir bag");
    throwsStrict(() => glob("**", { cwd2: dir }), "cwd2", "cwd", "glob bag");
    throwsStrict(() => copyFile(t, new Path(dir, "z"), { overwrit: true }), "overwrit", "overwrite", "copyFile bag");
    throwsStrict(() => new FileLock(t, { retr: 1 }), "retr", "retry, retryMs", "FileLock ctor bag");
    throwsStrict(() => new File(t).writer({ apend: true }).close(), "apend", "bufferSize, preallocate, append", "writer() bag");
    throwsStrict(() => new File(t).reader({ bufferSize: 1, bogus: 2 }).close(), "bogus", "bufferSize", "reader() bag");

    let evil = false;
    try {
        writeFile(t, "y", { get append() { throw new RangeError("boom"); } });
    } catch (e) { evil = e instanceof RangeError; }
    assert(evil, "a throwing getter on a known boolean key propagates");
    evil = false;
    try {
        new FileReader(t, { bufferSize: { valueOf() { throw new RangeError("boom"); } } }).close();
    } catch (e) { evil = e instanceof RangeError; }
    assert(evil, "a throwing valueOf on a known number key propagates");

    assert(readFile(t, undefined) === "x", "undefined opts = no options");
    assert(readFile(t, null) === "x", "null opts = no options");

    remove(t);
    removeAll(dir);
}

print("test_file: all tests passed");

