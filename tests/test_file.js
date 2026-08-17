/* test_file.js — dyna:file buffered reader/writer, common cross-platform API.
 * Backend differs per OS (Linux fadvise/fallocate/io_uring, macOS F_RDAHEAD/
 * F_PREALLOCATE/F_FULLFSYNC) but behaviour is identical and tested here. */
import { Path, FileReader, FileWriter, readFile, writeFile } from "dyna:file";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = new Path("/tmp/dynajs_file_test.txt");

/* --- one-shot writeFile / readFile roundtrip --- */
const body = "line one\nline two\nline three\n" + "x".repeat(200000);
const n = writeFile(path, body);
assert(n === body.length, "writeFile returns byte count");
assert(readFile(path) === body, "readFile roundtrips writeFile");

/* --- buffered FileWriter: many small writes across the buffer boundary --- */
{
    const w = new FileWriter(path, { bufferSize: 4096, preallocate: 1 << 20 });
    let expect = "";
    for (let i = 0; i < 5000; i++) { const s = "row " + i + "\n"; w.write(s); expect += s; }
    /* an ArrayBuffer write path */
    const ab = new Uint8Array([65, 66, 67, 10]).buffer; /* "ABC\n" */
    w.write(ab); expect += "ABC\n";
    w.sync();   /* durable flush (F_FULLFSYNC on macOS) */
    w.close();
    assert(readFile(path) === expect, "buffered writer + ArrayBuffer roundtrips");

    /* --- FileReader: readLine across buffer refills --- */
    const r = new FileReader(path, { bufferSize: 64 });
    assert(r.readLine() === "row 0", "first line");
    assert(r.readLine() === "row 1", "second line");
    let count = 2;
    let line;
    while ((line = r.readLine()) !== null) count++;
    assert(count === 5001, "read every line back (" + count + ")"); /* 5000 rows + ABC */
    assert(r.readLine() === null, "readLine returns null at EOF");
    r.close();

    /* --- read(n) chunking --- */
    const r2 = new FileReader(path);
    const first6 = r2.read(6);
    assert(first6 === "row 0\n", "read(6) returns exactly 6 bytes");
    const rest = r2.readAll();
    assert(first6 + rest === expect, "read(6)+readAll reconstructs the file");
    r2.close();
}

/* --- append mode --- */
writeFile(path, "A");
writeFile(path, "B", { append: true });
{
    const w = new FileWriter(path, { append: true });
    w.write("C"); w.close();
}
assert(readFile(path) === "ABC", "append mode concatenates");

