// flags: --std
// covers: dyna:file.Watcher
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_watcher_bytes");
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

__kitS.section("Watcher ctor bag", () => {
    throws(() => new Watcher(dir, { recursiv: true }).close(),
           /unknown option "recursiv" \(valid: recursive, debounceMs, ignore\)/, "Watcher ctor bag");
    throws(() => new Watcher(dir, { recursive: true, debounce: 5 }).close(),
           /unknown option "debounce"/, "Watcher ctor bag (2nd key)");
    const target = new Path(dir, "locked.txt");
    writeFile(target, "x");
    const l = new FileLock(target, { retry: 0, retryMs: 1 });
    assert(l.withLock(() => "crit") === "crit", "FileLock with known keys still works");
    assert(l.closed, "withLock consumes the lock");
});
__kitS.end();

removeAll(dir);
print(`test_file_bytes: ${n} assertions passed`);
if (fails) throw new Error(fails + " failures");
