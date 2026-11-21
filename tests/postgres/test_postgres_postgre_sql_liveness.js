// covers: dyna:postgres.PostgreSQL
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_liveness");
import { PostgreSQL } from "dyna:postgres";

let n = 0, fails = 0;
const ok = (c, m) => { n++; if (c) print("  ok  " + m); else { fails++; print("FAIL: " + m); } };

const t0 = Date.now();
const pg = new PostgreSQL({ host: "10.255.255.1", port: 5432, user: "x",
                            database: "x", connectTimeoutMs: 500 });

await __kitS.section("SELECT 1", async () => {
try { await pg.query("SELECT 1"); ok(false, "pg: blackhole connect must fail"); }
catch (e) { ok(/timed out/.test(e.message), "pg: deadline rejects (got: " + e.message.slice(0, 40) + ")"); }
});

const pgDt = Date.now() - t0;

__kitS.section("pg: the deadline fired, not the OS connect timeout", () => {
// wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
ok(pgDt < 15000, "pg: the deadline fired, not the OS connect timeout (" + pgDt + "ms)");
});

__kitS.end();

print("test_postgres_postgre_sql_liveness: " + n + " checks, " + fails + " failures (unclosed clients left live on purpose)");
if (fails) throw new Error("test_postgres_postgre_sql_liveness: " + fails + " failures");
