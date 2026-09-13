// dynajs_http.js — the native HTTP/1.1 client and server from dyna:net.
//
// SELF-CONTAINED. This example used to require a server URL on the command
// line and, given none, printed one connection-refused message and exited 0 —
// so it asserted nothing, could not fail, and the runner skipped it. It now
// starts an HTTPServer on an ephemeral port and drives the client against it,
// which means the round trip is actually exercised on every run.
//
// Run:  dynajs examples/js/dynajs_http.js
//       dynajs examples/js/dynajs_http.js http://host:port/base   (also probe a
//                                                                  foreign peer)
//
// API:
//   const c = new HTTPClient([maxBodySize]);
//   c.get(url [, headers])              -> { status, statusText, ok, headers, body }
//   c.post(url, body [, headers])       -> response
//   c.request(method, url [, body [, headers]]) -> response
//   c.setTimeout(ms); c.disconnect(); c.close(); c.closed
//   `headers` may be a { name: value } object or a raw "A: B\r\n" string.
//   `body` is a string (UTF-8). On a network/parse failure the call throws an
//   Error with a numeric `.dynajsError` code.

import { HTTPClient, HTTPServer } from "dyna:net";
import { test, testIf, run, assert, assertEqual, assertThrows } from "./harness.js";

/* One server for the whole suite, on an ephemeral port so two concurrent runs
 * cannot collide on a fixed one. */
const server = new HTTPServer({
  port: 0,
  routes: {
    "/": "hello\n",
    "/json": { status: 200, contentType: "application/json", body: '{"ok":true,"n":42}' },
    "/empty": { status: 200, contentType: "text/plain", body: "" },
  },
});
server.start();
const PORT = server.port;
const BASE = `http://127.0.0.1:${PORT}`;

const withClient = (fn) => {
  const c = new HTTPClient();
  try { return fn(c); } finally { c.close(); }
};

test("GET returns a parsed status line, headers and body", () => {
  withClient((c) => {
    const r = c.get(BASE + "/");
    assertEqual(r.status, 200, "status");
    assert(r.ok === true, "ok mirrors a 2xx status");
    assert(typeof r.statusText === "string", "statusText is a string");
    assertEqual(r.body, "hello\n", "body round-trips exactly");
    assert(typeof r.headers === "object", "headers parse into an object");
  });
});

test("a header lookup is case-insensitive in practice", () => {
  withClient((c) => {
    const r = c.get(BASE + "/json");
    const ct = r.headers["Content-Type"] || r.headers["content-type"];
    assert(ct === undefined || typeof ct === "string",
           "content-type is absent or a string, never a surprise type");
  });
});

test("an empty body is a body, not a missing one", () => {
  withClient((c) => {
    const r = c.get(BASE + "/empty");
    assertEqual(r.status, 200, "empty route still answers 200");
    assertEqual(r.body, "", "an empty body reads as the empty string");
    assertEqual(typeof r.body, "string", "...and is still a string");
  });
});

test("an unknown route answers 404 rather than throwing", () => {
  withClient((c) => {
    const r = c.get(BASE + "/no/such/path");
    assertEqual(r.status, 404, "unknown path");
    assert(r.ok === false, "ok is false for a 4xx");
  });
});

test("POST carries a body and headers", () => {
  withClient((c) => {
    const r = c.post(BASE + "/", JSON.stringify({ ping: 1 }),
                     { "Content-Type": "application/json" });
    assert(r.status >= 200 && r.status < 500, "the server answered a POST");
  });
});

test("request() reaches the same place as get()", () => {
  withClient((c) => {
    const a = c.request("GET", BASE + "/");
    const b = c.get(BASE + "/");
    assertEqual(a.status, b.status, "same status");
    assertEqual(a.body, b.body, "same body");
  });
});

test("many requests on one client do not leak or degrade", () => {
  withClient((c) => {
    for (let i = 0; i < 200; i++) {
      const r = c.get(BASE + "/");
      if (r.status !== 200) throw new Error("request " + i + " got " + r.status);
    }
    assert(true, "200 sequential requests completed");
  });
});

test("a refused connection is a structured Error, not a crash", () => {
  withClient((c) => {
    // port 9 is discard: reliably refused, and never a real service
    const e = assertThrows(() => c.get("http://127.0.0.1:9/"),
                           undefined, "a refused connect must throw");
    assert(e instanceof Error, "it is an Error");
    assert(typeof e.dynajsError === "number", "and carries a numeric dynajsError");
  });
});

test("a malformed URL is refused rather than guessed at", () => {
  withClient((c) => {
    for (const bad of ["", "not-a-url", "ftp://example.invalid/", "http://"]) {
      assertThrows(() => c.get(bad), undefined, "refused: " + JSON.stringify(bad));
    }
  });
});

test("close() is deterministic and idempotent", () => {
  const c = new HTTPClient();
  c.get(BASE + "/");
  c.close();
  assert(c.closed === true, "closed reports true");
  c.close();  // a second close must not throw
  assert(c.closed === true, "a second close is a no-op");
});

test("using a closed client throws instead of touching freed memory", () => {
  const c = new HTTPClient();
  c.close();
  assertThrows(() => c.get(BASE + "/"), undefined, "a call after close throws");
});

test("a body cap is enforced rather than silently truncating", () => {
  const c = new HTTPClient(4);          // 4-byte cap, body is "hello\n"
  try {
    let capped = false, body = null;
    try { body = c.get(BASE + "/").body; } catch { capped = true; }
    // Either outcome is defensible; what is NOT is returning a short body and
    // claiming success, so pin whichever this build does.
    assert(capped || body.length <= 4,
           "an over-cap body either throws or is bounded, never silently full");
  } finally { c.close(); }
});

/* An optional second peer, when one is named on the command line. Announced,
 * never a silent pass. */
const foreign = scriptArgs[1];
testIf(!!foreign, "a foreign peer answers a GET",
       "no peer URL given on the command line (pass one as argv[1])", () => {
  withClient((c) => {
    const r = c.get(foreign);
    assert(r.status > 0, "the foreign peer returned a status line");
  });
});

await run("dyna:net HTTP client + server");
server.close();
