// flags: --std
// solo: measures time, CPU or RSS; a parallel wave turns the numbers into noise
// covers: dyna:http.App
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with each Python client taking the server port as its argument, so the recording proxy sits in front of it
import { sections as __kitSections } from "../lib/kit.js";
// slots: 2
const __kitS = __kitSections("test_http_app_http_perf_regress");
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

__kitS.section("sleep 0.05", () => {
    write(T + "/appfix.js", `
import { App } from "dyna:http";
import * as std from "std";
const app = new App({ port: 0 });
app.rpc("/rpc", { ping: () => "pong" });
app.start();
const f = std.open("${T}/appfix.port", "w");
f.puts(String(app.port));
f.close();
`);
    sh(`rm -f ${T}/appfix.port`); spawnHelper(`./dynajs --std ${T}/appfix.js >${T}/appfix.log 2>&1`, { pidFile: `${T}/appfix.pid` });
    let appport = 0;
    for (let i = 0; i < 200 && !appport; i++) {
        appport = parseInt((cat(T + "/appfix.port") || "").trim(), 10) || 0;
        if (!appport) sh("sleep 0.05");
    }
    assert(appport > 0, "app fixture published its port (log: " +
           cat(T + "/appfix.log").slice(0, 80) + ")");

    write(T + "/a.js", `import { connect, latin1 } from ${KIT};
const P = ${appport};
async function raw(payload, n = 1) {
    const s = await connect(P, "127.0.0.1", { connectTimeoutMs: 3000 });
    await s.write(payload);
    let out = "";
    const deadline = Date.now() + 3000;
    while (out.split("HTTP/1.1").length - 1 < n) {
        const left = deadline - Date.now();
        if (left <= 0) break;
        const r = await s.read({ timeoutMs: left });
        if (r.timedOut) break;
        if (!r.bytes.length) break;
        out += latin1(r.bytes);
    }
    s.abort();
    return out;
}
const code = (out) => out.slice(0, 4) === "HTTP" ? out.slice(9, 12) : null;
const R = {};
const body = '{"jsonrpc":"2.0","method":"ping","id":1}';
let hs = "";
for (let i = 0; i < 70; i++) hs += "X-H" + i + ": v\\r\\n";
let req = "POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: application/json\\r\\n" + hs + "X-H0: again\\r\\nContent-Length: " + body.length + "\\r\\n\\r\\n" + body;
R.dup_past_64 = code(await raw(req)) === "400";
for (const [cnt, tag] of [[252, "h255"], [253, "h256"], [254, "h257"]]) {
    hs = "";
    for (let i = 0; i < cnt; i++) hs += "Y-" + i + ": v\\r\\n";
    req = "POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: application/json\\r\\n" + hs + "Content-Length: " + body.length + "\\r\\n\\r\\n" + body;
    await raw(req);
    R[tag] = code(await raw(req));
}
const long = "/longpath/" + "a".repeat(2000);
R.long_target = code(await raw("GET " + long + " HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"));
print(JSON.stringify(R));
`);
    sh(`${EXE} --std ${T}/a.js > ${T}/a.json 2>${T}/a.err`);
    let R = {};
    try { R = JSON.parse(cat(T + "/a.json")); }
    catch (e) { assert(false, "app probe failed: " + cat(T + "/a.err").slice(0, 120)); }
    assert(R.dup_past_64 === true,
           "duplicate header past 70 distinct names is refused 400 (got " +
           JSON.stringify(R.dup_past_64) + ")");
    assert(R.h255 === "200", "255 headers serves (got " + R.h255 + ")");
    assert(R.h256 === "200", "256 headers serves at the cap (got " + R.h256 + ")");
    assert(R.h257 === "431", "257 headers is 431 one past the cap (got " + R.h257 + ")");
    assert(R.long_target === "404",
           "2000-byte target truncates and routes (404), never 400 (got " + R.long_target + ")");
    sh(`p=$(cat ${T}/appfix.pid 2>/dev/null); ` +
       `case "$p" in ''|*[!0-9]*) ;; *) kill $p 2>/dev/null; sleep 0.2; kill -9 $p 2>/dev/null;; esac`);
});
__kitS.end();

if (fails === 0) {
    print("test_http_perf_regress: all tests passed (" + n + " assertions)");
} else {
    print("test_http_perf_regress: " + fails + " failures of " + n);
    throw new Error("test_http_perf_regress failed");
}
