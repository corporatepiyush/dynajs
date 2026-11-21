// flags: --std
// timeout: 300
// covers: dyna:sqlite.SQLite
// Split from tests/test_audit_w3_native_7.js: the net sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sqlite_sq_lite_audit_w3_native_7");
import * as std from "std";
import * as sqliteMod from "dyna:sqlite";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// ---- N3-24 --------------------------------------------------------------
// ORACLE: SQLite's own definition -- sqlite3_changes() is "the number of rows
// modified by the most recently completed INSERT, UPDATE or DELETE"; DDL and
// SELECT modify no rows, so exec() of those must report 0, not the count left
// over from an earlier statement. And C strings end at U+0000: SQL text that
// contains one cannot be what the caller meant to run.
__kitS.section("N3-24 -------------------------------------------------------------- ORACLE: SQLite's o...", () => {

if (typeof sqliteMod.SQLite === "function") {
    const db = new sqliteMod.SQLite(":memory:");
    db.exec("create table t(a)");
    ok(db.exec("insert into t values (1),(2),(3)") === 3, "control: an INSERT of 3 rows reports 3");
    ok(db.exec("create table u(a)") === 0, "[red] N3-24: CREATE TABLE after a 3-row insert reports 0 (" + db.exec("create table u2(a)") + ")");
    ok(db.exec("update t set a = a where a > 100") === 0, "control: an UPDATE that matches nothing reports 0");
    ok(db.exec("delete from t where a = 1; create table v(a)") === 1, "[red] N3-24: multi-statement exec sums only real changes");
    ok(/U\+0000/.test(thrown(() => db.query("select 1 as x\0 garbage"))), "[red] N3-24: SQL with an embedded U+0000 is refused (" + thrown(() => db.query("select 1 as x\0 garbage")).slice(0, 50) + ")");
    ok(db.query("select count(*) as n from t")[0].n === 2, "control: the table holds the 2 remaining rows");
    db.close();
} else {
    throw new Error("test_net_audit_w3_native_7: SQLite is a hard dependency of every native build, and this binary lacks it");
}
print("test_net_audit_w3_native_7: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_net_audit_w3_native_7: " + failures + " failures");
});
__kitS.end();
