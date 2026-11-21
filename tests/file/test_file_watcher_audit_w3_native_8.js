// covers: dyna:file.Watcher
// Split from tests/test_audit_w3_native_8.js: the file sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_watcher_audit_w3_native_8");
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

await __kitS.section("control: the watcher reported the creations and deletions", async () => {
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
});
__kitS.end();
removeAll(new Path(String(T)));
console.log("test_file_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_file_audit_w3_native_8: " + fail + " failures");
