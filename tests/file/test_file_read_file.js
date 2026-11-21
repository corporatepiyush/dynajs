// flags: --std
// covers: dyna:file.readFile
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_read_file");
import * as std from "std";
import { Path, FileReader, FileWriter, FileLock, readFile, readBytes, writeFile, copyFile, makeDir, File, exists, remove, removeAll, glob } from "dyna:file";
import * as os from "os";
import { cpuNow } from "../lib/cpuclock.js";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_file.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.txt`);

const body = "line one\nline two\nline three\n" + "x".repeat(200000);
const n = writeFile(path, body);

__kitS.section("readFile {bytes:true} returns a Uint8Array", () => {
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
});

__kitS.section("a TypeError", () => {
    const t = new Path(`${String(path)}.strict.txt`);
    writeFile(t, "x");
    const dir = new Path(Path.temp(), "dj_strict." + (Date.now() % 10000000) + "_" + Math.random().toString(36).slice(2, 10));
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
    throwsStrict(() => writeFile(t, "x", { appends: true }), "appends", "append, atomic", "writeFile bag");
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
});
__kitS.end();

print("test_file: all tests passed");