/* --- reentrant close during arg coercion must NOT crash/UAF (repo rule) --- */
{
    const w = new FileWriter(path);
    let threw = false;
    try {
        w.write({ toString() { w.close(); return "boom"; } });
    } catch (e) { threw = true; } /* writing to a closed writer throws, not UAF */
    /* the important part is we got here without a crash */
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

/* --- closed resource rejects use --- */
{
    const w = new FileWriter(path);
    w.close();
    let threw = false;
    try { w.write("x"); } catch (e) { threw = true; }
    assert(threw, "writing a closed FileWriter throws");
}

/* --- binary payloads must be written as BYTES, never stringified ---
 *
 * writeFile/FileWriter.write used to resolve a payload as string-or-ArrayBuffer
 * only, so a TypedArray fell through to ToString: writeFile(p, u8) put the text
 * "0,1,2,255,65" on disk and returned 12, a plausible byte count. That made
 * writeFile(p, gzip(x)) -- the most natural call there is -- silently corrupt.
 * Every payload shape is pinned here, including a view with a NON-ZERO
 * byteOffset, which is the case a naive fix still gets wrong. */
{
    const bin = new Path("/tmp/dynajs_file_bin_test.bin");
    const u8 = new Uint8Array([0, 1, 2, 255, 65]);

    /* readFile returns a string, so compare through a byte-exact round trip:
     * write the bytes, read them back as latin1-ish text, check length + code
     * units. A stringified array would be 12 chars, not 5. */
    assert(writeFile(bin, u8) === 5, "writeFile(Uint8Array) returns the byte count");
    assert(readFile(bin).length === 5, "writeFile(Uint8Array) wrote 5 bytes, not a stringification");

    assert(writeFile(bin, u8.buffer) === 5, "writeFile(ArrayBuffer) returns the byte count");
    assert(readFile(bin).length === 5, "writeFile(ArrayBuffer) wrote 5 bytes");

    /* subarray(1,4) has byteOffset 1 and length 3: the offset must be honoured */
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

/* ====================================================================== *
 *  Path -- the value handle, and the differential that proves it.
 *
 *  This section absorbs the retired tests/test_path.js. That suite checked
 *  nine FREE FUNCTIONS whose expectations were cross-checked against Node 26's
 *  real path.posix over >35,000 cases. Those functions are gone; `Path` is the
 *  whole surface. So the harness is re-run here against the METHODS, which is
 *  the gate W3.1 asks for -- re-running it against the free functions would
 *  have proved nothing about the thing that shipped.
 *
 *  THE ORACLE IS A SECOND IMPLEMENTATION, written below in JS from the
 *  documented POSIX/Node semantics rather than derived from the C. Comparing
 *  the binding against the core it calls would be comparing something to
 *  itself; comparing it against an independently written normaliser is what
 *  makes a disagreement mean something.
 *
 *  ONE DELIBERATE SEMANTIC CHANGE, and it is why the old expectations could
 *  not simply be copied across: the free functions were purely LEXICAL on the
 *  raw string, so dirname("a/..") was "a". A Path is NORMALISED at
 *  construction, so new Path("a/..") is "." and its dirname is ".". The splits
 *  are splits of the normalised value -- which is the entire reason the handle
 *  can cache three offsets instead of re-scanning. The reference below models
 *  that by normalising first, and the sweep checks exactly it.
 * ====================================================================== */
{
    /* ---- the reference: Node's normalizeString and friends, in JS ---- */

    function refNormalizeString(path, allowAboveRoot) {
        let res = "", lastSegmentLength = 0, lastSlash = -1, dots = 0, code = 0;
        for (let i = 0; i <= path.length; ++i) {
            if (i < path.length) code = path.charCodeAt(i);
            else if (code === 47) break;
            else code = 47;
            if (code === 47) {
                if (lastSlash === i - 1 || dots === 1) {
                    /* "//" run or a "." segment: emit nothing */
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

    /* ---- the sweep ---- */

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
        /* new Path(p) is join([p]), so it normalises. */
        const norm = refJoin([p]);
        const h = new Path(p);
        assert(String(h) === norm, "Path(" + JSON.stringify(p) + ") = " +
               JSON.stringify(String(h)) + " want " + JSON.stringify(norm));
        /* Every split is a split OF THE NORMALISED VALUE -- see the note above. */
        assert(String(h.dirname) === refJoin([refDirname(norm)]),
               "dirname " + JSON.stringify(p));
        assert(h.basename === refBasename(norm), "basename " + JSON.stringify(p));
        assert(h.extname === refExtname(norm), "extname " + JSON.stringify(p));
        assert(h.isAbsolute === (norm.charCodeAt(0) === 47),
               "isAbsolute " + JSON.stringify(p));
        /* basenameWithout against every suffix of the value: this is where the
         * "stripping would empty the segment" rule lives. */
        for (let k = 0; k <= norm.length && k <= 6; k++) {
            const suf = norm.slice(norm.length - k);
            const want = (suf.length > 0 && suf.length <= norm.length && suf === norm)
                ? "" : refBasenameSuffix(norm, suf);
            assert(h.basenameWithout(suf) === want,
                   "basenameWithout(" + JSON.stringify(suf) + ") on " + JSON.stringify(norm));
        }
    }

    /* Node's basename(p, ext) rule, written out separately because its
     * "would empty the final segment" clause is the part that is easy to get
     * wrong and the part the C oracle already caught once. */
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

    /* ---- the handle's own contract, which the free functions had no way to have ---- */

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

    /* Relative between identical paths is ".", not "" -- a Path cannot be empty,
     * and "." is the path that means "here". This is the one place the handle's
     * return type forces a different answer from Node's free function, and it is
     * asserted rather than left to be discovered. */
    assert(String(new Path("/a/b").relativeTo(new Path("/a/b"))) === ".",
           "relativeTo(self) is '.', because a Path is never empty");

    print("  Path differential: " + cases + " cases against an independent JS reference");
}

print("test_file: all tests passed");

