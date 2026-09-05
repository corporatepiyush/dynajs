/* test_worker_native.js -- native modules on a FRESH worker runtime (E1).
 *
 * Static class IDs (dyn_app_class_id and friends) are assigned once per
 * PROCESS; JS_NewClassID reuses the stored id. On the main runtime a second
 * registration fails into the re-export path (test_module_interop); in a
 * Worker, JS_NewClass instead GROWS the fresh runtime's arrays past the
 * stale ids. Both shapes must work: this file pins the worker one by
 * constructing and starting every heavy net class inside a child runtime.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_worker_native.js
 */
import * as os from "os";
import * as std from "std";
import { Exec, cwd } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }

const DYN = cwd() + "/dynajs";

/* The worker script: imports dyna:net + dyna:bytes on the fresh runtime,
 * starts an App and an HTTPServer, reports both ports. parent.onmessage =
 * null terminates it (the documented release), which must let THIS process
 * exit with no further help. */
const WORKER = [
    'import * as os from "os";',
    'import { App, HTTPServer, TCPServer } from "dyna:net";',
    'import { Bytes } from "dyna:bytes";',
    "const parent = os.Worker.parent;",
    "const app = new App({ port: 0 });",
    'app.rpc("/r", { ping: () => "pong" });',
    "app.start();",
    'const srv = new HTTPServer({ port: 0, routes: { "/": "w" } });',
    "srv.start();",
    'const tcp = new TCPServer({ port: 0 });',
    'tcp.start({});',
    "parent.postMessage({ appPort: app.port, srvPort: srv.port,",
    "                     tcpPort: tcp.port, blen: new Bytes('hi').length });",
    "parent.onmessage = null;",
].join("\n");

function write(path, content) {
    const f = std.open(path, "w");
    f.puts(content);
    f.close();
}

print("=== 1. dyna:net classes construct+start on a worker runtime ===");
{
    write("/tmp/worker_nat_wkr.js", WORKER);
    /* The MAIN side must exit once the worker releases: run the whole thing
       under `sh` so a hang is observable as ALIVE after the grace period. */
    write("/tmp/worker_nat_main.js", [
        'import * as os from "os";',
        'import * as std from "std";',
        'const w = new os.Worker("/tmp/worker_nat_wkr.js");',
        "w.onmessage = function (e) { std.out.puts('PORTS ' + JSON.stringify(e.data) + '\\n'); std.out.flush(); w.onmessage = null; };",
        "w.onerror = function (err) { std.out.puts('WORKER-ERR ' + (err.message || err) + '\\n'); std.out.flush(); };",
    ].join("\n"));
    const sh = [
        DYN + " /tmp/worker_nat_main.js >/tmp/worker_nat_out.txt 2>&1 & pid=$!",
        "sleep 5",
        "if kill -0 $pid 2>/dev/null; then echo ALIVE; kill $pid 2>/dev/null; wait $pid 2>/dev/null; else echo DEAD; fi",
        "cat /tmp/worker_nat_out.txt",
    ].join("\n");
    const r = Exec("sh", ["-c", sh], { timeoutMs: 30000 });
    const out = String(r.stdout);
    ok(out.indexOf("ALIVE") < 0, "main exits once the worker releases (" + out.split("\n")[0] + ")");
    const m = out.match(/PORTS (\{.*\})/);
    ok(m, "worker posted its ports");
    if (m) {
        const ports = JSON.parse(m[1]);
        ok(ports.appPort > 0, "App started in worker (port " + ports.appPort + ")");
        ok(ports.srvPort > 0, "HTTPServer started in worker (port " + ports.srvPort + ")");
        ok(ports.tcpPort > 0, "TCPServer constructed in worker (port " + ports.tcpPort + ")");
        ok(ports.blen === 2, "dyna:bytes works in worker");
    }
    ok(out.indexOf("WORKER-ERR") < 0, "no worker error");
}

/* Control: the same shape WITHOUT native modules, to prove the harness
   itself is not what passed. */
print("=== 2. control: plain worker still round-trips ===");
{
    write("/tmp/worker_plain.js", [
        'import * as os from "os";',
        "const parent = os.Worker.parent;",
        "parent.postMessage({ n: 41 + 1 });",
        "parent.onmessage = null;",
    ].join("\n"));
    write("/tmp/worker_plain_main.js", [
        'import * as os from "os";',
        'import * as std from "std";',
        'const w = new os.Worker("/tmp/worker_plain.js");',
        "w.onmessage = function (e) { std.out.puts('GOT ' + e.data.n + '\\n'); w.onmessage = null; };",
        "w.onerror = function (err) { std.out.puts('WORKER-ERR\\n'); };",
    ].join("\n"));
    const sh = [
        DYN + " /tmp/worker_plain_main.js > /tmp/worker_plain_out.txt 2>&1 & pid=$!",
        "sleep 3",
        "if kill -0 $pid 2>/dev/null; then echo ALIVE; kill $pid 2>/dev/null; wait $pid 2>/dev/null; else echo DEAD; fi",
        "cat /tmp/worker_plain_out.txt",
    ].join("\n");
    const r = Exec("sh", ["-c", sh], { timeoutMs: 30000 });
    const out = String(r.stdout);
    ok(out.indexOf("DEAD") >= 0, "plain worker lets the process exit");
    ok(out.indexOf("GOT 42") >= 0, "plain message round-trips (" + out.trim() + ")");
}

if (fails) {
    print("test_worker_native: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_worker_native failed");
}
print("test_worker_native: " + n + " assertions, 0 failures");
