// covers: dyna:file.File
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_file_handle");
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

__kitS.section("writeText returns the byte count, like writeFile", () => {
const root = makeTempDir("dyna-file-handle-");
let ok = false;
try {

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

    f.append(" world");
    eq(f.readText(), "hello world", "append concatenates");
    writeFile(p, "hello");
    writeFile(p, " world", { append: true });
    eq(f.readText(), "hello world", "append equals writeFile({append:true})");
}

{
    const f = new File(root.join("bin.dat"));
    const payload = new Uint8Array([1, 2, 255, 0, 3]);
    f.writeBytes(payload);
    const got = f.readBytes();
    eq(got.length, 5, "readBytes returns the byte count, not a re-encoded string");
    eq(Array.from(got).join(","), "1,2,255,0,3", "every byte survives, 0xFF included");
    eq(got.constructor.name, "Uint8Array", "and it is a Uint8Array");

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

{
    const p = root.join("s.txt");
    writeFile(p, "x");
    eq(new File(String(p)).readText(), "x", "new File(string) builds the Path for you");
    eq(new File(p).readText(), "x", "new File(Path) too");
    throwsWith(() => new File(42), "must be a Path or a string", "a number is refused");
    throwsWith(() => new File(), "requires a Path", "no argument is refused");
    eq(readFile(String(p)), "x", "T5: the free functions take a string path too");
    throwsWith(() => readFile(42), "must be a Path", "a number is still refused by the free functions");
}

{
    const src = new File(root.join("src.txt"));
    src.writeText("payload");
    const dstPath = root.join("dst.txt");
    const dst = src.copyTo(dstPath);

    assert(dst instanceof File || Path.isPath(dst.path), "copyTo returns a File");
    eq(dst.readText(), "payload", "the copy has the content");
    eq(src.readText(), "payload", "and the original survives");

    dst.writeText("changed");
    eq(src.readText(), "payload", "writing the copy does not touch the original");

    const movedTo = root.join("moved.txt");
    const same = src.moveTo(movedTo);
    assert(same.path.equals(movedTo), "moveTo RETARGETS the handle to the new path");
    eq(same.readText(), "payload", "and it reads from there");
    assert(!exists(root.join("src.txt")), "the old path is gone");
}

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

{
    const f = new File(root.join("gone.txt"));
    f.writeText("x");
    assert(f.exists(), "created");
    f.remove();
    assert(!f.exists(), "removed");
    f.writeText("again");
    eq(f.readText(), "again", "a File is a path, not a descriptor");
}

ok = true;
} finally {
    removeAll(root);
}
assert(ok, "the suite ran to completion");
assert(!exists(root), "the temp tree is cleaned up");
});
__kitS.end();

print("test_file_handle: all " + n + " assertions passed");
