// flags: --std
// Split from tests/test_p2_batch1.js: the file sections.
import { File, Path, makeTempFile, makeTempDir, removeAll } from "dyna:file";
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); }
    }
}
{
    const dir = makeTempDir("p2-");
    const src = new Path(dir.toString() + "/src.txt");
    const dst = new Path(dir.toString() + "/dst.txt");
    new File(src).writeText("hello");
    new File(dst).writeText("world");
    throws(() => new File(src).copyTo(dst), /EEXIST|exists|overwrite/i, "copyTo refuses existing dest");
    eq(new File(dst).readText(), "world", "copyTo did not clobber dest");
    removeAll(dir);
}
{
    const p = makeTempFile("p2-");
    const txt = "hi";
    new File(p).writeText(txt);
    eq(new File(p).readText(), txt, "makeTempFile realpath round-trip");
    const s = p.toString();
    assert(s.startsWith("/") && s.length > 5, "makeTempFile returns absolute path " + s);
    os.remove(s);
}
{
    const dir = makeTempDir("p2copy-");
    const src = new Path(dir.toString() + "/src.bin");
    const dst = new Path(dir.toString() + "/dst.bin");
    const data = new Uint8Array(256 * 1024);
    data.fill(0xAB);
    new File(src).writeBytes(data);
    const f = new File(src);
    const src2 = new Path(dir.toString() + "/src2.bin");
    new File(src).copyTo(src2);
    eq(new File(src2).readBytes().length, data.length, "copyTo via copyFile preserves size");
    removeAll(dir);
}
if (fails) { print("test_file_p2_batch1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_file_p2_batch1 failed"); }
print("test_file_p2_batch1: " + n + " assertions, 0 failures");
