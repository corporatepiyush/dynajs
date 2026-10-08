import * as net from "dyna:net";

if (typeof net.SQLite !== "function") {
  print("test_net_sqlite: SKIPPED (built without sqlite3)");
} else {
const SQLite = net.SQLite;

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const db = new SQLite(":memory:");
check(typeof db.version === "string" && db.version.length > 0,
      "the LINKED library version must be reported at runtime, got " + db.version);
print("  sqlite " + db.version);

db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL, raw BLOB)");

const evil = "'; DROP TABLE t; --";
db.exec("INSERT INTO t (name, score) VALUES (?, ?)", ["alice", 1.5]);
db.exec("INSERT INTO t (name, score) VALUES (?, ?)", [evil, 2.0]);
check(db.lastInsertRowId === 2, "lastInsertRowId " + db.lastInsertRowId);

const all = db.query("SELECT id, name, score FROM t ORDER BY id");
check(all.length === 2, "two rows survive; got " + all.length +
      " -- if the table was dropped, the injection executed");
check(all[0].name === "alice", "row 0 name '" + all[0].name + "'");
check(all[1].name === evil,
      "the injection string must round-trip as DATA, got '" + all[1].name + "'");
check(all[0].score === 1.5, "REAL column round trip, got " + all[0].score);

const one = db.query("SELECT name FROM t WHERE name = ?", ["alice"]);
check(one.length === 1 && one[0].name === "alice",
      "parameterised WHERE returned " + one.length + " rows");

let threw = false;
try { db.query("SELECT * FROM t WHERE name = ? AND score = ?", ["alice"]); }
catch (e) { threw = true; }
check(threw, "too few parameters must throw -- an unbound parameter turns a " +
             "WHERE into a no-op that quietly matches nothing");

threw = false;
try { db.query("SELECT * FROM t WHERE name = ?"); }
catch (e) { threw = true; }
check(threw, "omitting the parameter array entirely must throw too");

db.exec("INSERT INTO t (name, score, raw) VALUES (?, ?, ?)", ["nul", null, null]);
const nulls = db.query("SELECT score, raw FROM t WHERE name = ?", ["nul"]);
check(nulls[0].score === null && nulls[0].raw === null,
      "NULL columns must come back as null");

db.exec("INSERT INTO t (name, score) VALUES (?, ?)", ["big", 0]);
const big = db.query("SELECT 9007199254740993 AS n");
check(typeof big[0].n === "string",
      "an integer past 2^53 must come back as a string, not a lossy double " +
      "(got " + typeof big[0].n + ")");

db.exec("CREATE TABLE nums (i, r)");
db.exec("INSERT INTO nums (i, r) VALUES (?, ?)", [7, 2.5]);
const kinds = db.query("SELECT typeof(i) AS ti, typeof(r) AS tr FROM nums");
check(kinds[0].ti === "integer",
      "an integral number must bind as INTEGER, stored as " + kinds[0].ti);
check(kinds[0].tr === "real",
      "a fractional number must bind as REAL, stored as " + kinds[0].tr);

db.exec("CREATE TABLE bin (v BLOB)");
const blobIn = new Uint8Array([0, 104, 105, 255, 0]);
db.exec("INSERT INTO bin (v) VALUES (?)", [blobIn]);
const bl = db.query("SELECT typeof(v) AS ty, length(v) AS len, v FROM bin");
check(bl[0].ty === "blob",
      "a Uint8Array must bind as a BLOB, stored as " + bl[0].ty);
check(bl[0].len === blobIn.length,
      "the BLOB must be the view's bytes, not its decimal text -- SQLite says " +
      bl[0].len + " bytes for " + blobIn.length);
check(bl[0].v instanceof Uint8Array,
      "a BLOB must come back as a Uint8Array -- a bare ArrayBuffer has no " +
      ".length and no indexing, so a caller's loop runs zero times -- got " +
      Object.prototype.toString.call(bl[0].v));
check(blobIn.every((b, i) => b === bl[0].v[i]),
      "the BLOB round trip must preserve embedded NUL and high bytes, got " +
      Array.from(bl[0].v).join(","));

