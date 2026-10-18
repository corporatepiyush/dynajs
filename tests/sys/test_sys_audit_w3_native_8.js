// flags: --std
// Split from tests/test_audit_w3_native_8.js: the sys sections.
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { args, Exec } from "dyna:sys";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- C1-18 / C1-01: a burst of pooled exchanges neither blocks the caller
// nor starves timers ---------------------------------------------------------
// ORACLE: a pending request promises not to block its caller, and a loop that
// serves two sources gives each a turn. 48 exchanges of 8 MiB each go to the
// worker pool at once: more than it holds, and each completion costs
// milliseconds on the loop thread. So (1) issuing them returns at once -- the
// overflow used to run whole exchanges inside the call, 69 ms with no timer
// able to fire -- and (2) a 5 ms interval ticks between the first completion
// and the last; before, completions were delivered back to back (0 ticks).
// The peer is a python server in another process, so nothing here depends on
// this loop serving its own requests.
// CONTROL: every exchange resolves with the whole body.
if (Exec("/bin/sh", ["-c", "python3 -c 'import http.server, socketserver'"], { timeoutMs: 20000 }).code !== 0) {
    console.log("SKIP (python3 absent): C1-18 / C1-01 pooled exchange burst");
} else {
    writeFile(new Path(tp("big.py")), `import http.server, socketserver
BODY = b"x" * (8 << 20)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Length", str(len(BODY))); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(BODY)
    def log_message(self, *a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; request_queue_size = 128
s = S(("127.0.0.1", 0), H); print(s.server_address[1], flush=True); s.serve_forever()
`);
    writeFile(new Path(tp("burst.mjs")), `import { HTTPClient } from "dyna:net";
const port = scriptArgs[1], N = 48;
const pend = (c, u) => (c.getAsync ? c.getAsync(u) : c[Symbol.for("dyna.http.request")]("GET", u));
let ticks = 0, first = -1, last = -1, done = 0, bytes = 0;
const iv = setInterval(() => { ticks++; }, 5);
const cs = [], ps = [], t0 = Date.now();
for (let i = 0; i < N; i++) { const c = new HTTPClient(); cs.push(c); ps.push(pend(c, "http://127.0.0.1:" + port + "/").then((r) => { if (first < 0) first = ticks; last = ticks; done++; bytes += r.body.length; })); }
const issued = Date.now() - t0;
await Promise.all(ps);
clearInterval(iv);
for (const c of cs) c.close();
console.log(JSON.stringify({ done, bytes, between: last - first, issued }));
`);
    const BIN8 = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];
    const r = Exec("/bin/sh", ["-c", `cd '${tp("")}' && (python3 big.py > port.txt 2> py.err & echo $! > py.pid); for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do [ -s port.txt ] && break; sleep 0.2; done; '${BIN8}' burst.mjs "$(cat port.txt)"; rc=$?; kill "$(cat py.pid)" 2>/dev/null; exit $rc`], { timeoutMs: 90000, encoding: "utf8" });
    let o = {};
    try { o = JSON.parse(r.stdout.trim().split("\n").pop()); } catch (e) { o = { err: (r.stdout + r.stderr).slice(0, 200) }; }
    ok(o.done === 48 && o.bytes === 48 * (8 << 20), "control: all 48 pooled exchanges resolved in full (" + JSON.stringify(o).slice(0, 160) + ")");
    ok(o.issued < 30, "[red] C1-01: issuing more exchanges than the pool holds does not run them inside the call (" + o.issued + " ms)");
    ok(o.between >= 2, "[red] C1-18: interval ticks land between completions (" + o.between + " ticks between the first and the last)");
}
removeAll(new Path(String(T)));
console.log("test_sys_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_sys_audit_w3_native_8: " + fail + " failures");
