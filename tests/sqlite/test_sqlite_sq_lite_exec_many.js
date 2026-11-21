// covers: dyna:sqlite.SQLite.execMany
// execMany(sql, rows) runs one prepared statement once per row inside a SAVEPOINT, so a batch
// is all rows or none: a failing row (constraint, bad value, wrong parameter count) rolls the
// whole batch back and throws naming the row, and an enclosing BEGIN stays open with its earlier
// work. Rows are read as dense arrays only: no getter or proxy can run mid-batch and touch the
// statement cache. A/B (mac, DEV_HOST_EXCLUSIVE=1, 12 interleaved reps, 200k 3-int rows,
// :memory:, speedup = base ns / new ns, median [IQR], control 0.998..1.007): exec loop in
// BEGIN/COMMIT 1.134 [1.130..1.143] (params on the stack, fast-array reads), with an index
// 1.052; point query by rowid 1.103; execMany vs the old exec loop 1.425 [1.417..1.436], with
// an index 1.155 (B-tree bound).
// Mutants (red): no SAVEPOINT (rows before the failing one stay), ROLLBACK instead of
// ROLLBACK TO (the enclosing transaction is lost), no rollback on failure, holey row accepted
// through the dense path, param count not checked per row.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sqlite_sq_lite_exec_many");
import { SQLite } from "dyna:sqlite";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        print("  FAIL " + msg);
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
        print("  FAIL " + msg + " (threw " + String(e).slice(0, 90) + ")");
        return;
    }
    failed++;
    print("  FAIL " + msg + " (no throw)");
}
const count = (db, t) => db.query("SELECT COUNT(*) AS c FROM " + t)[0].c;

__kitS.section("CREATE TABLE t (k INTEGER PRIMARY KEY, v)", () => {
    const db = new SQLite(":memory:");
    db.exec("CREATE TABLE t (k INTEGER PRIMARY KEY, v)");
    const blob = new Uint8Array([1, 2, 3]);
    const got = db.execMany("INSERT INTO t VALUES (?, ?)", [
        [1, "text"], [2, 2.5], [3, null], [4, blob], [5, true], [6, 9007199254740993n],
    ]);
    ok(got === 6, "returns the rows changed (" + got + ")");
    const r = db.query("SELECT k, v, typeof(v) AS ty FROM t ORDER BY k");
    ok(r.map((x) => x.ty).join() === "text,real,null,blob,integer,integer",
        "values bind with exec's types (" + r.map((x) => x.ty).join() + ")");
    ok(r[0].v === "text" && r[1].v === 2.5 && r[3].v instanceof Uint8Array && r[3].v[2] === 3 && r[4].v === 1,
        "values read back intact");
    ok(db.execMany("INSERT INTO t VALUES (?, ?)", []) === 0, "an empty batch changes nothing");
    ok(db.execMany("UPDATE t SET v = ? WHERE k <= ?", [["a", 2], ["b", 3]]) === 5,
        "changes sum over rows (2 + 3)");
    ok(db.execMany("INSERT INTO t VALUES (?, ?)", [[7, "seven"]]) === 1 && db.exec("INSERT INTO t VALUES (?, ?)", [8, "x"]) === 1,
        "exec reuses the statement execMany cached");

    const before = count(db, "t");
    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?)", [[100, "a"], [101, "b"], [1, "dup"], [102, "c"]]),
        /row 2.*UNIQUE|row 2.*constraint/i, "a constraint failure names its row");
    ok(count(db, "t") === before, "a failing row rolls back the rows before it (" + count(db, "t") + " vs " + before + ")");
    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?)", [[110, "a"], [111]]),
        /row 1: statement takes 2 parameters, 1 given/, "a short row names its row and counts");
    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?)", [[120, "a"], [121, { x: 1 }]]),
        /parameter 2 is an object/, "an object value is refused");
    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?)", [[130, "a"], [131, , ]]),
        /row 1 is not a dense array/, "a holey row is refused before anything runs");
    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?)", [[140, "a"], "x"]),
        /row 1 is not a dense array/, "a non-array row is refused");
    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?)", { length: 1, 0: [150, "a"] }),
        /dense array of parameter arrays/, "rows must be an array");
    ok(count(db, "t") === before, "no failed batch left a row behind");
    ok(db.execMany("INSERT INTO t VALUES (?, ?)", [[160, "ok"]]) === 1, "the cached statement still runs after failures");

    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?); DELETE FROM t", [[170, "a"]]),
        /one statement/, "a second statement is refused");
    throwsMatch(() => db.execMany("  commit", [[]]), /transaction control/, "transaction control is refused");
    throwsMatch(() => db.execMany("SAVEPOINT x", [[]]), /transaction control/, "a savepoint is refused");
    ok(db.execMany("DELETE FROM t WHERE k = ?", [[170]]) === 0, "a statement can change zero rows");

    db.exec("BEGIN");
    db.exec("INSERT INTO t VALUES (200, 'outer')");
    throwsMatch(() => db.execMany("INSERT INTO t VALUES (?, ?)", [[201, "a"], [200, "dup"]]),
        /row 1/, "a failure inside BEGIN still throws");
    ok(db.query("SELECT v FROM t WHERE k = 200").length === 1 && db.query("SELECT v FROM t WHERE k = 201").length === 0,
        "inside BEGIN only the batch rolls back; the earlier insert stays");
    ok(db.execMany("INSERT INTO t VALUES (?, ?)", [[202, "b"]]) === 1, "the enclosing transaction is still usable");
    db.exec("COMMIT");
    ok(db.query("SELECT k FROM t WHERE k IN (200, 202) ORDER BY k").map((x) => x.k).join() === "200,202",
        "COMMIT keeps the outer work and the later batch");
    db.close();
});
__kitS.section("CREATE TABLE w", () => {
    const big = [];
    for (let i = 0; i < 20; i++)
        big.push("c" + i);
    const db = new SQLite(":memory:");
    db.exec("CREATE TABLE w (" + big.join(", ") + ")");
    const row = (b) => big.map((_, i) => b + i);
    const sql = "INSERT INTO w VALUES (" + big.map(() => "?").join(",") + ")";
    ok(db.execMany(sql, [row(0), row(100)]) === 2, "20 parameters per row (past the stack buffer)");
    ok(db.query("SELECT c19 FROM w ORDER BY c0")[1].c19 === 119, "the wide rows read back");
    const holey = [1, 2];
    holey[19] = 3;
    ok(db.exec(sql, holey) === 1 && db.query("SELECT c19, c5 FROM w WHERE c0 = 1")[0].c5 === null,
        "exec still takes a holey parameter array (holes bind NULL)");
    db.close();
});
__kitS.section("CREATE TABLE z (a)", () => {
    const db = new SQLite(":memory:", { readonly: true });
    throwsMatch(() => db.execMany("CREATE TABLE z (a)", [[]]), /row 0|readonly|read-only/i,
        "a readonly handle refuses a write batch");
    db.close();
});
__kitS.end();
print("test_sqlite_sq_lite_exec_many: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_sqlite_sq_lite_exec_many: " + failed + " failures");
