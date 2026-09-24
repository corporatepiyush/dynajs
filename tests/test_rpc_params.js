/* test_rpc_params.js -- JSON-RPC params dispatch in App.rpc (B1).
 *
 * The spec allows params as a by-position ARRAY (spread into the handler,
 * the Node/WHATWG convention), a by-name OBJECT (one argument), or OMITTED
 * (zero arguments). Before the fix the array was passed whole -- a handler
 * written (a, b) => a + b received one argument holding an array, and
 * `a + b` came out as a string concatenation. The suite's own handlers had
 * silently adopted the buggy convention ([a, b]) => ..., which is why it
 * stayed green; this file pins the documented convention instead.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_rpc_params.js
 */
import { App, HTTPClient } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

const app = new App({ port: 0 });
app.rpc("/rpc", {
    add: (a, b) => a + b,
    join: (a, b, c) => [a, b, c].join("|"),
    noArgs: (x) => (x === undefined ? "no-arg" : "arg:" + x),
    named: (o) => o.k + ":" + o.v,
    addAsync: (a, b) => new Promise((res) => setTimeout(() => res(a + b), 50)),
});
app.start();

const c = new HTTPClient();
const base = "http://127.0.0.1:" + app.port + "/rpc";
const rpc = (method, params, id) => JSON.stringify(
    params === undefined
        ? { jsonrpc: "2.0", method, id }
        : { jsonrpc: "2.0", method, params, id });
async function call(method, params, id) {
    const r = await c.postAsync(base, rpc(method, params, id));
    return JSON.parse(r.body);
}

print("=== 1. by-position params spread into the handler ===");
eq((await call("add", [40, 2], 1)).result, 42, "add(40, 2) = 42");
eq((await call("join", ["a", "b", "c"], 2)).result, "a|b|c", "three args spread");

print("=== 2. omitted and empty params call with zero arguments ===");
eq((await call("noArgs", undefined, 3)).result, "no-arg", "omitted params -> no args");
eq((await call("noArgs", [], 4)).result, "no-arg", "empty array -> no args");

print("=== 3. by-name object params are a single argument ===");
eq((await call("named", { k: "x", v: 9 }, 5)).result, "x:9", "object passed whole");

print("=== 4. result identity is preserved (no accidental array coercion) ===");
eq((await call("add", [1, 2], 6)).result, 3, "numeric add, not string concat");

print("=== 5. params past the cap are refused with -32602 ===");
{
    const big = new Array(300).fill(1);
    const r = await call("add", big, 7);
    eq(r.error.code, -32602, "error code -32602");
    eq(r.error.message, "Invalid params", "error message");
    ok(r.id === 7, "the id round-trips on the error");
    /* a notification (no id) past the cap must not produce a response slot
       at all -- JSON-RPC 2.0: notifications never answer */
    const nb = await c.postAsync(base, JSON.stringify([
        { jsonrpc: "2.0", method: "add", params: big },
        { jsonrpc: "2.0", method: "add", params: [1, 1], id: 8 },
    ]));
    const narr = JSON.parse(nb.body);
    eq(narr.length, 1, "the notification produced no slot (" + JSON.stringify(narr) + ")");
    eq(narr[0].result, 2, "the second element still answers");
}

print("=== 6. batch calls dispatch positional params ===");
{
    const batch = [
        { jsonrpc: "2.0", method: "add", params: [2, 3], id: 10 },
        { jsonrpc: "2.0", method: "noArgs", id: 11 },
    ];
    const r = await c.postAsync(base, JSON.stringify(batch));
    const arr = JSON.parse(r.body);
    eq(arr.length, 2, "two results");
    eq(arr[0].result, 5, "positional in batch");
    eq(arr[1].result, "no-arg", "omitted params in batch");
}

print("=== 7. async handlers receive the spread args ===");
eq((await call("addAsync", [20, 22], 12)).result, 42, "promise settles with the sum");

app.close();

if (fails) {
    print("test_rpc_params: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_rpc_params failed");
}
print("test_rpc_params: " + n + " assertions, 0 failures");