// dynajs_http.js — native HTTP/1.1 client from the dyna:net module.
//
// dyna:net exposes HTTPClient, self-contained, in-repo. Each client owns a
// private native arena, so .close() reclaims all of its native memory in one
// step (the GC finalizer is only a safety net). A response's native bytes are
// copied into JS strings at the boundary, then freed — nothing native escapes.
//
// Run:  dynajs examples/js/dynajs_http.js [http://host:port/base]
// With no argument it demonstrates the clean error path against a closed port.
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

import { HTTPClient } from "dyna:net";

const base = scriptArgs[1];

function show(label, r) {
  print(label, "->", r.status, r.statusText, "(ok=" + r.ok + ")");
  const ct = r.headers["Content-Type"] || r.headers["content-type"];
  if (ct) print("   content-type:", ct);
  print("   body[0..80]:", JSON.stringify(r.body.slice(0, 80)));
}

if (base) {
  const c = new HTTPClient();
  try {
    show("GET  " + base, c.get(base));
    show("POST " + base,
         c.post(base, JSON.stringify({ ping: Date.now() }),
                { "Content-Type": "application/json" }));
  } finally {
    c.close(); // deterministic native free
  }
  print("closed:", c.closed);
} else {
  // No server given: show that an unreachable host fails cleanly (no crash,
  // no leak) with a structured Error rather than taking the process down.
  const c = new HTTPClient();
  try {
    c.get("http://127.0.0.1:9/"); // discard port: connection refused
  } catch (e) {
    print("caught clean error:", e.message);
    print("   is Error:", e instanceof Error, " dynajsError:", e.dynajsError);
  } finally {
    c.close();
  }
}
