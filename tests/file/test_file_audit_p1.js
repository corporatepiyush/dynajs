// flags: --std
// Split from tests/test_audit_p1.js: the file sections.
import * as os from "os";
import { Path } from "dyna:file";
import { Watcher } from "dyna:file";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}
{
    const dir = "/tmp/test_audit_watch_" + Date.now();
    os.mkdir(dir, 0o755);
    const w = new Watcher(new Path(dir));
    w.start(() => {});
    assert(typeof w.close === "function", "watcher close() exists");
    w.close();
    w.close();
    os.remove(dir);
}
if (fails) { print("test_file_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_file_audit_p1 failed"); }
print("test_file_audit_p1: " + n + " assertions, 0 failures");
