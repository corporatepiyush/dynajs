// flags: --std
// Split from tests/test_audit_w3_native_8.js: the file sections.
import { Path, makeTempDir, makeDir, remove, writeFile, removeAll, copyFile, exists, Watcher } from "dyna:file";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- P1-28: copyFile ----------------------------------------------------
// ORACLE: copyFile copies a regular file. A device has no length: /dev/zero
// would be copied until the disk is full, and a FIFO blocks the thread.
// CONTROL: a regular file is copied byte for byte.
{
    writeFile(new Path(tp("src.txt")), "hello copy");
    copyFile(new Path(tp("src.txt")), new Path(tp("dst.txt")));
    ok(exists(new Path(tp("dst.txt"))), "control: copyFile copies a regular file");
    const k = kind(() => copyFile(new Path("/dev/null"), new Path(tp("devnull.out"))));
    ok(k === "TypeError", "[red] P1-28: copyFile refuses a device as its source (" + k + ")");
    ok(!exists(new Path(tp("devnull.out"))), "[red] P1-28: and creates no destination for it");
}
// ---- P1-18: Watcher releases what it no longer watches -------------------
// ORACLE: a watcher holds one descriptor per watched entry on kqueue. An
// entry that has been deleted has nothing left to watch, so its descriptor
// must be released; otherwise a directory with file churn climbs to the cap
// and then silently stops arming new files. On inotify the count is
// directories only and is unaffected.
// CONTROL: the watcher reports both the creations and the deletions.
{
    const root = tp("watched");
    makeDir(new Path(root), { recursive: true });
    const w = new Watcher(new Path(root), { debounceMs: 20 });
    let events = 0;
    w.start(() => { events++; });
    const base = w.stats().directories;
    for (let i = 0; i < 40; i++) writeFile(new Path(root + "/f" + i), "x");
    await sleep(250);
    const peak = w.stats().directories;
    for (let i = 0; i < 40; i++) remove(new Path(root + "/f" + i));
    await sleep(250);
    const after = w.stats().directories;
    ok(events >= 80, "control: the watcher reported the creations and deletions (" + events + ")");
    ok(after === base, "[red] P1-18: descriptors of deleted entries are released (" + base + " -> " + peak + " -> " + after + ")");
    if (w.stop) w.stop();
    if (w.close) w.close();
}
removeAll(new Path(String(T)));
console.log("test_file_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_file_audit_w3_native_8: " + fail + " failures");
