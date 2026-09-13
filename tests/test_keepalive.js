/* test_keepalive.js -- a started threaded server keeps the process alive
 * until it is stopped (C1).
 *
 * The acceptor/worker threads are invisible to js_os_poll, so a script whose
 * only work is a started HTTPServer/HTTPServerAsync exited at the end of the
 * script -- the server never served anything, it just died. The fix holds
 * the shared reactor while a server runs; this test proves it by spawning a
 * bare dynajs and asking the shell whether it is still alive after a second.
 * The control (no server) must still exit immediately, and a server that is
 * stopped must let the process exit -- both halves of the contract.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_keepalive.js
 */
import { Exec, cwd, pid } from "dyna:sys";
import { mkdir } from "os";

const DYN = cwd() + "/dynajs";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

const T = "/tmp/ka_test_" + pid();
std.gc();
mkdir(T, 0o755);

function write(path, content) {
    const f = std.open(path, "w");
    f.puts(content);
    f.close();
}

/* The wrapper: start dynajs in the background, wait a second, report whether
   it survived (kill -0), then kill it. Prints exactly one marker line. */
function probe(script) {
    write(T + "/srv.js", script);
    const sh = [
        DYN + ' ' + T + '/srv.js >/dev/null 2>&1 & pid=$!',
        'sleep 1',
        'if kill -0 $pid 2>/dev/null; then echo ALIVE; kill $pid 2>/dev/null; wait $pid 2>/dev/null; else echo DEAD; fi',
    ].join("\n");
    const r = Exec("sh", ["-c", sh], { timeoutMs: 30000 });
    return r.stdout.trim();
}

print("=== 1. HTTPServer alone keeps the process alive ===");
eq(probe([
    'import { HTTPServer } from "dyna:net";',
    'const s = new HTTPServer({ port: 0, routes: { "/": "x" } });',
    's.start();',
].join("\n")), "ALIVE", "a started threaded server holds the process");

print("=== 2. HTTPServerAsync alone keeps the process alive ===");
eq(probe([
    'import { HTTPServerAsync } from "dyna:net";',
    'const s = new HTTPServerAsync({ port: 0, routes: { "/": "x" } });',
    's.start();',
].join("\n")), "ALIVE", "a started async server holds the process");

print("=== 3. an App (already covered) still keeps the process alive ===");
eq(probe([
    'import { App } from "dyna:net";',
    'const app = new App({ port: 0 });',
    'app.start();',
].join("\n")), "ALIVE", "App keeps the process alive (regression)");

print("=== 4. a server that is stopped lets the process exit ===");
eq(probe([
    'import { HTTPServer } from "dyna:net";',
    'const s = new HTTPServer({ port: 0, routes: { "/": "x" } });',
    's.start();',
    's.stop();',
].join("\n")), "DEAD", "stop() releases the keep-alive");
eq(probe([
    'import { HTTPServerAsync } from "dyna:net";',
    'const s = new HTTPServerAsync({ port: 0, routes: { "/": "x" } });',
    's.start();',
    's.stop();',
].join("\n")), "DEAD", "async stop() releases the keep-alive");
eq(probe([
    'import { App } from "dyna:net";',
    'const app = new App({ port: 0 });',
    'app.close();',
].join("\n")), "DEAD", "App close() releases the keep-alive");

print("=== 5. control: a plain script still exits immediately ===");
eq(probe('print("hi");'), "DEAD", "no server, no keep-alive");

if (fails) {
    print("test_keepalive: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_keepalive failed");
}
print("test_keepalive: " + n + " assertions, 0 failures");