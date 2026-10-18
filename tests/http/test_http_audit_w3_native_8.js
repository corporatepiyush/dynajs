// flags: --std
// Split from tests/test_audit_w3_native_8.js: the http sections.
import * as http from "dyna:http";
import { Path, makeTempDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- N1-22: App registration methods chain ------------------------------
// ORACLE: dynajs.d.ts declares rpc/ws/sse as returning `this`.
// CONTROL: get() chains.
{
    const a = new http.App();
    ok(a.get("/a", () => "x") === a, "control: App.get returns the app");
    ok(a.rpc("/r", { f() { return 1; } }) === a, "[red] N1-22: App.rpc returns the app");
    ok(a.sse("/e", () => {}) === a, "[red] N1-22: App.sse returns the app");
    ok(a.ws("/w", { message() {} }) === a, "[red] N1-22: App.ws returns the app");
}
removeAll(new Path(String(T)));
console.log("test_http_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_audit_w3_native_8: " + fail + " failures");
