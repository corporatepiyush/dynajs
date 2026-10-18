// flags: --std
// timeout: 300
// Split from tests/test_audit_w3_native_3.js: the net sections.
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import * as net from "dyna:net";

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
// ---- N3-23 --------------------------------------------------------------
if (typeof net.SQLite === "function") {
    // ORACLE: the documented exit shapes. A script stopped by --timeout-ms dies
    // of an "interrupted" InternalError; exit status 113 is the hard-stop
    // watchdog, which exists for native loops that never poll. A 60M-step
    // recursive CTE inside sqlite3_step must take the first route.
    // CONTROL: a 1000-step CTE under the same flag returns its count.
    const r = child("sq_time.mjs", `
import { SQLite } from "dyna:net";
const db = new SQLite(":memory:");
print("small=" + db.query("with recursive c(x) as (select 1 union all select x+1 from c where x < 1000) select count(*) as n from c")[0].n);
db.query("with recursive c(x) as (select 1 union all select x+1 from c where x < 60000000) select count(*) as n from c");
print("finished");
`, ["--timeout-ms", "400"]);
    ok(/small=1000/.test(r.stdout), "control: a short query runs under --timeout-ms (" + (r.stdout + r.stderr).slice(0, 100) + ")");
    ok(!/finished/.test(r.stdout) && r.code !== 0, "the long query did not run to completion");
    ok(/interrupted/.test(r.stderr) && r.code !== 113, "[red] N3-23: SQLite is interrupted by the deadline, not by the hard stop (code " + r.code + ", " + r.stderr.slice(0, 80) + ")");

    // ORACLE: arithmetic. 8 x 50 MB of blobs cannot fit a 16 MB native budget.
    // CONTROL: 8 x 50 KB under the same budget is stored and counted.
    const m = child("sq_mem.mjs", `
import { SQLite } from "dyna:net";
const db = new SQLite(":memory:");
db.exec("create table t(b)");
for (let i = 0; i < 8; i++) db.exec("insert into t values (zeroblob(50000))");
print("small=" + db.query("select sum(length(b)) as n from t")[0].n);
let held = 0, err = "none";
try { for (let i = 0; i < 8; i++) { db.exec("insert into t values (zeroblob(50000000))"); held++; } } catch (e) { err = e.message; }
print("heldBig=" + held, "err=" + err);
`, ["--native-memory-limit", "16000000"]);
    ok(/small=400000/.test(m.stdout), "control: small rows fit the budget (" + (m.stdout + m.stderr).slice(0, 100) + ")");
    ok(/heldBig=0 err=.*(memory|full)/i.test(m.stdout), "[red] N3-23: 50 MB blobs are refused under a 16 MB native limit (" + m.stdout.slice(-80).trim() + ")");
} else {
    print("  SKIP: this build has no SQLite; N3-23 not exercised");
}
try { removeAll(TMP); } catch (e) {}
print("test_net_audit_w3_native_3: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_net_audit_w3_native_3: " + failures + " failures");
