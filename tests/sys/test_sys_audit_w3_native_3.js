// flags: --std
// timeout: 300
// Split from tests/test_audit_w3_native_3.js: the sys sections.
import * as os from "os";
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const TMP = makeTempDir("w3n3-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function child(name, src, flags = []) {
    writeFile(P(name), src);
    return Exec(BIN, [...flags, P(name).toString()], { timeoutMs: name === "budget2.mjs" ? 8000 : 60000, encoding: "utf8" });
}
// ---- P1-09 --------------------------------------------------------------
// ORACLE: the process table. After os.exec returns from a timeout nothing it
// started may remain: not the child as a zombie, not the child's own child.
// The grandchild sleeps for a duration unique to this run so pgrep cannot
// match anything else (the [s] keeps the pattern from matching the shell that
// runs pgrep, which Linux pgrep does not exclude). CONTROL: os.exec without a timeout returns the status.
{
    const tag = "97." + (os.getpid() % 100000);
    ok(os.exec(["/bin/sh", "-c", "exit 7"]) === 7, "control: os.exec returns the exit status");
    const rc = os.exec(["/bin/sh", "-c", "trap '' TERM; sleep " + tag + " & sleep " + tag], { timeout: 200 });
    ok(rc === -15, "timed-out os.exec reports -SIGTERM (" + rc + ")");
    os.sleep(200);
    const left = Exec("/bin/sh", ["-c", "pgrep -f '[s]leep " + tag + "' | wc -l"], { encoding: "utf8" });
    ok(+left.stdout.trim() === 0, "[red] P1-09: no process of the timed-out command survives (" + left.stdout.trim() + " left)");
    const z = Exec("/bin/sh", ["-c", "ps -o ppid=,stat= -ax | awk -v me=" + os.getpid() + " '$1==me && $2 ~ /^Z/' | wc -l"], { encoding: "utf8" });
    ok(+z.stdout.trim() === 0, "[red] P1-09: no zombie child is left behind (" + z.stdout.trim() + ")");
    Exec("/bin/sh", ["-c", "pkill -f 'sleep " + tag + "'; true"]);
}
try { removeAll(TMP); } catch (e) {}
print("test_sys_audit_w3_native_3: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_sys_audit_w3_native_3: " + failures + " failures");
