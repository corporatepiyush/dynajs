// flags: --std
// Three Workers and the main thread fetch at once. It is meaningful under a
// ThreadSanitizer build (the gate runs it there: build.sh g_tsan_workers), where
// state the HTTP client keeps per thread shows up as a data race the moment it
// is shared; on an ordinary build it only checks that every request completes.
import * as os from "os";
import * as std from "std";
import { App } from "dyna:net";
const app = new App({ port: 0 });
app.get("/n", () => "x".repeat(500));
app.start();
const B = "http://127.0.0.1:" + app.port;
const W = (std.getenv("TMPDIR") || "/tmp") + "/dj_tsan_w." + os.getpid() + ".js";
const f = std.open(W, "w");
f.puts(`import * as os from "os";\nlet n = 0, settled = 0;\nfor (let i = 0; i < 40; i++) fetch("${B}/n").then((r) => r.text()).then((s) => { if (s.length === 500) n++; if (++settled === 40) os.Worker.parent.postMessage(n); }, (e) => { if (++settled === 40) os.Worker.parent.postMessage(n); });\n`);
f.close();
let mine = 0;
const got = await new Promise((resolve) => {
  const out = [];
  for (let k = 0; k < 3; k++) { const w = new os.Worker(W); w.onmessage = (e) => { out.push(e.data); w.onmessage = null; if (out.length === 3) resolve(out); }; }
  for (let i = 0; i < 40; i++) fetch(B + "/n").then((r) => r.text()).then(() => mine++, () => 0);
  setTimeout(() => resolve(["TIMEOUT", out]), 30000);
});
os.remove(W);
for (let i = 0; i < 500 && mine < 40; i++) await new Promise((r) => setTimeout(r, 20));
const ok = JSON.stringify(got) === "[40,40,40]" && mine === 40;
print("tsan_worker_fetch: " + (ok ? "ok" : "FAILED " + JSON.stringify(got) + " main " + mine) + " (3 workers x 40 + main x 40 requests)");
app.close();
if (!ok) std.exit(1);
