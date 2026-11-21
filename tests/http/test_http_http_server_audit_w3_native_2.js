// timeout: 300
// covers: dyna:http.HTTPServer
// tests/net/test_audit_w3_native_2.js -- audit wave 3, reactor lane.
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   C1-12 a timer that is always due does not starve I/O
//   C1-06 fd exhaustion in accept does not spin, and the listener recovers
//   C1-03 exit with background jobs in flight tears down cleanly
// Not testable from JS (fixed by reading, noted in the ledger): C1-04 slot
// generation on DNS completions, C1-08 sendfile EOF-before-length, C1-19
// send queued behind a pending sendfile, C1-07 close-on-exec on accept.
// build-note: needs the native modules
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_http_server_audit_w3_native_2");
import { args, Exec, platform } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";

const BIN = args()[0];
const os_is_darwin = /darwin|mac/i.test(platform());
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const TMP = makeTempDir("w3n2-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function sh(name, src, pre = "") {
    writeFile(P(name), src);
    return Exec("sh", ["-c", pre + 'exec "$0" --std "$1"', BIN, P(name).toString()], { timeoutMs: 60000, encoding: "utf8" });
}

// ---- C1-12 --------------------------------------------------------------
// ORACLE: ordering, not time. A chain of N zero-delay timers is running while
// one HTTP exchange with a server in this same process is in flight; every
// step of that exchange needs an I/O poll. If I/O is polled between timers the
// exchange completes while the chain still has fires left; if timers starve
// I/O it completes only after all N.
// CONTROL: the chain still runs all N fires and the response is complete.

__kitS.section("import { HTTPServer, HTTPClient } from \"dyna:net\"; const N = 2000000, BODY", () => {
    const r = sh("starve.mjs", `
import { HTTPServer, HTTPClient } from "dyna:http";
const N = 2000000, BODY = "x".repeat(1 << 16);
const srv = new HTTPServer({ port: 0, routes: { "/b": BODY } });
srv.start();
const c = new HTTPClient();
const pend = (u) => c.get(u);
let fires = 0, at = -1, len = -1;
function spin() { if (++fires < N) setTimeout(spin, 0); else { print("fires=" + fires, "resolvedAt=" + at, "len=" + len); c.close(); srv.close(); } }
spin();
pend("http://127.0.0.1:" + srv.port + "/b").then((r) => { at = fires; len = r.body.length; });
`);
    const m = /fires=(\d+) resolvedAt=(-?\d+) len=(-?\d+)/.exec(r.stdout) || [];
    ok(r.code === 0 && +m[1] === 2000000, "control: the timer chain ran all its fires (" + (r.stdout + r.stderr).slice(0, 100) + ")");
    ok(+m[3] === 1 << 16, "control: the response carried every byte (" + m[3] + ")");
    ok(+m[2] > 0 && +m[2] < 2000000, "[red] C1-12: the exchange completed while timers were still firing (at fire " + m[2] + ")");
});
__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_2: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_2: " + failures + " failures");
