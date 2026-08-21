/* test_net_e2e.js -- the three database clients against REAL servers.
 *
 * The mock suites own the adversarial replies; this file owns what only a real
 * server can prove: SCRAM against a genuine postgres, RESP3 against a genuine
 * redis, a file-backed SQLite round trip, and pipeline() end to end.
 *
 * Skips LOUDLY per client when its server is not up -- a silent skip reads as
 * a pass and this file is in NATIVE_TESTS where nobody reads the count.
 *   redis-server on 127.0.0.1:6399   (brew install redis;
 *                                      redis-server --port 6399 --daemonize yes)
 *   postgres    on 127.0.0.1:55432   (user postgres, password pw, database djtest)
 */
import { Redis, PostgreSQL, SQLite } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const REDIS_PORT = 6399, PG_PORT = 55432;

/* ---- redis ----------------------------------------------------------- */
{
  let r = null;
  try {
    r = new Redis({ host: "127.0.0.1", port: REDIS_PORT });
    await r.command("PING");
  } catch (e) {
    print("test_net_e2e: redis SKIPPED (no server on 127.0.0.1:" +
          REDIS_PORT + ")");
    if (r) r.close();
    r = null;
  }
  if (r) {
    print("  redis " + r.protocol);
    check(await r.command("SET", "e2e:key", "v1") === "OK", "SET");
    check(await r.command("GET", "e2e:key") === "v1", "GET round trip");
    check(await r.command("INCR", "e2e:counter") >= 1, "INCR");
    /* injection: a value that is itself a command stays one argument */
    const evil = "x\r\n*1\r\n$8\r\nFLUSHALL\r\n";
    await r.command("SET", "e2e:evil", evil);
    check(await r.command("GET", "e2e:evil") === evil,
          "a CRLF+command value must round trip as DATA");
    const p = await r.pipeline([["SET", "e2e:p1", "a"], ["GET", "e2e:p1"],
                                ["PING"]]);
    check(Array.isArray(p) && p.length === 3 && p[1] === "a" && p[2] === "PONG",
          "pipeline order/values: " + JSON.stringify(p));
    /* expiry is server-side behaviour no mock can prove */
    await r.command("SET", "e2e:ttl", "x", "PX", 150);
    await new Promise((res) => setTimeout(res, 900));
    check(await r.command("GET", "e2e:ttl") === null, "PX expiry honoured");
    r.close();
  }
}

/* ---- postgres -------------------------------------------------------- */
{
  let db = null;
  try {
    db = new PostgreSQL({ host: "127.0.0.1", port: PG_PORT, user: "postgres",
                          password: "pw", database: "djtest" });
    await db.query("SELECT 1");
  } catch (e) {
    print("test_net_e2e: postgres SKIPPED (no server on 127.0.0.1:" +
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
    /* pipeline against the REAL server: one round trip, ordered results */
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
    /* a failing member does not poison the connection */
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
    /* repeat the same statement enough to cross prepareAfter: the hits are
       what the statement cache exists for */
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
}

/* ---- sqlite: FILE-backed, so persistence itself is under test -------- */
{
  const path = "/tmp/dynajs_e2e_sqlite.db";
  let w = new SQLite(path);
  w.exec("DROP TABLE IF EXISTS e2e");
  w.exec("CREATE TABLE e2e (k TEXT PRIMARY KEY, v BLOB)");
  const blob = new Uint8Array([0, 1, 255, 0, 7]);
  w.exec("INSERT INTO e2e VALUES (?, ?)", ["k", blob]);
  w.close();

  const r = new SQLite(path);
  const row = r.query("SELECT v FROM e2e WHERE k = ?", ["k"]);
  check(row.length === 1 && row[0].v instanceof Uint8Array &&
        blob.every((b, i) => b === row[0].v[i]),
        "BLOB persisted across open/close exactly");
  r.close();
}

if (fails === 0) print("test_net_e2e: all " + n + " checks passed");
else print("test_net_e2e: " + fails + " FAILED of " + n);
