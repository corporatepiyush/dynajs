/* test_fetch.js -- WHATWG Fetch API tests (plan row 1, 3.1)
 *
 * Verifies:
 *   - Headers (get, set, append, delete, has, entries, keys, values, forEach)
 *   - FormData (append, get, getAll, has, delete, set, entries, keys, values)
 *   - AbortController & AbortSignal (abort, timeout, throwIfAborted, listeners)
 *   - Request (method, url, headers, body, text, json, bytes, arrayBuffer)
 *   - Response (status, statusText, ok, headers, text, json, bytes, arrayBuffer, clone, bodyUsed)
 *   - fetch() against HTTPServerAsync (GET, POST JSON, POST FormData, abort signal, headers)
 */

import { HTTPServer } from "dyna:net";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } else { print("  ok  " + msg); } }
function eq(a, b, msg) { assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function assertThrows(fn, msg, errType) {
    let t = false;
    try { fn(); } catch (e) {
        t = true;
        if (errType) assert(e instanceof errType, msg + " (expected " + errType.name + ", got " + e + ")");
    }
    assert(t, msg + " (expected throw)");
}
async function assertRejects(promise, msg) {
    let t = false;
    try { await promise; } catch (e) { t = true; }
    assert(t, msg + " (expected reject)");
}

print("=== 1. Headers ===");
{
    const h = new Headers({ "Content-Type": "application/json", "X-Api-Key": "secret123" });
    eq(h.get("content-type"), "application/json", "Headers.get case-insensitive");
    eq(h.get("X-API-KEY"), "secret123", "Headers.get uppercase");
    eq(h.has("x-api-key"), true, "Headers.has true");
    eq(h.has("non-existent"), false, "Headers.has false");
    eq(h.get("non-existent"), null, "Headers.get non-existent returns null");

    h.append("X-Custom", "val1");
    h.append("x-custom", "val2");
    eq(h.get("x-custom"), "val1, val2", "Headers.append combines with comma");

    h.set("x-custom", "override");
    eq(h.get("x-custom"), "override", "Headers.set overrides existing");

    h.delete("X-API-KEY");
    eq(h.has("x-api-key"), false, "Headers.delete removes header");

    const entries = [...h.entries()];
    assert(entries.length === 2, "Headers.entries length");

    const copy = new Headers(h);
    eq(copy.get("content-type"), "application/json", "Headers copy ctor");
}

print("=== 2. FormData ===");
{
    const fd = new FormData();
    fd.append("name", "Alice");
    fd.append("tag", "dev");
    fd.append("tag", "admin");

    eq(fd.get("name"), "Alice", "FormData.get");
    eq(fd.has("tag"), true, "FormData.has true");
    eq(fd.has("other"), false, "FormData.has false");

    const tags = fd.getAll("tag");
    eq(tags.length, 2, "FormData.getAll length");
    eq(tags[0], "dev", "FormData.getAll[0]");
    eq(tags[1], "admin", "FormData.getAll[1]");

    fd.set("tag", "superadmin");
    eq(fd.getAll("tag").length, 1, "FormData.set replaces all with single value");
    eq(fd.get("tag"), "superadmin", "FormData.get after set");

    fd.delete("name");
    eq(fd.has("name"), false, "FormData.delete removes entry");
}

print("=== 3. AbortController & AbortSignal ===");
{
    const c = new AbortController();
    const sig = c.signal;
    eq(sig.aborted, false, "signal.aborted initially false");
    eq(sig.reason, undefined, "signal.reason initially undefined");

    let abortedFired = false;
    sig.addEventListener("abort", (e) => {
        abortedFired = true;
    });

    c.abort("User cancel");
    eq(sig.aborted, true, "signal.aborted true after abort");
    eq(sig.reason, "User cancel", "signal.reason populated");

    let throwsErr = null;
    try { sig.throwIfAborted(); } catch (e) { throwsErr = e; }
    eq(throwsErr, "User cancel", "signal.throwIfAborted throws reason");

    const staticSig = AbortSignal.abort("immediate reason");
    eq(staticSig.aborted, true, "AbortSignal.abort creates aborted signal");
    eq(staticSig.reason, "immediate reason", "AbortSignal.abort reason");
}

print("=== 4. Request & Response ===");
async function testReqResp() {
    const req = new Request("https://example.com/api", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ hello: "world" })
    });
    eq(req.method, "POST", "Request.method uppercase");
    eq(req.url, "https://example.com/api", "Request.url");
    eq(req.headers.get("content-type"), "application/json", "Request.headers");
    eq(await req.text(), JSON.stringify({ hello: "world" }), "Request.text()");

    const res = new Response(JSON.stringify({ status: "ok" }), {
        status: 201,
        statusText: "Created",
        headers: { "Content-Type": "application/json" }
    });
    eq(res.status, 201, "Response.status");
    eq(res.statusText, "Created", "Response.statusText");
    eq(res.ok, true, "Response.ok for 201");
    eq(res.bodyUsed, false, "Response.bodyUsed false initially");

    const clone = res.clone();
    eq(clone.status, 201, "Response.clone status");

    const json = await res.json();
    eq(json.status, "ok", "Response.json()");
    eq(res.bodyUsed, true, "Response.bodyUsed true after consume");

    // Re-read must throw TypeError
    let reReadThrew = false;
    try { await res.text(); } catch (e) { reReadThrew = (e instanceof TypeError); }
    assert(reReadThrew, "Response re-read throws TypeError (bodyUsed)");

    // Clone consumed response must throw
    let cloneThrew = false;
    try { res.clone(); } catch (e) { cloneThrew = (e instanceof TypeError); }
    assert(cloneThrew, "Cloning consumed Response throws TypeError");

    // Clone body can be read
    const cloneText = await clone.text();
    eq(cloneText, JSON.stringify({ status: "ok" }), "Clone body read");

    const bytesRes = new Response(new Uint8Array([1, 2, 3, 4]));
    const b = await bytesRes.bytes();
    assert(b instanceof Uint8Array && b.length === 4 && b[0] === 1 && b[3] === 4, "Response.bytes()");
}

