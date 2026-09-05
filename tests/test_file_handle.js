/* test_file_handle.js -- class File and class Glob (W3.5/W3.6).
 *
 * File is a VALUE HANDLE over one path; Glob is a COMPILED CAPABILITY over one
 * pattern. They are in the same file because they are the two halves of W3
 * that sit on top of Path, and because the contrast is the point: a handle
 * must be free to use, a capability has a crossover to publish.
 *
 * File delegates to the free functions rather than reimplementing them, so
 * the thing to test is that the delegation is FAITHFUL -- every method must
 * equal the free function it wraps, or there are two implementations of
 * readFile and only one of them is tested.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_file_handle.js
 */
import {
    Path, File, Glob, readFile, writeFile, stat, exists, remove,
    makeTempDir, removeAll, makeDir, glob, realPath,
} from "dyna:file";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (e === null) throw new Error("assertion failed: " + msg + " (expected a throw)");
    if (!String(e.message).includes(needle))
        throw new Error("assertion failed: " + msg + " (message was: " + e.message + ")");
}

const root = makeTempDir("dyna-file-handle-");
let ok = false;
try {

/* ---- File: the delegation must be faithful ----------------------------- */
{
    const p = root.join("a.txt");
    const f = new File(p);

    eq(f.writeText("hello"), 5, "writeText returns the byte count, like writeFile");
    eq(f.readText(), readFile(p), "readText EQUALS readFile on the same path");
    eq(f.exists(), exists(p), "exists equals the free function");
    eq(f.stat().size, stat(p).size, "stat equals the free function");
    eq(String(f.realPath()), String(realPath(p)), "realPath equals the free function");
    assert(Path.isPath(f.path), ".path is a Path");
    assert(f.path.equals(p), "and it is the one we constructed with");
    eq(String(f), String(p), "toString is the path");
    eq(JSON.stringify({ f }), JSON.stringify({ f: String(p) }), "toJSON too");

    /* append() supplies {append:true} so the common case cannot be spelled
     * wrong -- and it must equal doing so by hand. */
    f.append(" world");
    eq(f.readText(), "hello world", "append concatenates");
    writeFile(p, "hello");
    writeFile(p, " world", { append: true });
    eq(f.readText(), "hello world", "append equals writeFile({append:true})");
}

/* ---- readBytes / writeBytes are BINARY, and that is the whole point ----- */
{
    const f = new File(root.join("bin.dat"));
    /* 0xFF is not valid UTF-8. An implementation that routed readBytes through
     * readText would decode it to U+FFFD and hand back SEVEN bytes
     * (1,2,239,191,189,0,3) instead of five -- silent corruption, and exactly
     * what the first version of this method did. */
    const payload = new Uint8Array([1, 2, 255, 0, 3]);
    f.writeBytes(payload);
    const got = f.readBytes();
    eq(got.length, 5, "readBytes returns the byte count, not a re-encoded string");
    eq(Array.from(got).join(","), "1,2,255,0,3", "every byte survives, 0xFF included");
    eq(got.constructor.name, "Uint8Array", "and it is a Uint8Array");

    /* Every byte value, so no single value can be quietly mangled. */
    const all = new Uint8Array(256);
    for (let i = 0; i < 256; i++) all[i] = i;
    f.writeBytes(all);
    const back = f.readBytes();
    eq(back.length, 256, "all 256 byte values round-trip");
    let same = true;
    for (let i = 0; i < 256; i++) if (back[i] !== i) same = false;
    assert(same, "and each one is itself");

    eq(f.readBytes().length, 256, "readBytes is repeatable");
    f.writeText("text");
    eq(f.readText(), "text", "writeText still works on the same handle");
    f.remove();
}

/* ---- a string is accepted by the CONSTRUCTOR only ---------------------- */
{
    const p = root.join("s.txt");
    writeFile(p, "x");
    /* new File("...") is the one place a string is unambiguous: it is a
     * constructor whose entire subject is the path. */
    eq(new File(String(p)).readText(), "x", "new File(string) builds the Path for you");
    eq(new File(p).readText(), "x", "new File(Path) too");
    throwsWith(() => new File(42), "must be a Path or a string", "a number is refused");
    throwsWith(() => new File(), "requires a Path", "no argument is refused");
    /* ...and everywhere ELSE still demands a Path. */
    throwsWith(() => readFile(String(p)), "must be a Path",
        "the free functions are unchanged: still Path-only");
}

/* ---- copyTo / moveTo ---------------------------------------------------- */
{
    const src = new File(root.join("src.txt"));
    src.writeText("payload");
    const dstPath = root.join("dst.txt");
    const dst = src.copyTo(dstPath);

    assert(dst instanceof File || Path.isPath(dst.path), "copyTo returns a File");
    eq(dst.readText(), "payload", "the copy has the content");
    eq(src.readText(), "payload", "and the original survives");

    /* A copy must be a COPY: writing through one must not be visible through
     * the other. A hard link would pass a naive content check and fail this. */
    dst.writeText("changed");
    eq(src.readText(), "payload", "writing the copy does not touch the original");

    const movedTo = root.join("moved.txt");
    const same = src.moveTo(movedTo);
    assert(same.path.equals(movedTo), "moveTo RETARGETS the handle to the new path");
    eq(same.readText(), "payload", "and it reads from there");
    assert(!exists(root.join("src.txt")), "the old path is gone");
}

/* ---- reader / writer ---------------------------------------------------- */
{
    const f = new File(root.join("stream.txt"));
    const w = f.writer();
    w.write("line one\n");
    w.write("line two\n");
    w.close();
    const r = f.reader();
    eq(r.readLine(), "line one", "the stream round-trips through the handle");
    eq(r.readLine(), "line two", "second line");
    r.close();
    eq(f.readText(), "line one\nline two\n", "and readText agrees with the stream");
}

/* ---- remove ------------------------------------------------------------- */
{
    const f = new File(root.join("gone.txt"));
    f.writeText("x");
    assert(f.exists(), "created");
    f.remove();
    assert(!f.exists(), "removed");
    /* The handle stays valid -- it names a path, not an open descriptor, so a
     * removed file can simply be written again. */
    f.writeText("again");
    eq(f.readText(), "again", "a File is a path, not a descriptor");
}

/* ---- Glob: compiled once, matched against unbounded paths --------------- */
{
    makeDir(root.join("g"));
    makeDir(root.join("g", "sub"));
    writeFile(root.join("g", "one.txt"), "1");
    writeFile(root.join("g", "two.txt"), "2");
    writeFile(root.join("g", "top.js"), "3");
    writeFile(root.join("g", "sub", "deep.js"), "4");
    const base = root.join("g");

    const gl = new Glob("*.txt");
    eq(gl.pattern, "*.txt", "the pattern is readable back");
    eq(gl.hasWildcard, true, "and the wildcard flag is computed once, at construction");
    eq(new Glob("exact.txt").hasWildcard, false, "a literal pattern has none");

    /* matches() is PURELY LEXICAL -- no filesystem access -- so it works on a
     * path that does not exist. That is what makes it usable as a filter. */
    eq(gl.matches(new Path("one.txt")), true, "matches a name");
    eq(gl.matches(new Path("one.js")), false, "rejects another");
    eq(gl.matches(new Path("does-not-exist-anywhere.txt")), true,
        "matches is lexical: it never touches the disk");

    /* expand() DOES walk the disk, and must agree with the free glob(). */
    const viaClass = gl.expand(base).map(String).sort();
    const viaFree = glob("*.txt", { cwd: base }).map(String).sort();
    eq(viaClass.join(","), viaFree.join(","), "expand equals the free glob()");
    eq(viaClass.join(","), "one.txt,two.txt", "and finds the right files");

    /* filter() is expand's lexical twin over a list the caller already has. */
    const filtered = gl.filter([new Path("a.txt"), new Path("b.js"), new Path("c.txt")]);
    eq(filtered.map(String).join(","), "a.txt,c.txt", "filter keeps the matches");
    eq(filtered.every(Path.isPath), true, "and yields Paths");
    eq(gl.filter([]).length, 0, "an empty list filters to empty");

    /* ** spans directories; * does not. */
    /* ** matches ZERO or more directories, so it finds top.js as well as the
     * nested one. That is the standard meaning and the reason ** exists. */
    eq(new Glob("**/*.js").expand(base).map(String).sort().join(","),
        "sub/deep.js,top.js", "** spans zero or more directories");
    eq(new Glob("*.js").expand(base).map(String).join(","), "top.js",
        "* does not cross a separator");

    throwsWith(() => new Glob(), "requires a string", "the pattern is required");
    throwsWith(() => new Glob(42), "requires a string", "a non-string pattern is refused");
    throwsWith(() => gl.matches("one.txt"), "must be a Path", "matches wants a Path");
    throwsWith(() => gl.filter("nope"), "requires an array", "filter wants an array");
}

/* ---- the crossover, which is what a capability owes ---------------------- */
{
    /* Glob's configuration parse is trivial (a strdup and one wildcard scan),
     * so unlike Range or Compressor it has almost nothing to amortise. The
     * honest reading is that it exists for the API -- matches/filter/expand
     * over one pattern -- not for speed. Measured here so the claim is not
     * made blind. */
    const paths = [];
    for (let i = 0; i < 200; i++) paths.push(new Path("file" + i + (i % 3 ? ".txt" : ".js")));
    const gl = new Glob("*.txt");

    const t0 = performance.now();
    for (let r = 0; r < 50; r++) gl.filter(paths);
    const hoisted = performance.now() - t0;

    const t1 = performance.now();
    for (let r = 0; r < 50; r++) new Glob("*.txt").filter(paths);
    const perCall = performance.now() - t1;

    print("  Glob hoisted " + hoisted.toFixed(2) + " ms vs per-call " +
          perCall.toFixed(2) + " ms over 200 paths x 50 -> " +
          (perCall / hoisted).toFixed(2) + "x");
    /* PUBLISHED, not asserted. A duration fails for reasons that are not bugs:
     * under a sanitizer the two are within noise of each other and this gated
     * the whole run red. The number is the deliverable. */
}

ok = true;
} finally {
    removeAll(root);
}
assert(ok, "the suite ran to completion");
assert(!exists(root), "the temp tree is cleaned up");

print("test_file_handle: all " + n + " assertions passed");
