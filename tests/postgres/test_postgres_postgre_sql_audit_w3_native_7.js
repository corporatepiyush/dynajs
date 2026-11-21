// flags: --std
// timeout: 300
// covers: dyna:postgres.PostgreSQL
// Split from tests/test_audit_w3_native_7.js: the net sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_audit_w3_native_7");
import * as std from "std";
import * as postgresMod from "dyna:postgres";
import { ensurePostgres, stopStarted } from "../lib/services.js";

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

await __kitS.section("select 1", async () => {
    const svc = await ensurePostgres();
    const pg = new postgresMod.PostgreSQL({ ...(svc.ok ? svc.trust : { host: "127.0.0.1", port: svc.port, user: "djtrust", database: "postgres" }), connectTimeoutMs: 2500 });
    let up = false;
    try { await pg.query("select 1"); up = true; } catch (e) {}
    if (svc.ok && !up) ok(false, "the services.js PostgreSQL on " + svc.port + " refused the djtrust login");
    if (!up) {
        print("  SKIP: no PostgreSQL on 127.0.0.1:" + svc.port + " (" + (svc.why || "unreachable") + ")");
    } else {
        let e = "none";
        try { await pg.query("select 1 as a\0, 2 as b"); } catch (x) { e = x.constructor.name + ": " + x.message; }
        ok(/U\+0000/.test(e), "[red] N3-20: SQL with an embedded U+0000 is refused before it is sent (" + e.slice(0, 60) + ")");
        const r = await pg.query("select 9223372036854775807::int8 as max, (-9223372036854775807 - 1)::int8 as min, 42::int8 as small");
        const row = (r.rows ?? r)[0];
        ok(Number(row.small) === 42 && String(row.max) === "9223372036854775807" && String(row.min) === "-9223372036854775808", "control: int8 extremes survive on a live PostgreSQL (" + row.max + ", " + row.min + ")");
    }
    try { pg.close(); } catch (e) {}
    await stopStarted();
});
__kitS.end();
print("test_net_audit_w3_native_7: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_net_audit_w3_native_7: " + failures + " failures");
