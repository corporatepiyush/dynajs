/* test_onconnect_hook.js — P3 from SECURITY_COMPAT_PLAN.md.
 * HTTPClient.onConnect: called with the RESOLVED IP between TCP connect
 * and the first request byte. Returning false refuses the peer before any
 * request data leaves — the DNS-rebinding answer for name-based SSRF gates.
 */
import { HTTPServer, HTTPClient } from "dyna:http";

let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; console.log("  FAIL  " + w); } };

const srv = new HTTPServer({ port: 0, routes: { "/ok": "fine" } });
srv.start();

/* 1. allow: the hook fires with the resolved IP; returning true proceeds */
const c1 = new HTTPClient();
let sawIp = "";
c1.onConnect = (ip) => { sawIp = ip; return true; };
const r1 = c1.get(`http://127.0.0.1:${srv.port}/ok`);
ok(r1.status === 200, "request proceeds when the hook allows");
ok(sawIp === "127.0.0.1", "hook received the resolved IP (got " + sawIp + ")");

/* 2. refuse: returning false aborts BEFORE the request is sent */
const c2 = new HTTPClient();
let sawIp2 = "";
c2.onConnect = (ip) => { sawIp2 = ip; return false; };
let refused = false;
try { c2.get(`http://127.0.0.1:${srv.port}/ok`); }
catch (e) { refused = /connect/i.test(String(e)); }
ok(refused, "returning false refuses the connection");
ok(sawIp2 === "127.0.0.1", "the refusing hook also received the IP");

/* 3. no hook: normal behaviour unaffected */
const c3 = new HTTPClient();
const r3 = c3.get(`http://127.0.0.1:${srv.port}/ok`);
ok(r3.status === 200, "no hook: request works normally");

/* 4. a throwing hook also refuses (no silent success on handler error) */
const c4 = new HTTPClient();
c4.onConnect = () => { throw new Error("policy"); };
let threw = false;
try { c4.get(`http://127.0.0.1:${srv.port}/ok`); }
catch (e) { threw = true; }
ok(threw, "a throwing hook refuses, not silently proceeds");

/* 5. non-function onConnect is ignored (not a TypeError) */
const c5 = new HTTPClient();
c5.onConnect = "not a function";
const r5 = c5.get(`http://127.0.0.1:${srv.port}/ok`);
ok(r5.status === 200, "non-function onConnect ignored");

c1.close(); c2.close(); c3.close(); c4.close(); c5.close(); srv.close();
console.log("test_onconnect_hook: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_onconnect_hook: " + fail + " failures");
