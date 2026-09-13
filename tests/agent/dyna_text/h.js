// h.js — portable micro test harness for the dyna_text black-box suites.
// Copied from the proven tests/agent/strnum_bb/h.js pattern, extended with
// byte/b64 helpers for the codec modules.
//
// Probe contract: emit per-failure FAIL lines, then a SUMMARY line and
// RESULT PASS / RESULT FAIL. On any failure the probe throws uncaught at
// the end so the process exit code is nonzero on BOTH engines.
//
// dyna:* modules only load on dynajs, so these probes are dynajs-only; the
// ORACLE is python3 stdlib (zlib/csv/base64/tarfile/zipfile/xml.etree/yaml/
// fnmatch/json) run at GENERATION time by the gen_*.py scripts, which bake
// expectations into the probes. Engine-vs-oracle agreement is the test.
var __out = (typeof print === "function") ? function (s) { print(s); }
                                          : function (s) { console.log(s); };
var __pass = 0, __fail = 0;

function __show(v) {
  var t = typeof v;
  if (t === "string") return JSON.stringify(v);
  if (t === "number") { if (v === 0 && 1 / v < 0) return "-0"; return String(v); }
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  if (t === "boolean") return String(v);
  if (t === "function") return "fn";
  if (typeof Uint8Array !== "undefined" && v instanceof Uint8Array)
    return "u8[" + v.length + "]";
  try { return String(v); } catch (e) { return "[unprintable]"; }
}

function assert(cond, msg) {
  if (cond) { __pass++; return; }
  __fail++;
  __out("FAIL assert " + msg + " cond=" + __show(cond));
}
function assert_true(v, msg) {
  if (v === true) { __pass++; return; }
  __fail++;
  __out("FAIL assert_true " + msg + " got=" + __show(v));
}
function assert_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __fail++;
  __out("FAIL assert_eq " + msg + " got=" + a + " want=" + e);
}
function assert_ne(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (!(a === e && typeof actual === typeof expected)) { __pass++; return; }
  __fail++;
  __out("FAIL assert_ne " + msg + " both=" + a);
}
function assert_throws(fn, kind, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __fail++; __out("FAIL assert_throws " + msg + " no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  if (kind && name !== kind) {
    __fail++; __out("FAIL assert_throws " + msg + " threw=" + name + " want=" + kind);
    return;
  }
  __pass++;
}

// ---- binary helpers (self-contained; works on dynajs and node) ----
// Base64 decode without touching dyna:encoding, so any suite can embed
// binary fixtures produced by a python3 oracle.
var __B64A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function b64dec(s) {
  s = String(s).replace(/[\s=]+$/, "");
  var out = new Uint8Array(s.length * 3 >> 2), n = 0, buf = 0, bits = 0, i;
  for (i = 0; i < s.length; i++) {
    var v = __B64A.indexOf(s.charAt(i));
    if (v < 0) throw new SyntaxError("b64dec: bad char at " + i);
    buf = (buf << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; out[n++] = (buf >> bits) & 0xFF; }
  }
  return out.subarray(0, n);
}
function u8eq(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}
function hex2u8(hex) {
  var out = new Uint8Array(hex.length >> 1);
  for (var i = 0; i < out.length; i++)
    out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}
function u8hex(u8) {
  var s = "";
  for (var i = 0; i < u8.length; i++) s += (u8[i] >> 4).toString(16) + (u8[i] & 15).toString(16);
  return s;
}
function b2s(u8) { return new TextDecoder().decode(u8); }
function s2b(s) { return new TextEncoder().encode(s); }

