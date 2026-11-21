// flags: --std
// covers: dyna:http.App
// tests/http/test_http_app_worker_drain_hooks_leak.js -- F34: every Worker that fetches registers
// drain hooks on its thread's io reactor (dyn_http_async_hook, the keep-alive pool sweep).
// When the Worker's runtime is torn down the reactor is freed with those hooks still
// registered; the hook array was then dropped by count only and its block leaked once per
// Worker thread (LSan: 384 B in 3 allocations from dyn_net_on_drain for three Workers).
// On an ordinary build this only checks that every request completes; the leak itself is
// asserted by tests/ctest/run.sh test-net-hooks-leak, which runs this file under the ASan
// engine with detect_leaks=1 and fails on any report naming dyn_net_on_drain.
import { suite } from "../lib/kit.js";
import * as os from "os";
import * as std from "std";
import { App } from "dyna:http";

const t = suite("test_http_app_worker_drain_hooks_leak");

t.test("three Workers fetch from an App, then their runtimes and reactors are torn down", async (a) => {
    const app = new App({ port: 0 });
    app.get("/n", () => "x".repeat(100));
    app.start();
    const B = "http://127.0.0.1:" + app.port;
    const W = (std.getenv("TMPDIR") || "/tmp") + "/dj_hooks_leak_w." + os.getpid() + ".js";
    const f = std.open(W, "w");
    f.puts(`import * as os from "os";\nlet n = 0, settled = 0;\nfor (let i = 0; i < 4; i++) fetch("${B}/n").then((r) => r.text()).then((s) => { if (s.length === 100) n++; if (++settled === 4) os.Worker.parent.postMessage(n); }, () => { if (++settled === 4) os.Worker.parent.postMessage(n); });\n`);
    f.close();
    let timer;
    try {
        const got = await new Promise((resolve) => {
            const out = [];
            for (let k = 0; k < 3; k++) { const w = new os.Worker(W); w.onmessage = (e) => { out.push(e.data); w.onmessage = null; if (out.length === 3) resolve(out); }; }
            timer = setTimeout(() => resolve(["TIMEOUT", out]), 20000);
        });
        a.eq(JSON.stringify(got), "[4,4,4]", "3 workers x 4 requests all completed");
    } finally {
        clearTimeout(timer);
        os.remove(W);
        app.close();
    }
}, { timeoutMs: 25000 });

await t.run();
print("test_http_app_worker_drain_hooks_leak: ok");
