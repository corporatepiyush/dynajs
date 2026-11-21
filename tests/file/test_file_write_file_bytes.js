// flags: --std
// covers: dyna:file.writeFile
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_write_file_bytes");
import * as std from "std";
import {
    Path, File, FileReader, readFile, readBytes, writeFile, makeDir,
    removeAll, remove, glob, Glob, FileLock, Watcher, symlink,
} from "dyna:file";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; throw new Error("assertion failed: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL(no throw): " + m); }
    catch (e) {
        if (!re.test(String(e.message))) { fails++; print(`FAIL(${e.message}): ` + m); }
    }
}

const dir = new Path(Path.temp(), "dj_fbytes." + (Date.now() % 10000000) + "_" + Math.random().toString(36).slice(2, 10));
makeDir(dir, { recursive: true });
const bin = new Path(dir, "bin.dat");

function fresh(data, bufsize) {
    writeFile(bin, data);
    return new FileReader(bin, { bufferSize: bufsize || 4 });
}
function eq(a, b, m) {
    assert(a.length === b.length && a.every((v, i) => v === b[i]),
           m + ` (${JSON.stringify(Array.from(a))} vs ${JSON.stringify(Array.from(b))})`);
}

__kitS.section("readInto fills a buffer exactly 3x the read buffer", () => {
    const raw = new Uint8Array(12);
    for (let i = 0; i < 12; i++) raw[i] = (i + 1) * 10;
    const r = fresh(raw);
    const out = new Uint8Array(12);
    assert(r.readInto(out) === 12, "readInto fills a buffer exactly 3x the read buffer");
    eq(out, raw, "readInto content exact across fills");
    assert(r.readInto(out) === 0, "readInto returns 0 at EOF");
    assert(r.readInto(out) === 0, "readInto keeps returning 0 at EOF");
    r.close();

    const r2 = fresh(raw);
    const head = new Uint8Array(3);
    assert(r2.readInto(head) === 3, "readInto into a smaller buffer returns its size");
    eq(head, raw.subarray(0, 3), "small readInto content");
    const tail = new Uint8Array(9);
    assert(r2.readInto(tail) === 9, "the unconsumed remainder survives the small read");
    eq(tail, raw.subarray(3), "remainder content");
    assert(r2.readInto(tail) === 0, "EOF after exact consumption");
    r2.close();

    const five = new Uint8Array([1, 2, 3, 4, 5]);
    const r3 = fresh(five);
    const six = new Uint8Array(6);
    assert(r3.readInto(six) === 5, "readInto returns the byte count, not the buffer size, at a short EOF");
    eq(six.subarray(0, 5), five, "short-EOF content");
    assert(six[5] === 0, "no garbage past the returned count");
    r3.close();

    const r4 = fresh(raw);
    let acc = [], got = 0, one;
    while ((got = r4.readInto(one = new Uint8Array(2))) > 0) {
        assert(got === 1 || got === 2, "small reads return 1 or 2");
        for (let i = 0; i < got; i++) acc.push(one[i]);
    }
    assert(acc.length === 12 && acc.every((v, i) => v === raw[i]), "2-byte reads reassemble the file");
    r4.close();

    const r5 = fresh(new Uint8Array(0));
    assert(r5.readInto(new Uint8Array(8)) === 0, "readInto on a 0-byte file returns 0 (EOF)");
    r5.close();
    const r6 = fresh(raw);
    assert(r6.readInto(new Uint8Array(0)) === 0, "a 0-length buffer reports 0 without reading");
    const probe = new Uint8Array(4);
    assert(r6.readInto(probe) === 4 && probe[0] === raw[0],
           "a 0-length readInto did not consume file data");
    r6.close();

    const r7 = fresh(raw);
    const ab = new ArrayBuffer(32);
    const sub = new Uint8Array(ab, 8, 12);
    assert(r7.readInto(sub) === 12, "readInto into an offset subview");
    const whole = new Uint8Array(ab);
    assert(whole[7] === 0 && whole[8] === 10 && whole[19] === 120 && whole[20] === 0,
           "the view's byte offset was honored on both sides");
    r7.close();

    const r8 = fresh(raw);
    const ab2 = new ArrayBuffer(16);
    const i32 = new Int32Array(ab2, 0, 3);
    assert(r8.readInto(i32) === 12, "readInto into an Int32Array writes raw bytes");
    const dv = new DataView(ab2, 12, 4);
    const r8b = fresh(raw.subarray(0, 2));
    assert(r8b.readInto(dv) === 2, "readInto into a DataView subview");
    const check = new Uint8Array(ab2);
    eq(check.subarray(0, 12), raw, "Int32Array backing bytes are the file's");
    assert(check[12] === raw[0] && check[13] === raw[1], "DataView backing bytes continue at its offset");
    r8.close(); r8b.close();

    const text = "héllo";
    writeFile(bin, text);
    const rt = fresh(text, 3);
    const a = new Uint8Array(2);
    const b = new Uint8Array(4);
    assert(rt.readInto(a) === 2 && rt.readInto(b) === 4, "split read lengths");
    assert(a[1] === 0xC3 && b[0] === 0xA9, "the 2-byte é arrived SPLIT across reads: readInto is a bytes primitive");
    rt.close();

    const r9 = fresh(raw);
    throws(() => r9.readInto(), /requires a Uint8Array/i, "readInto requires a buffer");
    throws(() => r9.readInto("nope"), /TypedArray|ArrayBuffer/i, "readInto refuses a non-view");
    throws(() => r9.readInto(42), /TypedArray|ArrayBuffer/i, "readInto refuses a number");
    r9.close();
    throws(() => r9.readInto(new Uint8Array(4)), /./, "readInto on a closed reader refuses (no UAF)");
    throws(() => r9.readBytes(), /./, "readBytes on a closed reader refuses (no UAF)");
});
__kitS.end();

removeAll(dir);
print(`test_file_bytes: ${n} assertions passed`);
if (fails) throw new Error(fails + " failures");