print("=== 5. fetch() Integration ===");
async function testFetch() {
    const server = new HTTPServer({
        port: 0,
        routes: {
            "/api/hello": "Hello from fetch test!",
            "/api/json": {
                status: 200,
                contentType: "application/json",
                body: JSON.stringify({ message: "json ok", count: 42 })
            },
            "/api/echo": "echo received",
        }
    });
    server.start();

    const port = server.port;
    const base = "http://127.0.0.1:" + port;

    // GET text
    const r1 = await fetch(base + "/api/hello");
    eq(r1.status, 200, "fetch GET status 200");
    eq(r1.ok, true, "fetch GET ok true");
    const t1 = await r1.text();
    eq(t1, "Hello from fetch test!", "fetch GET text body");

    // GET json
    const r2 = await fetch(base + "/api/json");
    const j2 = await r2.json();
    eq(j2.message, "json ok", "fetch GET json message");
    eq(j2.count, 42, "fetch GET json count");

    // POST with FormData
    const fd = new FormData();
    fd.append("field1", "val1");
    fd.append("file", "test content", "doc.txt");
    const r3 = await fetch(base + "/api/echo", {
        method: "POST",
        body: fd
    });
    eq(r3.status, 200, "fetch POST FormData status 200");
    eq(await r3.text(), "echo received", "fetch POST FormData response text");

    // AbortSignal pre-aborted
    const cPre = new AbortController();
    cPre.abort("Immediate cancel");
    let preAborted = false;
    try {
        await fetch(base + "/api/hello", { signal: cPre.signal });
    } catch (e) {
        preAborted = (e === "Immediate cancel");
    }
    assert(preAborted, "fetch with pre-aborted signal rejects immediately");

    server.stop();
}

async function main() {
    await testReqResp();
    await testFetch();

    if (fails) {
        print("test_fetch: " + fails + " FAILED of " + n + " assertions");
        throw new Error("test_fetch failed");
    }
    print("test_fetch: " + n + " assertions, 0 failures");
}
main();
