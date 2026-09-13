import { TOML} from "dyna:config";
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

// ---- generated probe config/toml ----

const CASES = {"basic": {"ok": true, "v": {"title": "DynaJS", "port": 8080, "pi": 3.25, "flag": true}}, "table": {"ok": true, "v": {"server": {"host": "127.0.0.1", "port": 8080}, "client": {"retries": 3}}}, "dotted": {"ok": true, "v": {"server": {"host": "localhost", "port": 9000}}}, "aot": {"ok": true, "v": {"items": [{"name": "a"}, {"name": "b"}]}}, "array": {"ok": true, "v": {"nums": [1, 2, 3], "mixed": ["x", "y"]}}, "escapes": {"ok": true, "v": {"s": "a\tb\nc\"d\\e"}}, "literal": {"ok": true, "v": {"s": "C:\\path\\no\\escape"}}, "inline_table": {"ok": true, "v": {"point": {"x": 1, "y": 2}}}, "neg": {"ok": true, "v": {"n": -17, "f": -250.0}}, "dates": {"ok": true, "v": {"d": "1979-05-27", "dt": "1979-05-27T07:32:00Z", "ldt": "1979-05-27T07:32:00", "lt": "07:32:00"}}, "hexnum": {"ok": true, "v": {"h": 3735928559, "o": 493, "b": 13}}, "underscores": {"ok": true, "v": {"big": 1000000}}, "nested": {"ok": true, "v": {"a": {"b": {"c": {"deep": 1}}}}}, "multi_line_string": {"ok": true, "v": {"s": "line1\nline2"}}};
const TEXTS = {"basic": "title = \"DynaJS\"\nport = 8080\npi = 3.25\nflag = true", "table": "[server]\nhost = \"127.0.0.1\"\nport = 8080\n[client]\nretries = 3", "dotted": "server.host = \"localhost\"\nserver.port = 9000", "aot": "[[items]]\nname = \"a\"\n[[items]]\nname = \"b\"", "array": "nums = [1, 2, 3]\nmixed = [\"x\", \"y\"]", "escapes": "s = \"a\\tb\\nc\\\"d\\\\e\"", "literal": "s = 'C:\\path\\no\\escape'", "inline_table": "point = { x = 1, y = 2 }", "neg": "n = -17\nf = -2.5e2", "dates": "d = 1979-05-27\ndt = 1979-05-27T07:32:00Z\nldt = 1979-05-27T07:32:00\nlt = 07:32:00", "hexnum": "h = 0xdeadbeef\no = 0o755\nb = 0b1101", "underscores": "big = 1_000_000", "nested": "[a.b.c]\ndeep = 1", "multi_line_string": "s = \"\"\"\nline1\nline2\"\"\""};

for (const [id, want] of Object.entries(CASES)) {
  if (!want.ok) continue;
  let got = null, err = null;
  try { got = TOML.parse(TEXTS[id]); } catch (e) { err = e; }
  if (err) { assert(false, "toml " + id + " threw " + err.message); continue; }
  // normalize: date/time objects have no JSON form; stringified via the
  // replacer on BOTH sides (python bakes strings for them via default=str)
  const norm = (v) => JSON.stringify(v, (k, x) => {
    if (x !== null && typeof x === "object" && !Array.isArray(x) &&
        x.constructor !== Object) return String(x);
    return x;
  });
  assert_eq(norm(got), norm(want.v), "toml " + id + " matches tomllib");
}

for (const id of Object.keys(TEXTS)) {
  if (!CASES[id] || !CASES[id].ok) continue;
}
// dates: normalize both sides through fromUnix-free string compare
{
  const d = TOML.parse('d = 1979-05-27').d;
  assert_true(d !== undefined && String(d).indexOf("1979") >= 0, "date parsed, value " + String(d));
}

// invalid documents must throw (python tomllib refuses them too)
const BAD = [["leading_zero", "x = 007"], ["dup_key", "x = 1\nx = 2"], ["bare_val", "x = @nope"], ["unclosed_str", "x = \"open"], ["bad_date", "d = 1979-13-45"]];
for (const [id, text] of BAD) {
  let threw = null;
  try { TOML.parse(text); } catch (e) { threw = e; }
  assert_true(threw !== null, "toml bad/" + id + " throws");
}

// stringify round-trip on the common grammar
const doc = TOML.parse('title = "x"\n[server]\nport = 8080\nratio = 1.5');
const text = TOML.stringify(doc);
const doc2 = TOML.parse(text);
assert_eq(JSON.stringify(doc2), JSON.stringify(doc), "stringify->parse round-trip");
assert_true(text.indexOf("port = 8080") >= 0 || text.indexOf("port=8080") >= 0, "stringified text has port");
// quoting in stringify
const q = TOML.parse('k = "has \\"quote\\""');
assert_eq(q.k, 'has "quote"', "escaped quote parses");
// refusals
assert_throws(() => TOML.stringify(42), null, "stringify non-object root throws");
assert_throws(() => TOML.parse("[[[bad]"), null, "malformed table throws");
summary("config.toml");
