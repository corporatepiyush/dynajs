// flags: --std
// covers: dyna:http.App
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_app_keepalive");
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

__kitS.section("3. an App (already covered) still keeps the process alive", () => {
print("=== 3. an App (already covered) still keeps the process alive ===");
eq(probe([
    'import { App } from "dyna:http";',
    'const app = new App({ port: 0 });',
    'app.start();',
].join("\n")), "ALIVE", "App keeps the process alive (regression)");
});

__kitS.section("import { App } from \"dyna:net\"", () => {
eq(probe([
    'import { App } from "dyna:http";',
    'const app = new App({ port: 0 });',
    'app.close();',
].join("\n")), "DEAD", "App close() releases the keep-alive");
});

__kitS.section("5. control: a plain script still exits immediately", () => {
print("=== 5. control: a plain script still exits immediately ===");
eq(probe('print("hi");'), "DEAD", "no server, no keep-alive");
});
__kitS.end();

removeAll(new Path(T));
if (fails) {
    print("test_keepalive: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_keepalive failed");
}
print("test_keepalive: " + n + " assertions, 0 failures");
