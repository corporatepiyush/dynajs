// covers: dyna:net.TCPServer
// flags: --std
// timeout: 60
// A promise job that fails stops the job drain at once, also when the reactor drains the jobs a
// dispatch queued (dyn_aio_set_after_dispatch, corked so writes after an await leave in one send). The
// drain hands the exception to the loop's own job-error path (js_std_set_job_error_rt: the loop, the
// after-callback drain and js_std_await take it before running any job, exactly as if their own
// JS_ExecutePendingJob had failed), so no job queued behind the failing one runs: a write or print
// after a fatal error never happens, and the exit status and message are the ones the loop gives a job
// that fails outside any dispatch. Re-queuing a rethrow at the back of the queue would run the jobs in
// between first.
// Pinned here, each in a child process with stdout and stderr merged so the order is real: job A
// prints and throws, job B queued behind it prints and writes. "dispatch": both are queued from a TCP
// data handler (drained by the reactor); "timer": both are queued from a timer callback (drained by the
// loop, no cork). In both, A ran, the loop reported A's error, B did not run before that report, the
// peer never received B's byte, the exit status is non-zero, and the two runs agree on exit status and
// on the whole sequence. B may still run AFTER the report: tearing the runtime down drains the jobs
// still queued (js_run_shutdown_sweeps), with or without the cork; that is the loop's behaviour, not
// this hook's.
import * as std from "std";
import { Spawn, args } from "dyna:sys";
import { TCPServer } from "dyna:net";
import { suite } from "../lib/kit.js";

const SELF = scriptArgs[0];

if (scriptArgs[1] === "--child") {
  const mode = scriptArgs[2];
  const queue = (c) => {
    queueMicrotask(() => { std.err.puts("A-ran\n"); throw new Error("job A failed"); });
    queueMicrotask(() => { std.err.puts("B-ran\n"); c.write("B"); });
  };
  const srv = new TCPServer({ port: 0 });
  globalThis.keepServer = srv;
  srv.start({ data: (c, b) => { if (mode === "dispatch") queue(c); } });
  TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
    connect: (c) => { if (mode === "dispatch") c.write("q"); else setTimeout(() => queue(c), 0); },
    data: (c, b) => std.err.puts("PEER-GOT " + String.fromCharCode(...b) + "\n"),
  });
  setTimeout(() => { std.err.puts("NO-ERROR\n"); std.exit(3); }, 5000);
} else {
  const T = suite("test_net_tcp_server_job_error_stop");
  let A = null;
  const check = (c, m) => A.ok(c, m);
  const test = (n, fn, o) => T.test(n, (a) => { A = a; return fn(a); }, o);
  async function readAll(pipe) {
    const buf = new Uint8Array(4096);
    let out = "";
    for (;;) {
      const k = await pipe.read(buf);
      if (k <= 0) break;
      out += String.fromCharCode(...buf.subarray(0, k));
    }
    return out;
  }
  const q = (x) => "'" + String(x).replace(/'/g, "'\\''") + "'";
  async function runChild(mode) {
    const p = new Spawn("/bin/sh", ["-c", "exec " + q(args()[0]) + " --std " + q(SELF) + " --child " + mode + " 2>&1"]);
    const [o, , r] = await Promise.all([readAll(p.stdout), readAll(p.stderr), p.wait()]);
    const seq = o.split("\n").map((l) => l.trim())
      .filter((l) => /^(A-ran|B-ran|NO-ERROR|PEER-GOT .*|Error: job A failed)$/.test(l));
    return { o, seq, code: r.code };
  }
  const runs = {};
  for (const name of ["dispatch", "timer"]) test(name + ": a failing job stops the drain before the job behind it", async () => {
    const r = runs[name] = await runChild(name);
    const at = (m) => r.seq.indexOf(m);
    check(at("A-ran") >= 0, name + ": job A ran (" + r.seq.join(" > ") + ")");
    check(at("Error: job A failed") > at("A-ran"), name + ": the loop reported A's error (" + r.seq.join(" > ") + ")");
    check(at("B-ran") < 0 || at("B-ran") > at("Error: job A failed"),
          name + ": job B, queued behind the failing job, did not run before the error was reported ("
          + r.seq.join(" > ") + ")");
    check(at("PEER-GOT B") < 0, name + ": B's write never reached the peer");
    check(at("NO-ERROR") < 0, name + ": the failure ended the loop (did not run on to the fallback timer)");
    check(r.code !== 0 && r.code !== null, name + ": exit status is non-zero (got " + r.code + ")");
  });
  test("dispatch and timer runs agree on exit status and sequence", () => {
  const d = runs.dispatch, t = runs.timer;
  check(d.code === t.code, "dispatch and timer runs exit with the same status (" + d.code + " vs " + t.code + ")");
  check(d.seq.join(" > ") === t.seq.join(" > "),
        "dispatch and timer runs show the same sequence (" + d.seq.join(" > ") + " vs " + t.seq.join(" > ") + ")");
  }, { dependsOn: ["dispatch: a failing job stops the drain before the job behind it", "timer: a failing job stops the drain before the job behind it"] });
  await T.run();
}
