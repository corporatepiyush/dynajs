/* test_net_sqlite.js -- the SQLite client.
 *
 * The case that matters is the injection one: a value containing SQL syntax
 * must be stored and returned as DATA. If it is ever concatenated instead, the
 * quote closes the literal and the rest is executed.
 */
import * as net from "dyna:net";

/* SQLite is LINKED, so a build on a host without it has no SQLite export. Skip
   LOUDLY: a silent skip is indistinguishable from a pass, and this file is in
   NATIVE_TESTS where nobody reads the count. */
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

/* ---- bound parameters, including one that is pure SQL syntax ---- */
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

/* ---- a parameterised WHERE must actually filter ---- */
const one = db.query("SELECT name FROM t WHERE name = ?", ["alice"]);
check(one.length === 1 && one[0].name === "alice",
      "parameterised WHERE returned " + one.length + " rows");

/* ---- a parameter-count mismatch is REFUSED, not silently left NULL ---- */
let threw = false;
try { db.query("SELECT * FROM t WHERE name = ? AND score = ?", ["alice"]); }
catch (e) { threw = true; }
check(threw, "too few parameters must throw -- an unbound parameter turns a " +
             "WHERE into a no-op that quietly matches nothing");

threw = false;
try { db.query("SELECT * FROM t WHERE name = ?"); }
catch (e) { threw = true; }
check(threw, "omitting the parameter array entirely must throw too");

/* ---- types ---- */
db.exec("INSERT INTO t (name, score, raw) VALUES (?, ?, ?)", ["nul", null, null]);
const nulls = db.query("SELECT score, raw FROM t WHERE name = ?", ["nul"]);
check(nulls[0].score === null && nulls[0].raw === null,
      "NULL columns must come back as null");

db.exec("INSERT INTO t (name, score) VALUES (?, ?)", ["big", 0]);
const big = db.query("SELECT 9007199254740993 AS n");
check(typeof big[0].n === "string",
      "an integer past 2^53 must come back as a string, not a lossy double " +
      "(got " + typeof big[0].n + ")");

/* ---- an integral JS number must bind as INTEGER, not REAL ----
   JS has one number type; SQLite has storage classes. Binding 7 as a double
   stores 7.0, and typeof() is the only thing that tells them apart -- reading
   the value back gives 7 either way. */
/* The column is UNTYPED on purpose. A declared INTEGER column has affinity and
   coerces 7.0 to an integer on its own, so it hides what was actually bound --
   the first version of this check passed even with the binding broken. */
db.exec("CREATE TABLE nums (i, r)");
db.exec("INSERT INTO nums (i, r) VALUES (?, ?)", [7, 2.5]);
const kinds = db.query("SELECT typeof(i) AS ti, typeof(r) AS tr FROM nums");
check(kinds[0].ti === "integer",
      "an integral number must bind as INTEGER, stored as " + kinds[0].ti);
check(kinds[0].tr === "real",
      "a fractional number must bind as REAL, stored as " + kinds[0].tr);

/* ---- a byte view binds as a BLOB ----
   The ORACLE is typeof() and length() computed BY SQLITE, not the value read
   back: a Uint8Array stringified to "0,104,105,255,0" reads back as a string
   that looks plausible, and stores 15 bytes of text under a BLOB column name.
   A declared BLOB column has no affinity to repair it either way. */
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

/* An ArrayBuffer is the same value without the view. */
db.exec("DELETE FROM bin");
db.exec("INSERT INTO bin (v) VALUES (?)", [new Uint8Array([7, 8]).buffer]);
check(db.query("SELECT typeof(v) AS ty FROM bin")[0].ty === "blob",
      "a bare ArrayBuffer must bind as a BLOB too");

/* An EMPTY view is a blob, not NULL: sqlite3_bind_blob with a NULL pointer
   binds SQL NULL, which is a different value with a different meaning. */
db.exec("DELETE FROM bin");
db.exec("INSERT INTO bin (v) VALUES (?)", [new Uint8Array(0)]);
check(db.query("SELECT typeof(v) AS ty FROM bin")[0].ty === "blob",
      "an empty view must bind as an empty BLOB, not NULL, got " +
      db.query("SELECT typeof(v) AS ty FROM bin")[0].ty);

/* ---- every other object is refused, not stringified ---- */
for (const [what, v] of [["a plain object", { a: 1 }], ["an array", [1, 2]]]) {
  let objThrew = false;
  try { db.exec("INSERT INTO bin (v) VALUES (?)", [v]); }
  catch (e) { objThrew = /is an object/.test(String(e)); }
  check(objThrew, what + " must be refused, not stored as its toString()");
}

/* ---- a column named __proto__ is a KEY, not a prototype ----
   JS_SetPropertyStr walks the prototype chain, so this column would retarget
   the row's prototype and vanish from the result entirely. */
const pr = db.query("SELECT 1 AS __proto__, 2 AS ok");
check(Object.keys(pr[0]).indexOf("__proto__") !== -1 && pr[0].__proto__ === 1,
      "a column named __proto__ must be a key, not a prototype -- got keys " +
      JSON.stringify(Object.keys(pr[0])) + " value " +
      JSON.stringify(pr[0].__proto__));

