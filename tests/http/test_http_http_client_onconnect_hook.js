// covers: dyna:http.HTTPClient
import { sections as __kitSections } from "../lib/kit.js";
import { HTTPServer, HTTPClient } from "dyna:http";
const __kitS = __kitSections("test_http_http_client_onconnect_hook", { chain: true });
let srv, c1, r1, c2, c3, r3, c4, c5, r5;

let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; console.log("  FAIL  " + w); } };
let sawIp = "";

await __kitS.section("request proceeds when the hook allows", async () => {
srv = new HTTPServer({ port: 0, routes: { "/ok": "fine" } });
srv.start();

c1 = new HTTPClient();
c1.onConnect = (ip) => { sawIp = ip; return true; };
r1 = await c1.get(`http://127.0.0.1:${srv.port}/ok`);
ok(r1.status === 200, "request proceeds when the hook allows");
ok(sawIp === "127.0.0.1", "hook received the resolved IP (got " + sawIp + ")");
});
let sawIp2 = "";
let refused = false;

await __kitS.section("returning false refuses the connection", async () => {
c2 = new HTTPClient();
c2.onConnect = (ip) => { sawIp2 = ip; return false; };
try { await c2.get(`http://127.0.0.1:${srv.port}/ok`); }
catch (e) { refused = /connect/i.test(String(e)); }
ok(refused, "returning false refuses the connection");
ok(sawIp2 === "127.0.0.1", "the refusing hook also received the IP");
});

await __kitS.section("no hook: request works normally", async () => {
c3 = new HTTPClient();
r3 = await c3.get(`http://127.0.0.1:${srv.port}/ok`);
ok(r3.status === 200, "no hook: request works normally");
});
let threw = false;

await __kitS.section("a throwing hook refuses, not silently proceeds", async () => {
c4 = new HTTPClient();
c4.onConnect = () => { throw new Error("policy"); };
try { await c4.get(`http://127.0.0.1:${srv.port}/ok`); }
catch (e) { threw = true; }
ok(threw, "a throwing hook refuses, not silently proceeds");
});

await __kitS.section("non-function onConnect ignored", async () => {
c5 = new HTTPClient();
c5.onConnect = "not a function";
r5 = await c5.get(`http://127.0.0.1:${srv.port}/ok`);
ok(r5.status === 200, "non-function onConnect ignored");
});
__kitS.end();
if (!__kitS.listing) {

c1.close(); c2.close(); c3.close(); c4.close(); c5.close(); srv.close();
console.log("test_onconnect_hook: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_onconnect_hook: " + fail + " failures");
}
