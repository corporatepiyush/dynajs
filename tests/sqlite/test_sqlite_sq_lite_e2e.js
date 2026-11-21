// flags: --std
// covers: dyna:sqlite.SQLite
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sqlite_sq_lite_e2e");
import * as std from "std";
import { SQLite } from "dyna:sqlite";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const REDIS_PORT = 6399, PG_PORT = 55432;

__kitS.section("DROP TABLE IF EXISTS e2e", () => {
  const path = `${std.getenv("TMPDIR") || "/tmp"}/dj_e2e.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}.db`;
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
});
__kitS.end();

if (fails === 0) print("test_net_e2e: all " + n + " checks passed");
else print("test_net_e2e: " + fails + " FAILED of " + n);
