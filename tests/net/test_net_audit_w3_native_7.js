// flags: --std
// timeout: 300
// Split from tests/test_audit_w3_native_7.js: the net sections.
import * as std from "std";
import * as net from "dyna:net";

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
if (typeof net.SQLite === "function") {
    const db = new net.SQLite(":memory:");
    db.exec("create table t(a)");
    ok(db.exec("insert into t values (1),(2),(3)") === 3, "control: an INSERT of 3 rows reports 3");
    ok(db.exec("create table u(a)") === 0, "[red] N3-24: CREATE TABLE after a 3-row insert reports 0 (" + db.exec("create table u2(a)") + ")");
    ok(db.exec("update t set a = a where a > 100") === 0, "control: an UPDATE that matches nothing reports 0");
    ok(db.exec("delete from t where a = 1; create table v(a)") === 1, "[red] N3-24: multi-statement exec sums only real changes");
    ok(/U\+0000/.test(thrown(() => db.query("select 1 as x\0 garbage"))), "[red] N3-24: SQL with an embedded U+0000 is refused (" + thrown(() => db.query("select 1 as x\0 garbage")).slice(0, 50) + ")");
    ok(db.query("select count(*) as n from t")[0].n === 2, "control: the table holds the 2 remaining rows");
    db.close();
} else {
    print("  SKIP: this build has no SQLite");
}
// ---- N3-20 --------------------------------------------------------------
// Live PostgreSQL (5432, trust; PostgreSQL 18 on the development host).
// ORACLE: the wire protocol -- query text is a NUL-terminated string, so SQL
// containing U+0000 would be cut silently; it must be refused locally.
// CONTROL: the int8 extremes round-trip through the live server.
{
    const port = +(std.getenv("LIVESRV_PG_PORT") || 5432);
    const pg = new net.PostgreSQL({ host: "127.0.0.1", port, user: std.getenv("LIVESRV_PG_USER") || std.getenv("USER") || "postgres", database: "postgres", connectTimeoutMs: 2500 });
    let up = false;
    try { await pg.query("select 1"); up = true; } catch (e) {}
    if (!up) {
        print("  SKIP: no PostgreSQL on 127.0.0.1:" + port + " (brew services start postgresql@18)");
    } else {
        let e = "none";
        try { await pg.query("select 1 as a\0, 2 as b"); } catch (x) { e = x.constructor.name + ": " + x.message; }
        ok(/U\+0000/.test(e), "[red] N3-20: SQL with an embedded U+0000 is refused before it is sent (" + e.slice(0, 60) + ")");
        const r = await pg.query("select 9223372036854775807::int8 as max, (-9223372036854775807 - 1)::int8 as min, 42::int8 as small");
        const row = (r.rows ?? r)[0];
        ok(Number(row.small) === 42 && String(row.max) === "9223372036854775807" && String(row.min) === "-9223372036854775808", "control: int8 extremes survive on a live PostgreSQL (" + row.max + ", " + row.min + ")");
    }
    try { pg.close(); } catch (e) {}
}
print("test_net_audit_w3_native_7: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_net_audit_w3_native_7: " + failures + " failures");
