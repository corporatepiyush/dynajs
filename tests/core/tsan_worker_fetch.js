// flags: --std
// Three Workers and the main thread fetch at once. It is meaningful under a
// ThreadSanitizer build (tests/ctest/run.sh tsan-workers builds one in .obj-tsan and runs it), where
// state the HTTP client keeps per thread shows up as a data race the moment it
// is shared; on an ordinary build it only checks that every request completes.
// Each thread keeps at most 8 requests in flight (4 threads x 8 = 32 connects), below the kernel listen cap
// (macOS kern.ipc.somaxconn 128): 160 simultaneous connects overflowed it under TSan and failed as "connection failed".
import * as os from "os";
import * as std from "std";
import { App } from "dyna:http";
const app = new App({ port: 0 });
app.get("/n", () => "x".repeat(500));
app.start();
const B = "http://127.0.0.1:" + app.port;
const W = (std.getenv("TMPDIR") || "/tmp") + "/dj_tsan_w." + os.getpid() + ".js";
const f = std.open(W, "w");
f.puts(`import * as os from "os";\nlet n = 0;\nfor (let b = 0; b < 5; b++) await Promise.all(Array.from({ length: 8 }, () => fetch("${B}/n").then((r) => r.text()).then((s) => { if (s.length === 500) n++; }, () => 0)));\nos.Worker.parent.postMessage(n);\n`);
f.close();
let mine = 0, mineSettled = 0, firstErr = "";
const got = await new Promise((resolve) => {
  const out = [];
  for (let k = 0; k < 3; k++) { const w = new os.Worker(W); w.onmessage = (e) => { out.push(e.data); w.onmessage = null; if (out.length === 3) resolve(out); }; }
  (async () => { for (let b = 0; b < 5; b++) await Promise.all(Array.from({ length: 8 }, () => fetch(B + "/n").then((r) => r.text()).then(() => { mine++; mineSettled++; }, (e) => { mineSettled++; if (!firstErr) firstErr = String(e); }))); })();
  setTimeout(() => resolve(["TIMEOUT", out]), 30000);
});
os.remove(W);
for (let i = 0; i < 3000 && mineSettled < 40; i++) await new Promise((r) => setTimeout(r, 20));
const ok = JSON.stringify(got) === "[40,40,40]" && mine === 40;
print("tsan_worker_fetch: " + (ok ? "ok" : "FAILED " + JSON.stringify(got) + " main " + mine + " ok of " + mineSettled + " settled" + (firstErr ? "; first error: " + firstErr : "")) + " (3 workers x 40 + main x 40 requests)");
app.close();
if (!ok) std.exit(1);
