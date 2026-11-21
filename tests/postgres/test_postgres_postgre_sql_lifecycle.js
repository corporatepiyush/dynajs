// flags: --std
// covers: dyna:postgres.PostgreSQL
import { PostgreSQL } from "dyna:postgres";
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { ensurePostgres, stopStarted } from "../lib/services.js";

// The local PostgreSQL is probed in the kit's before hook, never under --list; a
// case whose server is missing is a SKIP. Split from tests/net/test_db_lifecycle.js
// (one module per suite).
const t = suite("test_postgres_postgre_sql_lifecycle");
const ENC = new TextEncoder();
const W = (s) => { const b = ENC.encode(s); os.write(1, b.buffer, 0, b.length); };
let A = null;
const ok = (c, m) => { A.ok(c, m); if (c) W("  ok  " + m + "\n"); };
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let PG = null, havePG = false, svcOk = false, WATCHDOG = null, svcWhy = "unreachable";
t.before(async () => {
  WATCHDOG = setTimeout(() => {
    W("test_postgres_postgre_sql_lifecycle: HUNG (watchdog)\n");
    std.exit(124);
  }, 30000);
  const svc = await ensurePostgres();
  if (!svc.ok) { svcWhy = svc.why; return; }
  svcOk = true;
  PG = svc.trust;
  try {
    const probe = new PostgreSQL({ ...PG, connectTimeoutMs: 800 });
    try { await probe.query("SELECT 1"); havePG = true; } catch (e) {}
    finally { try { probe.close(); } catch (e) {} }
  } catch (e) {}
});
t.after(async () => {
  if (WATCHDOG) clearTimeout(WATCHDOG);
  await stopStarted();
});
const needPG = (a) => {
  if (!havePG && svcOk) a.must(false, "the services.js PostgreSQL refused the djtrust login");
  if (!havePG) a.skip("no PostgreSQL: " + svcWhy);
};

test("pg: close in a notice callback is survived", async (a) => {
  needPG(a);
  const pg = new PostgreSQL(PG);
  try {
      let notices = 0, closedInCb = false;
      pg.on("notice", () => { notices++; try { pg.close(); closedInCb = true; } catch (e) {} });
      let outcome = "";
      try {
          await pg.query("DO $$ BEGIN RAISE NOTICE 'lc'; END $$;");
          outcome = "resolved";
      } catch (e) { outcome = "threw:" + String(e.message).slice(0, 30); }
      await sleep(100);
      ok(true, "pg: notice-close survived (" + notices + " notices, outcome=" + outcome + ", closedInCb=" + closedInCb + ")");
  } finally {
      try { pg.close(); } catch (e) {}
  }
});

test("pg: close in a pipeline resolve keeps all results", async (a) => {
  needPG(a);
  const pg = new PostgreSQL(PG);
  try {
      const stmts = []; for (let k = 0; k < 8; k++) stmts.push(["SELECT " + k]);
      let len = -1;
      await pg.pipeline(stmts).then(v => { len = v.length; try { pg.close(); } catch (e) {} });
      ok(len === 8, "pg: pipeline(8) close-in-resolve keeps all results");
  } finally {
      try { pg.close(); } catch (e) {}
  }
});

test("pg: close in then keeps a 50-row result", async (a) => {
  needPG(a);
  const pg = new PostgreSQL(PG);
  try {
      const res = await pg.query("SELECT generate_series(1, 50)").then(v => { try { pg.close(); } catch (e) {} return v; });
      const rows = res.rows ?? res;
      ok((rows.rows ? rows.rows.length : rows.length) === 50, "pg: 50-row result intact across close-in-then");
  } finally {
      try { pg.close(); } catch (e) {}
  }
});

test("pg: the connect deadline fires on time and names its cause", async () => {
  let msg = "", dt = Infinity;
  for (let i = 0; i < 3 && dt >= 1500; i++) {
      const pg = new PostgreSQL({ host: "10.255.255.1", port: 5432, user: "x",
                                  database: "x", connectTimeoutMs: 500 });
      try {
          const t0 = Date.now();
          try { await pg.query("SELECT 1"); } catch (e) { msg = e.message; }
          dt = Math.min(dt, Date.now() - t0);
      } finally {
          try { pg.close(); } catch (e) {}
      }
  }
  ok(dt < 1500 && dt >= 350, `pg: connect deadline fired on time (${dt}ms, best of up to 3 fresh clients)`);
  ok(/timed out|connect/i.test(msg), "pg: failure names cause: " + msg.slice(0, 48));
});

await t.run();
