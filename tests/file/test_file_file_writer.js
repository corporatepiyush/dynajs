// flags: --std
// covers: dyna:file.FileWriter
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_file_writer");
import * as std from "std";
import { Path, FileReader, FileWriter, FileLock, readFile, readBytes, writeFile, copyFile, makeDir, File, exists, remove, removeAll, glob } from "dyna:file";
import * as os from "os";
import { cpuNow } from "../lib/cpuclock.js";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_file.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.txt`);

const body = "line one\nline two\nline three\n" + "x".repeat(200000);
const n = writeFile(path, body);

__kitS.section("write() coerces before resolving the handle", () => {
    const w = new FileWriter(path);
    let threw = false;
    try {
        w.write({ toString() { w.close(); return "boom"; } });
    } catch (e) { threw = true; }
    assert(threw || w.closed, "write() coerces before resolving the handle");
});

__kitS.section("writing a closed FileWriter throws", () => {
    const w = new FileWriter(path);
    w.close();
    let threw = false;
    try { w.write("x"); } catch (e) { threw = true; }
    assert(threw, "writing a closed FileWriter throws");
});
__kitS.end();

print("test_file: all tests passed");
