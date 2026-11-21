// flags: --std
// timeout: 120
// covers: integration dyna:http.HTTPServer dyna:http.HTTPServerAsync -- the workers option is one contract on both server types
// HTTPServer and HTTPServerAsync `workers` are real: start() spawns that many reactor threads
// (default 1, clamped 1..64), each serving the connections it accepted whole; stop() joins every one.
// HTTPServerAsync serves static routes in C, so workers parallelise the whole request path (no JS runs
// per request). Threads are counted from the OS (/proc/self/status on Linux, `ps -M` elsewhere), so a
// server that silently keeps one reactor fails here. On Linux each extra reactor accepts on its own
// SO_REUSEPORT listener (one shared socket left every multishot accept of the io_uring backend on the
// first ring: 4 threads, 1x throughput), counted from /proc/self/net.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_server_workers", { chain: true });
import * as std from "std";
import * as os from "os";
import { HTTPClient, HTTPServer, HTTPServerAsync } from "dyna:http";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));

// Linux tracks reactor threads by tid, not by a process-wide count: a thread left over from the
// previous case (a dynajs thread still exiting; the gate's linux gcc uring leg saw the baseline at 2
// with comm "dynajs" before the first HSA case, then "got 2, want 3") must not shift the verdict.
// start() must add exactly `want` new tids, stop()/close() must end every one of them, and a
// restart adds `want` new tids again. Elsewhere (macOS: `ps -M`) counts are compared against a
// baseline taken once it has held for 5 polls.
function tidList() {
  const [ts, err] = os.readdir("/proc/self/task");
  return err ? null : ts.filter((t) => t[0] !== ".");
}
function threads() {
  const t = tidList();
  if (t) return t.length;
  const [r, w] = os.pipe();
  os.exec(["ps", "-M", "-p", String(os.getpid())], { stdout: w, usePath: true });
  os.close(w);
  const f = std.fdopen(r, "r");
  const out = f.readAsString(); f.close();
  return out.split("\n").filter((l) => l.trim().length).length - 1;
}
function stable(ms = 2000) {
  const t0 = Date.now();
  let n = threads(), same = 0;
  while (same < 5 && Date.now() - t0 < ms) { os.sleep(10); const m = threads(); same = m === n ? same + 1 : 0; n = m; }
  return n;
}
function snap() { const t = tidList(); return t ? { tids: new Set(t) } : { n: stable() }; }
function added(s) { return s.tids ? tidList().filter((t) => !s.tids.has(t)) : threads() - s.n; }
function count(x) { return typeof x === "number" ? x : x.length; }
function alive(s, started) { return s.tids ? tidList().filter((t) => started.includes(t)).length : threads() - s.n; }
// On a thread mismatch (Linux) the FAIL line carries every task as tid:comm; the engine names its
// threads (dyn-reactor-N, dyn-pool, dyn-fiber, dyn-par, dyn-worker, dyn-watchdog), so a leftover
// thread identifies itself in the gate log.
function tasks() {
  const t = tidList();
  return t ? t.map((x) => x + ":" + (std.loadFile("/proc/self/task/" + x + "/comm") || "?").trim()).join(",") : "n/a";
}
const teq = (a, b, m) => eq(a, b, a === b ? m : m + " [tasks " + tasks() + "]");
function settle(fn, want, ms = 2000) {
  const t0 = Date.now();
  let n = fn();
  while (n !== want && Date.now() - t0 < ms) { os.sleep(10); n = fn(); }
  return n;
}

const routes = { "/": "hello", "/j": { status: 200, contentType: "application/json", body: '{"a":1}' } };

async function hammer(port, clients, per) {
  let good = 0;
  const cs = [];
  for (let i = 0; i < clients; i++) cs.push(new HTTPClient());
  for (let r = 0; r < per; r++) {
    for (const c of cs) {
      const res = await c.get("http://127.0.0.1:" + port + (r & 1 ? "/j" : "/"));
      if (res.status === 200 && res.body === (r & 1 ? '{"a":1}' : "hello")) good++;
    }
  }
  for (const c of cs) c.close();
  return good;
}