db.exec("DELETE FROM bin");
db.exec("INSERT INTO bin (v) VALUES (?)", [new Uint8Array([7, 8]).buffer]);
check(db.query("SELECT typeof(v) AS ty FROM bin")[0].ty === "blob",
      "a bare ArrayBuffer must bind as a BLOB too");

db.exec("DELETE FROM bin");
db.exec("INSERT INTO bin (v) VALUES (?)", [new Uint8Array(0)]);
check(db.query("SELECT typeof(v) AS ty FROM bin")[0].ty === "blob",
      "an empty view must bind as an empty BLOB, not NULL, got " +
      db.query("SELECT typeof(v) AS ty FROM bin")[0].ty);

for (const [what, v] of [["a plain object", { a: 1 }], ["an array", [1, 2]]]) {
  let objThrew = false;
  try { db.exec("INSERT INTO bin (v) VALUES (?)", [v]); }
  catch (e) { objThrew = /is an object/.test(String(e)); }
  check(objThrew, what + " must be refused, not stored as its toString()");
}

const pr = db.query("SELECT 1 AS __proto__, 2 AS ok");
check(Object.keys(pr[0]).indexOf("__proto__") !== -1 && pr[0].__proto__ === 1,
      "a column named __proto__ must be a key, not a prototype -- got keys " +
      JSON.stringify(Object.keys(pr[0])) + " value " +
      JSON.stringify(pr[0].__proto__));

{
  const bdb = new SQLite(":memory:", { bigint: true });
  const got = bdb.query("SELECT 9007199254740993 AS n")[0].n;
  check(typeof got === "bigint" && got === 9007199254740993n,
        "bigint:true must return an exact BigInt, got " + typeof got + " " + got);
  bdb.exec("CREATE TABLE b (v)");
  bdb.exec("INSERT INTO b VALUES (?)", [9223372036854775807n]);
  check(bdb.query("SELECT v FROM b")[0].v === 9223372036854775807n,
        "a BigInt parameter must bind losslessly at the int64 limit, got " +
        bdb.query("SELECT v FROM b")[0].v);
  bdb.close();
}

threw = false;
try { db.query("SELECT * FROM no_such_table"); } catch (e) { threw = true; }
check(threw, "a bad statement must throw");

threw = false;
try { db.query("SELECT * FROM no_such_table"); } catch (e) { threw = true; }
check(threw, "the identical bad statement must still throw on repeat");

{
  const trick = new Proxy([1], {
    get(t, k) { if (k === "length") throw new Error("no length for you"); return Reflect.get(t, k); },
  });
  let msg = null;
  try { db.query("SELECT * FROM t WHERE id = ?", trick); }
  catch (e) { msg = String(e && e.message); }
  check(msg !== null && msg.includes("no length for you"),
        "the params-length conversion failure propagates unchanged, got " + msg);
}

db.close();

{
  const m = new SQLite(":memory:");
  m.exec("CREATE TABLE a1 (x); CREATE TABLE a2 (y); INSERT INTO a1 VALUES (7)");
  const t2 = m.query("SELECT count(*) AS c FROM sqlite_master WHERE name IN ('a1','a2')");
  check(t2[0].c === 2,
        "multi-statement exec must create BOTH tables, saw " + t2[0].c);
  const ins = m.exec(
    "INSERT INTO a1 VALUES (1); INSERT INTO a1 VALUES (2); INSERT INTO a1 VALUES (3)");
  check(ins === 3, "exec must sum changes across statements, got " + ins);
  check(m.query("SELECT count(*) AS c FROM a1")[0].c === 4,
        "all three inserts must have run");

  const one = m.query("SELECT 40+2 AS n;");
  check(one.length === 1 && one[0].n === 42,
        "a trailing ';' must keep the single-statement path, got " +
        JSON.stringify(one));

  let multiThrew = null;
  try { m.query("SELECT 1 AS a; SELECT 2 AS b"); }
  catch (e) { multiThrew = String(e); }
  check(multiThrew !== null && /one statement/.test(multiThrew),
        "query must refuse multi-statement text, got " + multiThrew);

  let paramThrew = null;
  try { m.exec("INSERT INTO a1 VALUES (?); INSERT INTO a1 VALUES (?)", [5]); }
  catch (e) { paramThrew = String(e); }
  check(paramThrew !== null && /first statement/.test(paramThrew),
        "parameters past the first statement must be refused, got " + paramThrew);

  check(m.exec("") === 0, "exec('') must be a no-op");
  check(m.exec("; ; ;") === 0, "exec(';;;') must be a no-op");
  check(JSON.stringify(m.query("")) === "[]", "query('') returns no rows");
  m.close();
}

