// flags: --std
// covers: dyna:postgres.PostgreSQL
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_e2e");
import * as std from "std";
import { ensurePostgres, stopStarted, PG_PORT } from "../lib/services.js";
import { PostgreSQL } from "dyna:postgres";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const REDIS_PORT = 6399;
const svc = __kitS.listing ? null : await ensurePostgres({ port: PG_PORT, user: "postgres", password: "pw", database: "djtest" });
if (svc && !svc.ok) print("test_postgres_postgre_sql_e2e: " + svc.why);

await __kitS.section("SELECT 1", async () => {
  let db = null;
  try {
    db = new PostgreSQL({ host: "127.0.0.1", port: PG_PORT, user: "postgres",
                          password: "pw", database: "djtest" });
    await db.query("SELECT 1");
  } catch (e) {
    print("test_postgres_postgre_sql_e2e: postgres SKIPPED (no server on 127.0.0.1:" +
          PG_PORT + ")");
    if (db) db.close();
    db = null;
  }
  if (db) {
    print("  postgres pid=" + db.backendPid);
    check(db.ready, "SCRAM handshake reached ready");
    const t = await db.query(
      "CREATE TABLE IF NOT EXISTS e2e_t (id serial primary key, v text)");
    check(t.command.startsWith("CREATE TABLE"),
          "DDL tag, got " + JSON.stringify(t.command));
    const ins = await db.query("INSERT INTO e2e_t (v) VALUES ($1), ($2)",
                               ["a'b", "plain"]);
    check(ins.rowCount === 2, "two rows inserted, got " + ins.rowCount);
    const sel = await db.query("SELECT id, v FROM e2e_t ORDER BY id");
    check(sel.rows.length === 2 && sel.rows[0].v === "a'b",
          "the quote must survive as data: " + JSON.stringify(sel.rows));
    const pr = await db.pipeline([
      ["SELECT $1::int + 1 AS n", [41]],
      ["INSERT INTO e2e_t (v) VALUES ($1)", ["piped"]],
      ["SELECT count(*) AS c FROM e2e_t"],
    ]);
    check(pr.length === 3 && pr[0].rows[0].n === 42,
          "pipeline member 0, got " + JSON.stringify(pr[0]));
    check(pr[1].rowCount === 1, "pipeline INSERT applied once");
    check(pr[2].rows[0].c === 3, "and visible to member 2, got " +
          JSON.stringify(pr[2].rows));
    const pf = await db.pipeline([
      ["SELECT 1 AS ok"],
      ["SELECT * FROM no_such_table_e2e"],
      ["SELECT 2 AS also_ok"],
    ]);
    check(pf[0].rows[0].ok === 1, "member before the error resolved");
    check(pf[1] instanceof Error, "failing member is an Error slot");
    check(pf[2] instanceof Error && /skipped/.test(String(pf[2].message)),
          "later member named as skipped by the SERVER's skip-to-Sync");
    const after = await db.query("SELECT 3 AS fine");
    check(after.rows.length === 1 && after.rows[0].fine === 3 &&
          after.command === "SELECT 1",
          "the connection still works after an aborted batch, got " +
          JSON.stringify(after.rows) + " / " + after.command);
    for (let i = 0; i < 5; i++)
      await db.query("SELECT $1::int + g AS s FROM generate_series(1,3) g", [i]);
    const stats = db.statementCache;
    check(stats.preparedHits > 0,
          "the statement cache must be hitting, got " + JSON.stringify(stats));
    check(db.statementCache.size > 0 && db.statementCache.size <= stats.max,
          "the cache holds what it ran, got " +
          JSON.stringify(db.statementCache));
    await db.query("DELETE FROM e2e_t");
    db.close();
  }
});
__kitS.end();
await stopStarted();

if (fails === 0) print("test_postgres_postgre_sql_e2e: all " + n + " checks passed");
else { print("test_postgres_postgre_sql_e2e: " + fails + " FAILED of " + n); throw new Error("test_postgres_postgre_sql_e2e: " + fails + " failures"); }