/* ---- bigint: exact 64-bit integers, in and out ---- */
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

/* ---- a bad statement throws with SQLite's own message ---- */
threw = false;
try { db.query("SELECT * FROM no_such_table"); } catch (e) { threw = true; }
check(threw, "a bad statement must throw");

/* and the SAME failing text twice in a row -- the second run goes through
   different code (cache eviction on error) and must fail identically */
threw = false;
try { db.query("SELECT * FROM no_such_table"); } catch (e) { threw = true; }
check(threw, "the identical bad statement must still throw on repeat");

db.close();

/* ---- multi-statement exec runs EVERY statement, not just the first ----
 * prepare_v2 compiles ONE statement; everything after the ';' used to be
 * dropped SILENTLY -- tables the caller believed created were not there. */
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

  /* trailing semicolons and whitespace are NOT a second statement */
  const one = m.query("SELECT 40+2 AS n;");
  check(one.length === 1 && one[0].n === 42,
        "a trailing ';' must keep the single-statement path, got " +
        JSON.stringify(one));

  /* query() refuses text carrying a SECOND statement rather than returning
     rows whose shape changes partway through */
  let multiThrew = null;
  try { m.query("SELECT 1 AS a; SELECT 2 AS b"); }
  catch (e) { multiThrew = String(e); }
  check(multiThrew !== null && /one statement/.test(multiThrew),
        "query must refuse multi-statement text, got " + multiThrew);

  /* parameters bind to the FIRST statement only; a later one declaring
     parameters would be silently unbound -- refused, not ignored */
  let paramThrew = null;
  try { m.exec("INSERT INTO a1 VALUES (?); INSERT INTO a1 VALUES (?)", [5]); }
  catch (e) { paramThrew = String(e); }
  check(paramThrew !== null && /first statement/.test(paramThrew),
        "parameters past the first statement must be refused, got " + paramThrew);

  /* empty input is a clean no-op, not a crash on a NULL statement */
  check(m.exec("") === 0, "exec('') must be a no-op");
  check(m.exec("; ; ;") === 0, "exec(';;;') must be a no-op");
  check(JSON.stringify(m.query("")) === "[]", "query('') returns no rows");
  m.close();
}

/* ---- the prepared-statement cache must never change an ANSWER ----
 * Repeated identical SQL hits the cache; every value below is checked against
 * the SAME text run repeatedly, including across a schema change that forces
 * SQLite to recompile under the cached handle. */
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
    /* an interleaved DIFFERENT cached statement must not cross wires */
    if (i % 3 === 0)
      check(c.query("SELECT v FROM other WHERE v = ?", ["none"]).length === 0,
            "interleaved cache entry " + i);
  }
  /* a MISS (distinct text) interleaved with hits */
  check(c.query(q + " ", [4])[0].v === "row4",
        "a distinct text must prepare fresh and still answer");
  /* schema change under the cache: DROP forces recompilation */
  c.exec("DROP TABLE ck");
  c.exec("CREATE TABLE ck (id INTEGER PRIMARY KEY, v TEXT)");
  c.exec("INSERT INTO ck (v) VALUES (?)", ["fresh"]);
  check(c.query(q, [1])[0].v === "fresh",
        "a cached statement must survive a schema change (recompiled), got " +
        JSON.stringify(c.query(q, [1])));
  /* writes stay exact through the cache */
  c.exec("INSERT INTO ck (v) VALUES (?)", ["more"]);
  check(c.query("SELECT count(*) AS c FROM ck")[0].c === 2,
        "inserts through the cache land exactly once");
  /* parameter COUNT mismatch is still refused on a cached hit */
  let cntThrew = false;
  try { c.query(q); } catch (e) { cntThrew = true; }
  check(cntThrew, "missing parameters must throw even on a cache hit");
  /* and the refusal must NOT have poisoned the cached entry: the answer must
     still be the CURRENT contents of the row (id 2 was re-keyed when the
     table was rebuilt above -- the point is that it answers at all) */
  const afterRefusal = c.query(q, [2]);
  check(afterRefusal.length === 1 && afterRefusal[0].v === "more",
        "the statement still answers after a refused call, got " +
        JSON.stringify(afterRefusal));
  /* LRU churn: cycle far more DISTINCT statements than the cache holds, then
     prove an evicted entry re-prepares and still answers -- against a row
     that exists in the REBUILT table (ids 1 and 2 only) */
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

/* ---- readonly must be enforced by SQLite, ATTACH included ----
 * Without an authorizer, a readonly main database can ATTACH a second file
 * WRITABLE and mutate through it -- the flag promised nothing. */
{
  const ro = new SQLite(":memory:", { readonly: true });
  let attachThrew = null;
  try { ro.exec("ATTACH ':memory:' AS evil"); }
  catch (e) { attachThrew = String(e); }
  check(attachThrew !== null && attachThrew !== "null",
        "ATTACH must be denied on a readonly connection, ran clean");
  ro.close();
}

if (fails === 0) print("test_net_sqlite: all " + n + " checks passed");
else print("test_net_sqlite: " + fails + " FAILED");
}
