// flags: --std
// covers: dyna:file.FileReader
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_file_reader");
import * as std from "std";
import { Path, FileReader, FileWriter, FileLock, readFile, readBytes, writeFile, copyFile, makeDir, File, exists, remove, removeAll, glob } from "dyna:file";
import * as os from "os";
import { cpuNow } from "../lib/cpuclock.js";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_file.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.txt`);

const body = "line one\nline two\nline three\n" + "x".repeat(200000);
const n = writeFile(path, body);

__kitS.section("row", () => {
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
});
__kitS.end();

print("test_file: all tests passed");