function listeners(port) {
  let n = 0;
  for (const f of ["/proc/self/net/tcp", "/proc/self/net/tcp6"]) {
    const t = std.loadFile(f);
    if (t === null) return -1;
    for (const l of t.split("\n").slice(1)) {
      const c = l.trim().split(/\s+/);
      if (c.length > 3 && c[3] === "0A" && parseInt(c[1].split(":")[1], 16) === port) n++;
    }
  }
  return n;
}

const base = stable(), base0 = snap(), reactors = [];
ok(base > 0, "thread count readable (" + base + ")");

await __kitS.section("NOTE:", async () => {
for (const [Cls, name] of [[HTTPServer, "HTTPServer"], [HTTPServerAsync, "HTTPServerAsync"]]) {
  for (const [w, want] of [[undefined, 1], [1, 1], [4, 4], [0, 1], [200, 64]]) {
    let pre = snap();
    if (pre.tids && pre.tids.size !== base0.tids.size) print("NOTE: " + name + " workers " + w + ": " + pre.tids.size + " tasks at the baseline, " + base0.tids.size + " at the start [tasks " + tasks() + "]");
    const s = new Cls(w === undefined ? { port: 0, routes } : { port: 0, workers: w, routes });
    teq(count(added(pre)), 0, name + " workers " + w + ": the constructor spawns nothing");
    s.start();
    teq(settle(() => count(added(pre)), want), want, name + " workers " + w + ": start() runs " + want + " reactor thread(s)");
    let started = added(pre);
    if (Array.isArray(started)) reactors.push(...started);
    if (listeners(s.port) >= 0)
      eq(listeners(s.port), want, name + " workers " + w + ": Linux gives each reactor its own SO_REUSEPORT listener");
    if (want <= 4) eq(await hammer(s.port, 32, 4), 128, name + " workers " + w + ": 32 keep-alive clients x 4 requests all served");
    s.stop();
    teq(settle(() => alive(pre, started), 0), 0, name + " workers " + w + ": stop() joins every reactor");
    if (listeners(s.port) >= 0) eq(listeners(s.port), 1, name + " workers " + w + ": stop() closes the per-reactor listeners");
    pre = snap();
    s.start();
    teq(settle(() => count(added(pre)), want), want, name + " workers " + w + ": restart runs " + want + " again");
    started = added(pre);
    if (Array.isArray(started)) reactors.push(...started);
    eq(await hammer(s.port, 4, 2), 8, name + " workers " + w + ": served after restart");
    s.close();
    teq(settle(() => alive(pre, started), 0), 0, name + " workers " + w + ": close() joins every reactor");
  }

  {
    const pre8 = snap();
    const s = new Cls({ port: 0, workers: 8, routes });
    s.start();
    teq(settle(() => count(added(pre8)), 8), 8, name + " workers 8: start() runs 8 reactor threads");
    const started8 = added(pre8);
    const c = new HTTPClient();
    for (let i = 0; i < 200; i++) {
      const r = await c.get("http://127.0.0.1:" + s.port + "/");
      if (r.status !== 200) { ok(false, name + ": one keep-alive connection stays on its reactor: request " + i + " got " + r.status); break; }
    }
    ok(true, name + ": 200 requests on one connection under 8 workers");
    c.close();
    const many = [];
    for (let i = 0; i < 64; i++) { const k = new HTTPClient(); eq((await k.get("http://127.0.0.1:" + s.port + "/j")).status, 200, name + " fresh connection " + i); many.push(k); }
    for (const k of many) k.close();
    s.close();
    teq(settle(() => alive(pre8, started8), 0), 0, name + " workers 8: close() joins every reactor");
  }
}
teq(base0.tids ? settle(() => alive(base0, reactors), 0) : settle(threads, base), base0.tids ? 0 : base, "every reactor gone at the end");
});
__kitS.end();

print(`test_http_server_workers: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
