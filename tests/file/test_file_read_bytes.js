// flags: --std
// covers: dyna:file.readBytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_read_bytes");
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

__kitS.section("readFile {bytes:true} round-trips invalid UTF-8", () => {
    const raw = new Uint8Array([0x00, 0x01, 0xFE, 0xFF, 0x00, 0x0D, 0x0A, 0xC8, 0x64, 0x80, 0xFF]);
    writeFile(bin, raw);
    const viaFlag = readFile(bin, { bytes: true });
    const viaAlias = readBytes(bin);
    const viaHandle = new File(bin).readBytes();
    eq(viaFlag, raw, "readFile {bytes:true} round-trips invalid UTF-8");
    eq(viaAlias, raw, "readBytes(path) round-trips invalid UTF-8");
    eq(viaHandle, raw, "File.readBytes() round-trips invalid UTF-8");
    const text = readFile(bin);
    let fffd = 0;
    for (const ch of text) if (ch === "\uFFFD") fffd++;
    assert(fffd >= 3, "the string path mangles the invalid bytes to U+FFFD, the byte paths none");

    const empty = new Path(dir, "empty.bin");
    writeFile(empty, "");
    assert(readBytes(empty).length === 0, "readBytes of a 0-byte file is an empty Uint8Array");
    assert(readFile(empty, { bytes: true }).length === 0, "readFile {bytes} of a 0-byte file is empty");
    const big = new Uint8Array((1 << 20) + 13);
    for (let i = 0; i < big.length; i++) big[i] = (i * 7 + 3) & 0xff;
    const bigPath = new Path(dir, "big.bin");
    writeFile(bigPath, big);
    eq(readBytes(bigPath), big, "readBytes over 1 MiB is byte-exact");
    remove(bigPath);
});

__kitS.section("readBytes(0) reads nothing", () => {
    const raw = new Uint8Array(100);
    for (let i = 0; i < 100; i++) raw[i] = i;
    const r = fresh(raw);
    const head = r.readBytes(7);
    assert(head instanceof Uint8Array && head.length === 7 && head[6] === 6, "readBytes(7)");
    const mid = r.readBytes(0);
    assert(mid.length === 0, "readBytes(0) reads nothing");
    const rest = r.readBytes();
    assert(rest.length === 93 && rest[92] === 99, "readBytes() takes the rest");
    assert(r.readBytes(5).length === 0, "readBytes at EOF is empty");
    r.close();

    writeFile(bin, "0123456789");
    const r2 = new FileReader(bin, { bufferSize: 4 });
    assert(r2.read(2) === "01", "string read first");
    eq(r2.readBytes(3), new Uint8Array([0x32, 0x33, 0x34]), "byte read continues the same stream");
    assert(r2.readLine() === "56789", "readLine continues after the byte read");
    r2.close();

    const r3 = fresh(new Uint8Array([9, 8, 7]));
    assert(r3.readBytes(1000).length === 3, "readBytes(n > file) clamps to the file");
    r3.close();
});
__kitS.end();

removeAll(dir);
print(`test_file_bytes: ${n} assertions passed`);
if (fails) throw new Error(fails + " failures");
