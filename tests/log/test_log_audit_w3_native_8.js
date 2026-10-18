// flags: --std
// Split from tests/test_audit_w3_native_8.js: the log sections.
import * as os from "os";
import { Path, makeTempDir, removeAll } from "dyna:file";
import { Logger } from "dyna:log";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- P1-16: log file mode -----------------------------------------------
// ORACLE: application logs carry request data; a file created for them must
// not be readable by every local user. 0640 keeps the group (a log shipper).
// CONTROL: the line is written.
{
    const lp = tp("app.log");
    const L = new Logger({ dest: lp });
    L.info("mode probe");
    if (L.flush) L.flush();
    const [st, err] = os.stat(lp);
    ok(err === 0 && st.size > 0, "control: the logger wrote its line");
    ok(err === 0 && (st.mode & 0o007) === 0, "[red] P1-16: a new log file is not world-readable (mode " + (st.mode & 0o777).toString(8) + ")");
    if (L.close) L.close();
}
removeAll(new Path(String(T)));
console.log("test_log_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_log_audit_w3_native_8: " + fail + " failures");
