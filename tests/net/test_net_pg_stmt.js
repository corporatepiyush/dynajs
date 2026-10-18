import { PostgreSQL } from "dyna:net";

const CFG = { host: "127.0.0.1", port: 55432, user: "postgres",
              password: "pw", database: "djtest" };

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }

(async () => {
  let db = null, plain = null;
  try {
    db = new PostgreSQL(CFG);
    try {
      await db.query("SELECT 1");
    } catch (e) {
      print("test_net_pg_stmt: SKIP -- no server on 127.0.0.1:55432 (" +
            String(e).slice(0, 50) + ")");
      try { db.close(); } catch (e2) {}
      return;
    }

    {
      const s0 = db.statementCache;
      check(s0.max > 0, "statementCacheSize default must be published");
      check(s0.prepareAfter >= 1, "prepareAfter must be published");

      const sql = "SELECT $1::int AS a, $2::text AS b";
      const before = db.statementCache;
      const r1 = await db.query(sql, ["7", "x"]);
      const afterFirst = db.statementCache;
      eq(afterFirst.preparedHits - before.preparedHits, 0,
         "the first sighting of a SQL must take the UNNAMED arm");

      const r2 = await db.query(sql, ["7", "x"]);
      const r3 = await db.query(sql, ["7", "x"]);
      const afterMore = db.statementCache;
      check(afterMore.preparedHits > afterFirst.preparedHits,
            "a repeated SQL must be promoted to a NAMED statement (hits " +
            afterFirst.preparedHits + " -> " + afterMore.preparedHits + ")");

      eq(JSON.stringify(r1.rows), JSON.stringify(r2.rows),
         "the cached arm must return the same rows as the unnamed one");
      eq(JSON.stringify(r2.rows), JSON.stringify(r3.rows),
         "and stay stable across repeats");
      eq(r3.rows[0].a, 7, "value through the cached arm");
      eq(r3.rows[0].b, "x", "text value through the cached arm");
    }

    {
      const sql = "SELECT $1::int * 3 AS v";
      for (let i = 0; i < 5; i++) await db.query(sql, [String(i)]);
      const got = [];
      for (let i = 0; i < 5; i++)
        got.push((await db.query(sql, [String(i)])).rows[0].v);
      eq(got.join(","), "0,3,6,9,12",
         "a cached statement must honour NEW parameters, not replay the first");
    }

    {
      plain = new PostgreSQL(Object.assign({}, CFG, { statementCacheSize: 0 }));
      const sql = "SELECT $1::int AS a";
      for (let i = 0; i < 5; i++) await plain.query(sql, ["1"]);
      const s = plain.statementCache;
      eq(s.preparedHits, 0, "statementCacheSize:0 must never prepare");
      eq(s.size, 0, "and must cache nothing");
      check(s.unnamed >= 5, "every query takes the unnamed arm, got " + s.unnamed);
      eq((await plain.query(sql, ["42"])).rows[0].a, 42,
         "and still returns correct rows");
    }

    {
      const small = new PostgreSQL(Object.assign({}, CFG,
                                                 { statementCacheSize: 4 }));
      await small.query("SELECT 1");
      for (let i = 0; i < 20; i++)
        await small.query("SELECT " + i + "::int AS a, $1::int AS b", ["1"]);
      const s = small.statementCache;
      check(s.size <= 4, "the cache must not exceed statementCacheSize, got " +
            s.size);
      eq((await small.query("SELECT 5::int AS a, $1::int AS b", ["9"])).rows[0].b, 9,
         "a full cache must still answer correctly (it stays unnamed)");
      small.close();
    }

    {
      const t = "djs_stmt_" + Date.now();
      await db.query("CREATE TABLE " + t + " (a int)");
      await db.query("INSERT INTO " + t + " VALUES (1)");
      const sel = "SELECT * FROM " + t + " WHERE a = $1::int";
      for (let i = 0; i < 4; i++) await db.query(sel, ["1"]);
      check(db.statementCache.preparedHits > 0, "the select is prepared");

      await db.query("ALTER TABLE " + t + " ADD COLUMN b text DEFAULT 'z'");

      let firstErr = null, rows = null;
      try { rows = (await db.query(sel, ["1"])).rows; }
      catch (e) { firstErr = String(e); }
      if (rows === null) rows = (await db.query(sel, ["1"])).rows;
      check(rows !== null && rows.length === 1,
            "after a schema change the query must recover, not fail for ever" +
            (firstErr ? " (first attempt: " + firstErr.slice(0, 60) + ")" : ""));
      check(rows && rows[0].b === "z",
            "and must see the NEW column, not a stale cached description");
      await db.query("DROP TABLE " + t);
    }

    {
      const eager = new PostgreSQL(Object.assign({}, CFG, { prepareAfter: 1 }));
      await eager.query("SELECT 1");
      const sql = "SELECT $1::int AS eager";
      await eager.query(sql, ["1"]);
      await eager.query(sql, ["1"]);
      check(eager.statementCache.preparedHits >= 1,
            "prepareAfter:1 must promote on the second call at the latest");
      eager.close();
    }
  } catch (e) {
    fails++;
    print("FAIL: unexpected throw: " + e + (e && e.stack ? "\n" + e.stack : ""));
  } finally {
    try { if (db) db.close(); } catch (e) {}
    try { if (plain) plain.close(); } catch (e) {}
  }
  if (fails === 0) print("test_net_pg_stmt: all " + n + " checks passed");
  else print("test_net_pg_stmt: " + fails + " FAILED of " + n);
})();
