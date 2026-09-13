import { SQLite } from "dyna:net";
import { Path, makeDir, removeAll, exists } from "dyna:file";
import { Exec, pid as getPid } from "dyna:sys";
// h.js — portable assert harness for dyna_sys black-box probes (dynajs only;
// the dyna:* modules do not exist under node). Pattern follows
// tests/agent/strnum_bb/h.js: per-failure FAIL lines, SUMMARY line, then
// RESULT PASS / RESULT FAIL; any failure throws uncaught so the process exit
// code is nonzero.
var __out = (typeof print === "function") ? function (s) { print(s); }
                                          : function (s) { console.log(s); };
var __pass = 0, __fail = 0;
var __asyncPending = 0;
var __asyncFailed = false;

function __show(v) {
  var t = typeof v;
  if (t === "string") return JSON.stringify(v.length > 120 ? v.slice(0, 117) + "..." : v);
  if (t === "number") { if (v === 0 && 1 / v < 0) return "-0"; return String(v); }
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  if (t === "boolean") return String(v);
  if (t === "function") return "fn";
  if (t === "bigint") return v + "n";
  if (v instanceof Error) return v.name + ": " + v.message;
  try { var j = JSON.stringify(v); if (j && j.length > 200) j = j.slice(0, 197) + "..."; return j; } catch (e) { return "[unprintable]"; }
}

function __failLine(kind, msg, extra) {
  __fail++;
  __asyncFailed = true;
  __out("FAIL " + kind + " " + msg + (extra !== undefined ? " " + extra : ""));
}

function assert(cond, msg) {
  if (cond) { __pass++; return; }
  __failLine("assert", msg, "cond=" + __show(cond));
}
function assert_true(v, msg) {
  if (v === true) { __pass++; return; }
  __failLine("assert_true", msg, "got=" + __show(v));
}
function assert_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __failLine("assert_eq", msg, "got=" + a + " want=" + e);
}
function assert_ne(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (!(a === e && typeof actual === typeof expected)) { __pass++; return; }
  __failLine("assert_ne", msg, "both=" + a);
}
// async variant: assertion evaluated in a promise callback; counts immediately
function assert_a(cond, msg) {
  if (cond) { __pass++; return; }
  __failLine("assert", msg);
}
function assert_a_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __failLine("assert_eq", msg, "got=" + a + " want=" + e);
}
function assert_throws(fn, kind, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __failLine("assert_throws", msg, "no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  if (kind && name !== kind) { __failLine("assert_throws", msg, "threw=" + name + " want=" + kind); return; }
  __pass++;
}
function assert_throws_msg(fn, kind, msgSubstr, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __failLine("assert_throws_msg", msg, "no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  var m = (threw && typeof threw.message === "string") ? threw.message : "";
  if (kind && name !== kind) { __failLine("assert_throws_msg", msg, "threw=" + name + " want=" + kind); return; }
  if (msgSubstr && m.indexOf(msgSubstr) < 0) { __failLine("assert_throws_msg", msg, "msg=" + __show(m) + " want~" + msgSubstr); return; }
  __pass++;
}
// promise-rejection variant: p.then(...) style; checker receives error
function assert_rejects(p, kind, msg, checkMsgSubstr) {
  __asyncPending++;
  p.then(function (v) {
      __failLine("assert_rejects", msg, "resolved with " + __show(v));
      __asyncPending--;
      __maybeDone();
    }, function (e) {
      var name = (e && typeof e.name === "string") ? e.name : "?";
      var m = (e && typeof e.message === "string") ? e.message : "";
      if (kind && name !== kind) __failLine("assert_rejects", msg, "threw=" + name + " want=" + kind);
      else if (checkMsgSubstr && m.indexOf(checkMsgSubstr) < 0) __failLine("assert_rejects", msg, "msg=" + __show(m) + " want~" + checkMsgSubstr);
      else __pass++;
      __asyncPending--;
      __maybeDone();
    });
  return p;
}

// ---- ordered event trace (for event-order assertions) ----
var __trace = [];
function trace(ev) { __trace.push(ev); }
function trace_reset() { __trace = []; }
function assert_trace(expected, msg) {
  var a = JSON.stringify(__trace), e = JSON.stringify(expected);
  if (a === e) { __pass++; return; }
  __failLine("trace", msg, "got=" + a + " want=" + e);
}
function trace_slice() { return __trace.slice(); }

// ---- counter of async completions so summary waits for the loop ----
var __done = false;
var __doneFns = [];
function onDone(fn) { __doneFns.push(fn); }
function __maybeDone() {
  if (__done && __asyncPending === 0) {
    var fns = __doneFns; __doneFns = [];
    for (var i = 0; i < fns.length; i++) { try { fns[i](); } catch (e) { __out("FAIL onDone-handler " + e); __fail++; } }
  }
}
// finish() must be called by the probe when it has scheduled all work whose
// assertions count toward this run. summary() then waits for outstanding
// async assertions (up to timeoutMs) before emitting SUMMARY/RESULT.
var __waiters = [];
var __tag = "";
function waitAsync(ms) { __waiters.push(ms || 100); }
function summary(tag) {
  __tag = tag;
  var waitMs = 0;
  for (var i = 0; i < __waiters.length; i++) waitMs += __waiters[i];
  __done = true;
  if (__asyncPending > 0) {
    // setTimeout drives the loop until pending async assertions settle
    var step = function () {
      if (__asyncPending === 0) { __maybeDone(); finish(); return; }
      if (waitMs <= 0) {
        __out("FAIL async-timeout " + __asyncPending + " async assertion(s) never settled in " + __tag);
        __fail += __asyncPending;
        __asyncPending = 0;
        finish();
        return;
      }
      var chunk = waitMs < 50 ? waitMs : 50;
      waitMs -= chunk;
      setTimeout(step, chunk);
    };
    setTimeout(step, Math.min(50, waitMs || 50));
    return;
  }
  finish();
}
function finish() {
  __out("SUMMARY " + __tag + " pass=" + __pass + " fail=" + __fail);
  if (__fail > 0) {
    __out("RESULT FAIL");
    throw new Error("PROBE FAILED: " + __fail + " failure(s) in " + __tag);
  }
  __out("RESULT PASS");
}

// ---- generated probe sqlite/differential ----

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
