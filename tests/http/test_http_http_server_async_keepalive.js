// flags: --std
// covers: dyna:http.HTTPServerAsync
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_server_async_keepalive");
import { args, Exec, pid } from "dyna:sys";
import { mkdir } from "os";
import { Path, removeAll } from "dyna:file";

const DYN = args()[0];
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

__kitS.section("2. HTTPServerAsync alone keeps the process alive", () => {
print("=== 2. HTTPServerAsync alone keeps the process alive ===");
eq(probe([
    'import { HTTPServerAsync } from "dyna:http";',
    'const s = new HTTPServerAsync({ port: 0, routes: { "/": "x" } });',
    's.start();',
].join("\n")), "ALIVE", "a started async server holds the process");
});

__kitS.section("import { HTTPServerAsync } from \"dyna:net\"", () => {
eq(probe([
    'import { HTTPServerAsync } from "dyna:http";',
    'const s = new HTTPServerAsync({ port: 0, routes: { "/": "x" } });',
    's.start();',
    's.stop();',
].join("\n")), "DEAD", "async stop() releases the keep-alive");
});
__kitS.end();

removeAll(new Path(T));
if (fails) {
    print("test_keepalive: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_keepalive failed");
}
print("test_keepalive: " + n + " assertions, 0 failures");