// assert a Uint8Array equals a b64 literal produced by the python3 oracle
function assert_b64(actual, wantB64, msg) {
  var want;
  try { want = b64dec(wantB64); }
  catch (e) { __fail++; __out("FAIL assert_b64 " + msg + " bad-literal"); return; }
  if (u8eq(actual, want)) { __pass++; return; }
  __fail++;
  __out("FAIL assert_b64 " + msg + " gotlen=" + (actual && actual.length) +
        " wantlen=" + want.length);
}
// first differing index (for diagnostics), or -1
function u8diff(a, b) {
  var n = Math.min(a.length, b.length);
  for (var i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return a.length === b.length ? -1 : n;
}

// ---- normalized case comparison (generated probes) ----
function __fnv1a(str) {
  var h = 0x811c9dc5 | 0;
  for (var i = 0; i < str.length; i++) {
    h = h ^ str.charCodeAt(i);
    h = Math.imul(h, 16777619) | 0;
  }
  return (h >>> 0);
}
function __norm(r) {
  var t = typeof r;
  if (t === "string") {
    if (r.length <= 512) return { t: "s", v: r };
    var L = r.length;
    return { t: "S", len: L, fnv: __fnv1a(r), h: r.slice(0, 64),
             a: r.slice(L - 64), m: r.slice((L >> 1) - 32, (L >> 1) + 32) };
  }
  if (t === "number") {
    if (r === 0 && 1 / r < 0) return { t: "n", v: "-0" };
    return { t: "n", v: String(r) };
  }
  if (r === undefined) return { t: "u" };
  if (r === null) return { t: "z" };
  if (t === "boolean") return { t: "b", v: String(r) };
  if (t === "function") return { t: "f" };
  if (typeof Uint8Array !== "undefined" && r instanceof Uint8Array) {
    var hex = "";
    for (var i = 0; i < r.length; i++) hex += (r[i] >> 4).toString(16) + (r[i] & 15).toString(16);
    if (hex.length <= 512) return { t: "x", v: hex };
    return { t: "X", len: r.length, fnv: __fnv1a(hex), h: hex.slice(0, 64),
             a: hex.slice(-64), m: hex.slice((hex.length >> 1) - 32, (hex.length >> 1) + 32) };
  }
  var j;
  try { j = JSON.stringify(r); } catch (e) { return { t: "jerr", m: String(e && e.name) }; }
  if (j === undefined) return { t: "u" };
  if (j.length <= 1024) return { t: "j", v: j };
  var p2 = [j.slice(0, 96), j.slice(-96)];
  return { t: "J", len: j.length, fnv: __fnv1a(j), h: p2[0], a: p2[1] };
}
function __deepEq(a, b) {
  if (a === b) return true;
  if (a === null || b === null || a === undefined || b === undefined) return false;
  if (typeof a !== "object" || typeof b !== "object") return false;
  var ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (var i = 0; i < ka.length; i++) {
    if (!Object.prototype.hasOwnProperty.call(b, ka[i])) return false;
    if (!__deepEq(a[ka[i]], b[ka[i]])) return false;
  }
  return true;
}
function case_eq(id, want, fn) {
  var got;
  try { got = __norm(fn()); }
  catch (e) {
    var n = (e && typeof e.name === "string") ? e.name : "Error";
    got = { t: "e", n: n };
  }
  var w = (want && want.__raw) ? want.v : want;
  if (__deepEq(got, w)) { __pass++; return; }
  __fail++;
  __out("FAIL case " + id + " got=" + JSON.stringify(got).slice(0, 400) +
        " want=" + JSON.stringify(w).slice(0, 400));
}
// error-name-level case (message text is impl-defined)
function case_err(id, wantName, fn) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __fail++; __out("FAIL case " + id + " no-throw want=" + wantName); return; }
  var n = (threw && typeof threw.name === "string") ? threw.name : "?";
  if (n === wantName) { __pass++; return; }
  __fail++;
  __out("FAIL case " + id + " threw=" + n + " want=" + wantName);
}
// assert without baked expectation (engine-under-test invariants only)
function case_true(id, fn) {
  var v;
  try { v = fn(); } catch (e) { __fail++; __out("FAIL case " + id + " threw=" + __show(e)); return; }
  if (v === true) { __pass++; return; }
  __fail++;
  __out("FAIL case " + id + " got=" + __show(v));
}
// must-throw case that also verifies the engine SURVIVED (returns afterwards)
function case_throw_ok(id, fn) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __fail++; __out("FAIL case_throw_ok " + id + " no-throw"); return; }
  __pass++;
}

function summary(tag) {
  __out("SUMMARY " + tag + " pass=" + __pass + " fail=" + __fail);
  if (__fail > 0) {
    __out("RESULT FAIL");
    throw new Error("PROBE FAILED: " + __fail + " failure(s) in " + tag);
  }
  __out("RESULT PASS");
}
