import { fetch, AbortController } from "dyna:net";
import { getEnv } from "dyna:sys";
import { ok, eq, setTag, done } from "../../rh_module.js";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
const BASE = "http://127.0.0.1:" + PORT;
setTag("http.fetch_abort");

var phase = 0;

var ac1 = new AbortController();
ac1.abort(new Error("pre-aborted"));
fetch(BASE + "/status?c=200", { signal: ac1.signal }).then(
  function () { ok(false, "pre-aborted fetch resolved"); next(); },
  function (e) { ok(String(e && e.message).indexOf("pre-aborted") >= 0,
                    "pre-aborted fetch rejects with signal reason, got " + e); next(); });

function inFlight() {
  var ac = new AbortController();
  var got = false;
  fetch(BASE + "/slow", { signal: ac.signal }).then(
    function () { ok(false, "in-flight abort: fetch resolved"); got = true; },
    function (e) { ok(e !== undefined && e !== null, "in-flight abort rejects"); got = true; });
  setTimeout(function () { ac.abort(new Error("user-abort")); }, 100);
  setTimeout(function () {
    ok(got, "in-flight abort settled fetch");
    ok(true, "process still alive after in-flight abort (no fatal kill)");
    next();
  }, 900);
}

function doubleAbort() {
  var ac = new AbortController();
  ac.abort(new Error("first"));
  ac.abort(new Error("second"));
  fetch(BASE + "/status?c=200", { signal: ac.signal }).then(
    function () { ok(false, "double-aborted fetch resolved"); next(); },
    function (e) { ok(String(e && e.message).indexOf("first") >= 0,
                      "double abort rejects with FIRST reason, got " + e); next(); });
}

function afterDone() {
  var ac = new AbortController();
  fetch(BASE + "/status?c=200", { signal: ac.signal }).then(
    function (r) {
      eq(r.status, 200, "abort-after fetch resolved 200");
      ac.abort(new Error("too-late"));
      ok(true, "abort after completion did not kill");
      setTimeout(next, 300);
    },
    function (e) { ok(false, "abort-after fetch rejected: " + e); setTimeout(next, 300); });
}

function slowAbort() {
  var ac = new AbortController();
  fetch(BASE + "/slow", { signal: ac.signal }).then(
    function () { ok(false, "slow-abort resolved"); doneAll(); },
    function () { ok(true, "slow-abort rejected"); doneAll(); });
  setTimeout(function () { ac.abort(); ac.abort(); }, 80);
  var doneAll = function () { setTimeout(next, 500); };
}

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
