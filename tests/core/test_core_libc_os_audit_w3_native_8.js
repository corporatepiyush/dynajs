// flags: --std
// covers: c:src/modules/libc/libc-os.c
// Split from tests/test_audit_w3_native_8.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_libc_os_audit_w3_native_8");
import * as os from "os";
import { Path, makeTempDir, removeAll } from "dyna:file";
import { cpuNow } from "../lib/cpuclock.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- C1-11: JS timers ---------------------------------------------------
// ORACLE: a timer queue is a priority queue; set and clear are O(log n), so
// the cost per operation cannot grow tenfold when n grows tenfold. The old
// sorted list was linear per operation (measured 2.7 us at 2k, 28 us at 20k).
// CONTROL: firing order is by deadline, ties in creation order; a cleared
// timer never fires.

__kitS.section("control: os.kill(self, 0) succeeds", () => {
    ok(os.kill(os.getpid(), 0) === 0, "control: os.kill(self, 0) succeeds");
    ok(kind(() => os.kill(-1, 0)) === "RangeError", "[red] P1-21: os.kill(-1, sig) is refused");
    ok(kind(() => os.kill(0, 0)) === "RangeError", "[red] P1-21: os.kill(0, sig) is refused");
    ok(kind(() => os.kill(os.getpid(), 4096)) === "RangeError", "[red] P1-21: an out-of-range signal number is refused");
    ok(os.exec(["/bin/sh", "-c", "exit 0"]) === 0, "control: os.exec runs a command");
    ok(kind(() => os.exec(["/bin/sh", "-c", "exit 0\0 ignored"])) === "TypeError", "[red] P1-21: os.exec refuses an argument containing NUL");
    ok(kind(() => os.exec(["/bin/sh", "-c", "exit 0"], { cwd: "/tmp\0x" })) === "TypeError", "[red] P1-21: os.exec refuses a cwd containing NUL");
    const rc = os.exec(["/bin/sh", "-c", "kill -PIPE $$; exit 0"]);
    ok(rc === -os.SIGPIPE, "[red] P1-21: an os.exec child has the default SIGPIPE disposition (" + rc + ")");
});
__kitS.end();
removeAll(new Path(String(T)));
console.log("test_core_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_core_audit_w3_native_8: " + fail + " failures");
