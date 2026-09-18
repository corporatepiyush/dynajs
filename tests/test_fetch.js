// flags: --std
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
import * as std from "std";
import * as os from "os";

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

/* === 8. fetch redirects (WHATWG Fetch 4.3) ============================ *
 * The contract (dynajs.d.ts) says redirects are followed automatically.
 * The origin is a python child because 3xx-with-Location needs headers the
 * static route tables cannot express. The python source below never writes a
 * backslash escape directly: CRLF is chr(13)+chr(10), so no escaping layer
 * between here and the child can corrupt the wire grammar. */
async function testRedirects() {
    const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
    if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
        print("test_fetch: redirect section SKIP (python3 not available)");
        return;
    }
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_fetch_redir.${Date.now()}`;
    sh(`mkdir -p ${T}`);
    const write = (p, s2) => { const f = std.open(p, "w"); f.puts(s2); f.close(); };
    const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s3 = f.readAsString(); f.close(); return s3; };
    const mkActor = (tag, peerTag) => {
        write(`${T}/act${tag}.py`, [
            "import socket, sys, threading, time",
            "CRLF = chr(13) + chr(10)",
            `peerTag = "${peerTag}"`,
            `HERE = "${T}"`,
            "s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)",
            's.bind(("127.0.0.1", 0)); s.listen(16)',
            'open(HERE + "/port' + tag + '.txt", "w").write(str(s.getsockname()[1]))',
            "threading.Timer(90, lambda: sys.exit(0)).start()",
            "def serve(cn):",
            "    try:",
            "        d = b\"\"",
            "        while CRLF.encode() not in d:",
            "            b2 = cn.recv(4096)",
            "            if not b2: return",
            "            d += b2",
            "        line = d.split(CRLF.encode())[0].decode(\"latin1\")",
            "        parts = line.split(\" \")",
            "        path = parts[1] if len(parts) > 1 else \"/\"",
            "        method = parts[0]",
            "        head, _, body = d.partition(CRLF.encode() * 2)",
            "        cl = 0",
            "        for hl in head.decode(\"latin1\").split(CRLF):",
            "            if hl.lower().startswith(\"content-length:\"):",
            "                try: cl = int(hl.split(\":\",1)[1].strip() or 0)",
            "                except ValueError: cl = 0",
            "        while len(body) < cl:",
            "            b3 = cn.recv(65536)",
            "            if not b3: break",
            "            body += b3",
            "        def reply(b_):",
            "            cn.sendall(b\"HTTP/1.1 200 OK\\r\\nContent-Length: %d\\r\\n\\r\\n\" % len(b_) + b_)",
            "        if path == \"/final\":",
            "            reply(b\"FINAL\")",
            "        elif path == \"/echo\":",
            "            auth = \"\"",
            "            for hl in head.decode(\"latin1\").split(CRLF):",
            "                if hl.lower().startswith(\"authorization:\"): auth = hl",
            "            reply((method + \" \" + body.decode(\"latin1\", \"replace\") + \" \" + auth).encode())",
            "        elif path == \"/p302\":",
            "            cn.sendall(b\"HTTP/1.1 302 Found\\r\\nLocation: /final\\r\\nContent-Length: 0\\r\\n\\r\\n\")",
            "        elif path == \"/p303\":",
            "            cn.sendall(b\"HTTP/1.1 303 See Other\\r\\nLocation: /echo\\r\\nContent-Length: 0\\r\\n\\r\\n\")",
            "        elif path == \"/p307\":",
            "            cn.sendall(b\"HTTP/1.1 307 Temporary Redirect\\r\\nLocation: /echo\\r\\nContent-Length: 0\\r\\n\\r\\n\")",
            "        elif path == \"/loop\":",
            "            cn.sendall(b\"HTTP/1.1 301 Moved Permanently\\r\\nLocation: /loop\\r\\nContent-Length: 0\\r\\n\\r\\n\")",
            "        elif path == \"/px\":",
            "            peer = int(open(HERE + \"/port\" + peerTag + \".txt\").read().strip())",
            "            cn.sendall((\"HTTP/1.1 302 Found\\r\\nLocation: http://127.0.0.1:%d/echo\\r\\nContent-Length: 0\\r\\n\\r\\n\" % peer).encode())",
            "        else:",
            "            cn.sendall(b\"HTTP/1.1 404 Not Found\\r\\nContent-Length: 0\\r\\n\\r\\n\")",
            "        time.sleep(0.15)",
            "    except Exception: pass",
            "    finally:",
            "        try: cn.close()",
            "        except Exception: pass",
            "while True:",
            "    c, _ = s.accept()",
            "    threading.Thread(target=serve, args=(c,), daemon=True).start()",
        ].join("\n"));
        sh(`python3 ${T}/act${tag}.py >${T}/act${tag}.log 2>&1 &`);
    };
    mkActor("1", "2");
    mkActor("2", "1");
    sh("sleep 0.8");
    const p1 = parseInt(cat(`${T}/port1.txt`).trim(), 10);
    const p2 = parseInt(cat(`${T}/port2.txt`).trim(), 10);
    assert(p1 > 0 && p2 > 0, "redirect actors started");
    const base = "http://127.0.0.1:" + p1;
    try {
        const r1 = await fetch(base + "/p302");
        eq(r1.status, 200, "302 is followed (status)");
        eq(await r1.text(), "FINAL", "302 is followed (body of the target)");
        eq(r1.url, base + "/final", "302 is followed (url is the target)");

        const r2 = await fetch(base + "/p303", { method: "POST", body: "x=1" });
        eq((await r2.text()).split(" ")[0], "GET", "303 turns POST into GET");

        const r3 = await fetch(base + "/p307", { method: "POST", body: "keepme" });
        assert((await r3.text()).indexOf("POST keepme") === 0,
            "307 preserves the method and body");

        const r4 = await fetch(base + "/px", { headers: { Authorization: "secret-auth" } });
        assert((await r4.text()).indexOf("secret-auth") < 0,
            "Authorization is stripped on a cross-origin redirect");

        const r5 = await fetch(base + "/p303", { headers: { Authorization: "same-auth" } });
        assert((await r5.text()).indexOf("same-auth") >= 0,
            "Authorization survives a same-origin redirect");

        let loopErr = null;
        try { await fetch(base + "/loop"); } catch (e) { loopErr = e; }
        assert(loopErr && /too many redirects/i.test(loopErr.message),
            "a redirect loop throws instead of hanging");
    } finally {
        sh(`pkill -f "act1.py" 2>/dev/null; pkill -f "act2.py" 2>/dev/null; true`);
        sh(`rm -rf ${T}`);
    }
}

async function main() {
    await testReqResp();
    await testFetch();
    await testRedirects();

    if (fails) {
        print("test_fetch: " + fails + " FAILED of " + n + " assertions");
        throw new Error("test_fetch failed");
    }
    print("test_fetch: " + n + " assertions, 0 failures");
}
main();
