// CTL:http
// INDEPENDENT review probe: FIX 4 + FIX 6 — a non-HTTP status line must
// REJECT (not fabricate {status:0}); every async entry point (getAsync,
// postAsync, requestAsync) routes through the reject path; sync get throws.
import { HTTPClient, fetch } from "dyna:net";
import { getEnv } from "dyna:sys";
import { ok, eq, track, setTag, done } from "../../rh_module.js";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
const BASE = "http://127.0.0.1:" + PORT;
setTag("http.garbage_status");

var checks = 0;
function settled() { checks++; }

// 1. getAsync on /garbage ("NOT-HTTP AT ALL") must reject with an Error
var c1 = new HTTPClient();
track(c1.getAsync(BASE + "/garbage"), function (err, v) {
  settled();
  ok(err !== null, "getAsync /garbage rejects");
  if (err) {
    ok(err instanceof Error, "getAsync /garbage reject is Error");
    eq(err.dynajsError, 6, "getAsync /garbage dynajsError==ERR_PARSE(6), got " + err.dynajsError);
  } else {
    ok(false, "getAsync /garbage resolved with status=" + v.status + " body=" + String(v.body).slice(0, 40));
  }
});

// 2. postAsync on /garbage must reject the same way
var c2 = new HTTPClient();
track(c2.postAsync(BASE + "/garbage", "x"), function (err, v) {
  settled();
  ok(err !== null && err instanceof Error, "postAsync /garbage rejects as Error");
  if (err) eq(err.dynajsError, 6, "postAsync /garbage dynajsError==6");
});

// 3. requestAsync on /garbage must reject
var c3 = new HTTPClient();
track(c3.requestAsync("GET", BASE + "/garbage"), function (err, v) {
  settled();
  ok(err !== null && err instanceof Error, "requestAsync /garbage rejects as Error");
  if (err) eq(err.dynajsError, 6, "requestAsync /garbage dynajsError==6");
});

// 4. blocking get on /garbage must THROW (sync contract)
var threw = null;
try { new HTTPClient().get(BASE + "/garbage"); } catch (e) { threw = e; }
ok(threw !== null, "sync get /garbage throws");
ok(threw && threw.dynajsError === 6, "sync get /garbage dynajsError==6, got " + (threw && threw.dynajsError));

// 5. control: a real status line still resolves on all three
track(c1.getAsync(BASE + "/status?c=200"), function (err, v) {
  settled();
  ok(err === null, "getAsync /status resolves");
  if (v) eq(v.status, 200, "getAsync /status status==200");
});
track(c3.requestAsync("GET", BASE + "/status?c=201"), function (err, v) {
  settled();
  ok(err === null && v && v.status === 201, "requestAsync /status?c=201 resolves 201");
});

// 6. fetch on /garbage must reject (not resolve a 0-status Response)
track(fetch(BASE + "/garbage"), function (err, v) {
  settled();
  ok(err !== null, "fetch /garbage rejects");
  if (v) ok(false, "fetch /garbage resolved a Response with status=" + v.status);
});

// 7. double-settle guard: exactly one of resolve/reject observed per request.
//    Track settle counts via then-once wrappers on a fresh request.
var settles = 0;
var p7 = c1.getAsync(BASE + "/garbage");
p7.then(function () { settles++; }, function () { settles++; });
setTimeout(function () {
  eq(settles, 1, "exactly one settle per rejected request, got " + settles);
  eq(checks, 6, "all checks settled, got " + checks);
  setTag("http.garbage_status");
  done();
}, 2500);
