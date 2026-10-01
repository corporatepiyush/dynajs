#!/usr/bin/env python3
"""gen_sqlite.py — dyna:net's SQLite client, differenced against the sqlite3
CLI on the SAME database file (create/insert/select over one file from both
sides)."""
from probe_lib import emit

IMP = '''import { SQLite } from "dyna:net";
import { Path, makeDir, removeAll, readFile, exists } from "dyna:file";
import { Exec, pid as getPid } from "dyna:sys";
'''

emit("sqlite", "differential", IMP, r'''
const D = new Path(Path.cwd(), "scratch", "sqlite-" + getPid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const dbPath = new Path(D, "t.db");
const CLI = "sqlite3";
function cliJson(sql) {
  const r = Exec(CLI, [String(dbPath), sql]);
  if (r.code !== 0) throw new Error("sqlite3 CLI failed: " + r.stderr);
  return r.stdout;
}

const db = new SQLite(dbPath);
assert_true(typeof db.version === "string" && db.version.length > 0, "db.version present: " + db.version);

// dyna writes; CLI reads the same file
db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, val REAL)");
db.exec("INSERT INTO t (name, val) VALUES (?, ?)", ["alpha", 1.5]);
db.exec("INSERT INTO t (name, val) VALUES (?, ?)", ["be'ta", 2.25]);
const viaCli = cliJson("SELECT name, val FROM t ORDER BY id;");
assert_true(viaCli.indexOf("alpha") >= 0 && viaCli.indexOf("1.5") >= 0, "CLI sees dyna's insert: " + JSON.stringify(viaCli));
assert_true(viaCli.indexOf("be'ta") >= 0, "quote-containing string stored (bound, not interpolated)");

// CLI writes; dyna reads
cliJson("INSERT INTO t (name, val) VALUES ('gamma', 3.0);");
const rows = db.query("SELECT id, name, val FROM t ORDER BY id");
assert_eq(rows.length, 3, "dyna sees CLI's insert");
assert_eq(rows[2].name, "gamma", "row content");
assert_eq(rows[2].val, 3.0, "REAL round-trip");

// type fidelity through INTEGER column: integral double binds as integer
db.exec("CREATE TABLE n (x)");
db.exec("INSERT INTO n VALUES (?)", [5]);
db.exec("INSERT INTO n VALUES (?)", [5.5]);
db.exec("INSERT INTO n VALUES (?)", [true]);
const typeofs = db.query("SELECT x, typeof(x) AS t FROM n").map((r) => r.t).join(",");
assert_eq(typeofs, "integer,real,integer", "bool binds as integer, integral double binds as integer: " + typeofs);

// BLOB round-trip
db.exec("CREATE TABLE b (x BLOB)");
const blob = new Uint8Array(256);
for (let i = 0; i < 256; i++) blob[i] = i;
db.exec("INSERT INTO b VALUES (?)", [blob]);
const got = db.query("SELECT x FROM b")[0].x;
assert_true(got instanceof Uint8Array && got.length === 256 && got[255] === 255, "BLOB round-trips as Uint8Array");

// NULL
db.exec("INSERT INTO t (name, val) VALUES (NULL, NULL)");
assert_eq(db.query("SELECT name FROM t WHERE name IS NULL").length, 1, "NULL round-trip");

// 64-bit integers past 2^53 stay TEXT (exact digits) unless bigint
db.exec("CREATE TABLE big (x)");
db.exec("INSERT INTO big VALUES (?)", ["9223372036854775807"]);
const bigRow = db.query("SELECT x FROM big")[0];
assert_true(bigRow.x === "9223372036854775807" || typeof bigRow.x === "bigint", "64-bit stays exact: " + JSON.stringify(bigRow));

// lastInsertRowId
db.exec("CREATE TABLE ids (id INTEGER PRIMARY KEY, v TEXT)");
db.exec("INSERT INTO ids (v) VALUES (?)", ["one"]);
const rid = db.lastInsertRowId;
assert_true(rid >= 1, "lastInsertRowId >= 1");

// readonly handle: opens without CREATE; ATTACH denied
const roPath = new Path(D, "t.db");
const ro = new SQLite(roPath, { readonly: true });
let attachErr = null;
try { ro.exec("ATTACH DATABASE ? AS x", [String(new Path(D, "evil.db"))]); } catch (e) { attachErr = e; }
assert_true(attachErr !== null, "ATTACH denied on readonly handle");
ro.close();

// refusals
assert_throws(() => db.query("SELECT 1; SELECT 2"), null, "query refuses multi-statement text");
assert_throws(() => db.query("SELECT ?"), null, "unbound parameter refused");
assert_throws(() => db.query("SELECT ? LIMIT 1", [1, 2]), null, "extra params refused");
let objErr = null;
try { db.query("SELECT ? AS x", [{ a: 1 }]); } catch (e) { objErr = e; }
assert_true(objErr !== null, "object parameter refused (no silent stringify)");

// __proto__ column name is a key like any other
const p = db.query('SELECT 1 AS "__proto__"');
assert_eq(p[0]["__proto__"], 1, "__proto__ column accessible");

db.close();
// after close the file still exists and CLI still reads it
assert_eq(exists(dbPath), true, "db file persisted");
cliJson("SELECT count(*) FROM t;");

removeAll(D);
summary("sqlite.differential");
''')
print("gen_sqlite: 1 probe")