{
  const c = new SQLite(":memory:");
  c.exec("CREATE TABLE ck (id INTEGER PRIMARY KEY, v TEXT)");
  c.exec("CREATE TABLE other (v TEXT)");
  const q = "SELECT v FROM ck WHERE id = ?";
  for (let i = 1; i <= 5; i++)
    c.exec("INSERT INTO ck (v) VALUES (?)", ["row" + i]);
  for (let i = 1; i <= 10; i++) {
    const r = c.query(q, [3]);
    check(r.length === 1 && r[0].v === "row3",
          "cached hit " + i + " must answer row3, got " + JSON.stringify(r));
    if (i % 3 === 0)
      check(c.query("SELECT v FROM other WHERE v = ?", ["none"]).length === 0,
            "interleaved cache entry " + i);
  }
  check(c.query(q + " ", [4])[0].v === "row4",
        "a distinct text must prepare fresh and still answer");
  c.exec("DROP TABLE ck");
  c.exec("CREATE TABLE ck (id INTEGER PRIMARY KEY, v TEXT)");
  c.exec("INSERT INTO ck (v) VALUES (?)", ["fresh"]);
  check(c.query(q, [1])[0].v === "fresh",
        "a cached statement must survive a schema change (recompiled), got " +
        JSON.stringify(c.query(q, [1])));
  c.exec("INSERT INTO ck (v) VALUES (?)", ["more"]);
  check(c.query("SELECT count(*) AS c FROM ck")[0].c === 2,
        "inserts through the cache land exactly once");
  let cntThrew = false;
  try { c.query(q); } catch (e) { cntThrew = true; }
  check(cntThrew, "missing parameters must throw even on a cache hit");
  const afterRefusal = c.query(q, [2]);
  check(afterRefusal.length === 1 && afterRefusal[0].v === "more",
        "the statement still answers after a refused call, got " +
        JSON.stringify(afterRefusal));
  for (let i = 0; i < 100; i++) {
    const r = c.query("SELECT " + i + " AS n");
    check(r[0].n === i, "churn statement " + i + " answered " + JSON.stringify(r));
  }
  const evicted = c.query(q, [1]);
  check(evicted.length === 1 && evicted[0].v === "fresh",
        "an evicted statement re-prepares and still answers, got " +
        JSON.stringify(evicted));
  c.close();
}

{
  const ro = new SQLite(":memory:", { readonly: true });
  let attachThrew = null;
  try { ro.exec("ATTACH ':memory:' AS evil"); }
  catch (e) { attachThrew = String(e); }
  check(attachThrew !== null && attachThrew !== "null",
        "ATTACH must be denied on a readonly connection, ran clean");
  ro.close();
}


{
    const kdb = new SQLite(":memory:");
    kdb.exec("CREATE TABLE kf (a)");
    kdb.exec("INSERT INTO kf VALUES (1);  ");
    const c1 = kdb.exec("DELETE FROM kf;  ");
    kdb.exec("INSERT INTO kf VALUES (1);  ");
    const c2 = kdb.exec("DELETE FROM kf;  ");
    check(c1 === 1 && c2 === 1,
        "exec with trailing whitespace keys and re-hits the cache correctly (" +
        c1 + "/" + c2 + ")");
    kdb.close();
}

if (fails === 0) print("test_net_sqlite: all " + n + " checks passed");
else print("test_net_sqlite: " + fails + " FAILED");
}
