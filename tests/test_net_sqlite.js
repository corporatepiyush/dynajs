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

/* ---- readonly is enforced by SQLite, not by us ---- */
db.close();

if (fails === 0) print("test_net_sqlite: all " + n + " checks passed");
else print("test_net_sqlite: " + fails + " FAILED");
}
