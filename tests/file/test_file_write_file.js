// flags: --std
// covers: dyna:file.writeFile
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_write_file");
import * as std from "std";
import { Path, FileReader, FileWriter, FileLock, readFile, readBytes, writeFile, copyFile, makeDir, File, exists, remove, removeAll, glob } from "dyna:file";
import * as os from "os";
import { cpuNow } from "../lib/cpuclock.js";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_file.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.txt`);

const body = "line one\nline two\nline three\n" + "x".repeat(200000);
const n = writeFile(path, body);

__kitS.section("writeFile returns byte count", () => {
assert(n === body.length, "writeFile returns byte count");
});

__kitS.section("readFile roundtrips writeFile", () => {
assert(readFile(path) === body, "readFile roundtrips writeFile");
});

writeFile(path, "A");
writeFile(path, "B", { append: true });
{
    const w = new FileWriter(path, { append: true });
    w.write("C"); w.close();
}

__kitS.section("append mode concatenates", () => {
assert(readFile(path) === "ABC", "append mode concatenates");
});

__kitS.section("hello world", () => {
{
    writeFile(path, "hello world");
    const r = new FileReader(path);
    let ok = true;
    try { r.read({ valueOf() { r.close(); return 3; } }); }
    catch (e) { ok = true; }
    assert(r.closed, "read() coerces the count before resolving the handle");
}
});

__kitS.section("writeFile(Uint8Array) returns the byte count", () => {
{
    const bin = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_file.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.bin`);
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
});
__kitS.end();

print("test_file: all tests passed");
