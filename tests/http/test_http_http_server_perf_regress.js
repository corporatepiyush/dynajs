// flags: --std
// solo: measures time, CPU or RSS; a parallel wave turns the numbers into noise
// covers: dyna:http.HTTPServer
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with each Python client taking the server port as its argument, so the recording proxy sits in front of it
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_server_perf_regress");
import { App, HTTPClient, HTTPServer } from "dyna:http";
import { spawnHelper } from "../lib/helper.js";
import * as std from "std";
import * as os from "os";

let n = 0, fails = 0;
function assert(cond, msg) {
    n++;
    if (!cond) { fails++; print("  FAIL: " + msg); }
}

const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_perf.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;

const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
sh(`rm -rf ${T}; mkdir -p ${T}`);

__kitS.section("caps probe failed:", () => {
    const s = new HTTPServer({ port: 0, routes: { "/": "ok\n" } });
    s.start();
    write(T + "/b.js", `import { connect, latin1 } from ${KIT};
const P = ${s.port};
async function raw(payload) {
    const c = await connect(P, "127.0.0.1", { connectTimeoutMs: 3000 });
    await c.write(payload);
    const r = await c.read({ timeoutMs: 3000 });
    c.abort();
    return r.timedOut ? "" : latin1(r.bytes);
}
const R = {};
const pad = "Z-Pad: " + "A".repeat(20000) + "\\r\\n";
R.big_header = (await raw("GET / HTTP/1.1\\r\\nHost: x\\r\\n" + pad + "\\r\\n")).slice(9, 12);
R.big_body = (await raw("POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 20000\\r\\n\\r\\n" + "B".repeat(20000))).slice(9, 12);
const hdr = "GET / HTTP/1.1\\r\\nHost: x\\r\\n" + "Z: " + "a".repeat(16000) + "\\r\\n\\r\\n";
R.head_under_cap = hdr.length < 16 * 1024 ? (await raw(hdr)).slice(9, 12) : "SKIP";
print(JSON.stringify(R));
`);
    sh(`${EXE} --std ${T}/b.js > ${T}/b.json 2>${T}/b.err`);
    let R = {};
    try { R = JSON.parse(cat(T + "/b.json")); }
    catch (e) { assert(false, "caps probe failed: " + cat(T + "/b.err").slice(0, 120)); }
    assert(R.big_header === "431",
           "complete 20KB header block is 431 (got " + R.big_header + ")");
    assert(R.big_body === "413",
           "fitting header + 20KB declared body is 413 (got " + R.big_body + ")");
    if (R.head_under_cap !== "SKIP")
        assert(R.head_under_cap === "200",
               "header block under the cap serves (got " + R.head_under_cap + ")");
    s.close();
});

__kitS.section("flood probe failed:", () => {
    const s = new HTTPServer({ port: 0, routes: { "/": "c\n" } });
    s.start();
    write(T + "/c.js", `import { connect, latin1 } from ${KIT};
const P = ${s.port};
const socks = [];
let errs = 0, oks = 0, denied = 0;
for (let i = 0; i < 400; i++) {
    try { socks.push(await connect(P, "127.0.0.1", { connectTimeoutMs: 5000 })); }
    catch (e) { denied++; }
}
for (const c of socks) {
    try { await c.write("GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"); }
    catch (e) { errs++; }
}
for (const c of socks) {
    const r = await c.read({ timeoutMs: 5000 });
    const out = r.timedOut ? "" : latin1(r.bytes);
    if (out.slice(9, 12) === "200") oks++;
    else errs++;
}
for (const c of socks) c.abort();
print(JSON.stringify({ oks, errs, denied }));
`);
    sh(`${EXE} --std ${T}/c.js > ${T}/c.json 2>${T}/c.err`);
    let R = {};
    try { R = JSON.parse(cat(T + "/c.json")); }
    catch (e) { assert(false, "flood probe failed: " + cat(T + "/c.err").slice(0, 120)); }
    assert(R.oks >= 390,
           "400 simultaneous connections: >=390 answered 200 (got " + R.oks + ")");
    assert(R.errs === 0,
           "zero non-2xx/wrong responses under 400 conns (got " + R.errs + ")");
    s.close();
});

await __kitS.section("sync client served while timer pending", async () => {
    let fired = 0;
    const s = new HTTPServer({ port: 0, routes: { "/": "t\n" } });
    s.start();
    const c = new HTTPClient();
    setTimeout(() => { fired = 1; }, 60);
    const r = await c.get("http://127.0.0.1:" + s.port + "/");
    assert(r.status === 200, "sync client served while timer pending");
    for (let spin = 0; !fired && spin < 100; spin++)
        await sleep(10);
    assert(fired === 1, "JS timer fired through the direct reactor wait");
    c.close();
    s.close();
});
__kitS.end();

if (fails === 0) {
    print("test_http_perf_regress: all tests passed (" + n + " assertions)");
} else {
    print("test_http_perf_regress: " + fails + " failures of " + n);
    throw new Error("test_http_perf_regress failed");
}
