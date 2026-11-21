// covers: dyna:file.copyFile
// Split from tests/test_audit_w3_native_8.js: the file sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_copy_file_audit_w3_native_8");
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

__kitS.section("hello copy", () => {
    writeFile(new Path(tp("src.txt")), "hello copy");
    copyFile(new Path(tp("src.txt")), new Path(tp("dst.txt")));
    ok(exists(new Path(tp("dst.txt"))), "control: copyFile copies a regular file");
    const k = kind(() => copyFile(new Path("/dev/null"), new Path(tp("devnull.out"))));
    ok(k === "TypeError", "[red] P1-28: copyFile refuses a device as its source (" + k + ")");
    ok(!exists(new Path(tp("devnull.out"))), "[red] P1-28: and creates no destination for it");
});
__kitS.end();
removeAll(new Path(String(T)));
console.log("test_file_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_file_audit_w3_native_8: " + fail + " failures");
