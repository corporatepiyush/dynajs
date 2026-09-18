// CTL:http
// INDEPENDENT review probe: FIX 5 — fetch abort matrix. Pre-aborted signal,
// abort while in flight, double abort, abort after completion, abort during a
// slow body: none may fatally kill the process; the losing transport
// rejection must be swallowed (process exits 0 with all checks green).
import { fetch, AbortController } from "dyna:net";
import { getEnv } from "dyna:sys";
import { ok, eq, setTag, done } from "../../rh_module.js";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
const BASE = "http://127.0.0.1:" + PORT;
setTag("http.fetch_abort");

var phase = 0;

// 1. abort BEFORE send: fetch rejects with the signal reason immediately
var ac1 = new AbortController();
ac1.abort(new Error("pre-aborted"));
fetch(BASE + "/status?c=200", { signal: ac1.signal }).then(
  function () { ok(false, "pre-aborted fetch resolved"); next(); },
  function (e) { ok(String(e && e.message).indexOf("pre-aborted") >= 0,
                    "pre-aborted fetch rejects with signal reason, got " + e); next(); });

// 2. abort while in flight (server /slow sleeps): race won by abort
function inFlight() {
  var ac = new AbortController();
  var got = false;
  fetch(BASE + "/slow", { signal: ac.signal }).then(
    function () { ok(false, "in-flight abort: fetch resolved"); got = true; },
    function (e) { ok(e !== undefined && e !== null, "in-flight abort rejects"); got = true; });
  setTimeout(function () { ac.abort(new Error("user-abort")); }, 100);
  // give the transport time to notice the disconnect and reject underneath
  setTimeout(function () {
    ok(got, "in-flight abort settled fetch");
    ok(true, "process still alive after in-flight abort (no fatal kill)");
    next();
  }, 900);
}

// 3. abort twice, then a late request still works
function doubleAbort() {
  var ac = new AbortController();
  ac.abort(new Error("first"));
  ac.abort(new Error("second")); // second call must be a no-op
  fetch(BASE + "/status?c=200", { signal: ac.signal }).then(
    function () { ok(false, "double-aborted fetch resolved"); next(); },
    function (e) { ok(String(e && e.message).indexOf("first") >= 0,
                      "double abort rejects with FIRST reason, got " + e); next(); });
}

// 4. abort AFTER completion: fetch already returned; abort must be harmless
function afterDone() {
  var ac = new AbortController();
  fetch(BASE + "/status?c=200", { signal: ac.signal }).then(
    function (r) {
      eq(r.status, 200, "abort-after fetch resolved 200");
      ac.abort(new Error("too-late"));
      ok(true, "abort after completion did not kill");
      setTimeout(next, 300); /* let any late rejection surface */
    },
    function (e) { ok(false, "abort-after fetch rejected: " + e); setTimeout(next, 300); });
}

// 5. abort during slow-body download, twice in a row back-to-back requests
function slowAbort() {
  var ac = new AbortController();
  fetch(BASE + "/slow", { signal: ac.signal }).then(
    function () { ok(false, "slow-abort resolved"); doneAll(); },
    function () { ok(true, "slow-abort rejected"); doneAll(); });
  setTimeout(function () { ac.abort(); ac.abort(); }, 80);
  var doneAll = function () { setTimeout(next, 500); };
}

// 6. control: un-aborted fetch through the same server still works after all
//    the abort churn (transport not poisoned)
function control() {
  fetch(BASE + "/status?c=200").then(
    function (r) { eq(r.status, 200, "control fetch after aborts works"); finish(); },
    function (e) { ok(false, "control fetch failed: " + e); finish(); });
}

var order = [inFlight, slowAbort, doubleAbort, afterDone, control][0];
var steps = [inFlight, slowAbort, doubleAbort, afterDone, control];
var si = 0;
function next() {
  phase++;
  if (si < steps.length) { var f = steps[si++]; f(); }
}
function finish() {
  ok(phase >= 5, "all abort phases ran, phase=" + phase);
  setTag("http.fetch_abort");
  done();
}
next();
