// timeout: 60
// covers: dyna:sqlite.SQLite
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sqlite_sq_lite_sqlite_bounds");
import { SQLite } from "dyna:sqlite";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
function throwsMatch(fn, re, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        if (re.test(String(e)))
            return;
        failed++;
        console.log("  FAIL " + msg + " (threw " + String(e).slice(0, 70) + ")");
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

throwsMatch(() => new SQLite("a\u0000b.sqlite"), /NUL/i, "a NUL in the path is refused");
__kitS.section(":memory: still opens", () => {
    const db = new SQLite(":memory:");
    ok(db !== undefined, ":memory: still opens");
    const long = "select 1 /* " + "x".repeat(200000) + " */";
    for (let i = 0; i < 50; i++) {
        const r = db.exec(long);
        if (r === undefined && i === 0)
            ok(false, "the oversized statement executes");
    }
    ok(true, "an uncacheable (>128 KiB) statement repeated 50x does not fail");
    const r2 = db.query("select 42 as v");
    ok(Array.isArray(r2) && r2.length === 1 && r2[0].v === 42,
        "the connection still queries after the oversized repeats");
    db.close();
});
__kitS.end();
console.log("test_net_sqlite_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_net_sqlite_bounds: " + failed + " failures");
