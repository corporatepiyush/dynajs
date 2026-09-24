// flags: --std
// timeout: 300
/* test_net_teardown_churn.js -- the dispose/teardown paths under live load.
 *
 * The repro class is exit shape, not happy path: every mode below opens real
 * resources with work IN FLIGHT (commands sent, queries parked, connects half
 * made, datagrams awaited) and then leaves by a different door --
 *
 *   exit      std.exit() with everything still open and pending;
 *   uncaught  an uncaught exception with everything still open and pending;
 *   gc        drop every reference, force the collector, end naturally --
 *             the finalizers' dispose runs with no live context around;
 *   close     explicit close() of everything, in-flight work included;
 *   kill      SIGTERM (unhandled): the process dies with resources armed
 *             (bounded death; SIGKILL is not in os.* by design).
 *
 * What must NOT happen in any door: a use-after-free of a disposed context, a
 * promise settled from a finalizer (which enqueues reactions nobody drains,
 * and JS_FreeRuntime then reports as object leaks), a leaked descriptor or
 * heap block the teardown owed (LSan names the allocator), a wedged loop, or
 * an exit status that is not the door's own.
 *
 * The parent spawns one child per mode and judges ONLY the exit status and
 * the child's stderr: when the binary under test is an ASan/LSan build, every
 * sanitizer report lands in that stderr and fails the row here -- so this one
 * file is both the plain gate and the sanitizer gate.
 *
 * A STALLED CHILD IS A FAIL, NOT A HANG (the fdchurn contract).
 */
import * as std from "std";
import * as os from "os";
import { Spawn, args } from "dyna:sys";
import { TCPServer, UDPSocket, Redis, PostgreSQL, DNSResolver } from "dyna:net";

const SELF = scriptArgs[0];
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

/* ===================== child: one exit-shape scenario ===================== */

async function child(mode, cycles) {
  /* A peer that accepts and NEVER answers: every command stays in flight. */
  const mock = new TCPServer();
  mock.start({ connect: () => {} });
  const mp = mock.port;

  const churnOne = () => {
    /* Every handler captures its own resource: the {resource, handler,
       closure} group is a REFCOUNT CYCLE, so it cannot die with the script's
       references -- it survives to the runtime's final sweep, where the
       context is already gone. That sweep is the dispose shape under test. */
    /* redis: HELLO + PING on the wire, no reply ever */
    const R = new Redis({ port: mp, host: "127.0.0.1" });
    R.on("error", () => { R.command("LATE").catch(() => {}); });
    R.command("PING").catch(() => {});
    /* pg: startup + query, no reply ever */
    const P = new PostgreSQL({ port: mp, host: "127.0.0.1", user: "u" });
    P.on("error", () => { P.query("SELECT late").catch(() => {}); });
    P.query("SELECT 1").catch(() => {});
    /* dns: query parked against a black hole, callback captures the resolver */
    const D = new DNSResolver({ server: "10.255.255.1", port: 53, timeoutMs: 60000 });
    D.query("never.test", 1, () => { D.close(); });
    /* udp: receive parked, handler captures the socket */
    const U = new UDPSocket({ port: 0, host: "127.0.0.1" });
    U.start({ message: (d) => { U.send(d, "127.0.0.1", 1); } });
    /* tcp: a connect in flight to a black hole, handler captures the conn */
    let C;
    C = TCPServer.connect({ host: "10.255.255.1", port: 54321 }, {
      connect: () => { if (C) C.close(); },
    });
    return { R, P, D, U, C };
  };

  for (let i = 0; i < cycles; i++) {
    const h = churnOne();
    await sleep(2);
    h.R.close(); h.P.close(); h.D.close(); h.U.close();
    try { h.C.close(); } catch (_) {}
  }

  if (mode === "close") {
    const h = churnOne();
    await sleep(2);
    h.R.close(); h.P.close(); h.D.close(); h.U.close();
    try { h.C.close(); } catch (_) {}
    mock.close();
    return 0;
  }

  /* The exit-shape set: opened, loaded, and left LIVE through the door. */
  const live = churnOne();
  await sleep(2);
  if (mode === "gc") {
    /* Drop everything and collect: dispose runs from the final sweep. */
    void live;
    std.gc();
    std.gc();
    return 0;
  }
  if (mode === "kill") {
    /* SIGKILL is deliberately not in os.* (uncatchable); SIGTERM with no
       handler is the same abrupt death for this purpose. */
    os.kill(os.getpid(), os.SIGTERM);
    return 99;   /* unreachable */
  }
  if (mode === "exit") {
    /* abrupt, everything armed */
    std.exit(0);
    return 0;
  }
  return 0;
}

/* The uncaught door must be a REAL uncaught exit: an error swallowed by a
   .catch is just another clean exit, and the teardown it exercises is the
   live-context one. A throw at MODULE scope runs the CLI's uncaught path --
   free the context, then the runtime's final sweep disposes the survivors
   with no context left -- which is exactly the dispose shape under test.
   Handlers here capture NOTHING: this is the ticketed repro class (live
   resources at uncaught exit), and it must exit through the door cleanly. */
function uncaughtChild(cycles) {
  const mock = new TCPServer();
  mock.start({ connect: () => {} });
  const mp = mock.port;
  const churnPlain = () => {
    const R = new Redis({ port: mp, host: "127.0.0.1" });
    R.on("error", () => {});
    R.command("PING").catch(() => {});
    const P = new PostgreSQL({ port: mp, host: "127.0.0.1", user: "u" });
    P.on("error", () => {});
    P.query("SELECT 1").catch(() => {});
    const D = new DNSResolver({ server: "10.255.255.1", port: 53, timeoutMs: 60000 });
    D.query("never.test", 1, () => {});
    const U = new UDPSocket({ port: 0, host: "127.0.0.1" });
    U.start({ message: () => {} });
    const C = TCPServer.connect({ host: "10.255.255.1", port: 54321 }, {
      connect: () => {},
    });
  };
  for (let i = 0; i < cycles; i++)
    churnPlain();
  churnPlain();
  throw new Error("teardown churn: uncaught with live resources");
}

/* The cyclic exit, CLOSED: a behaviour pin, not a canary.
 *
 * The SAME uncaught door, but every handler captures its own resource, so
 * {resource, pending capability, closure, scope} is a refcount CYCLE whose
 * only external anchor is the pending entry's capability reference. That
 * reference is C-held; with it marked, the collector can see the graph, and
 * the runtime's shutdown sweep fails the parked capability at exit instead of
 * leaving the graph pinned for ever. The pinned shape:
 *
 *   - the uncaught throw reaches the top (exit code 1, no signal, no
 *     collector-induced mid-run collection -- in-flight churn loses nothing
 *     before the exit);
 *   - no abort: JS_FreeRuntime's gc_obj_list assert never fires, and no
 *     finalizer settles anything (a settle from teardown is the failure the
 *     dispose-path guards exist for).
 *
 * On an engine without the shutdown sweep the C-held anchor is unmarked as
 * well, the graph survives every collection, and JS_FreeRuntime aborts on its
 * gc_obj_list assert -- the row then fails named on the exit shape and on the
 * assert text in the child's stderr, which is the detector direction. */
function uncaughtCyclicChild(cycles) {
  const mock = new TCPServer();
  mock.start({ connect: () => {} });
  const mp = mock.port;
  const churnOne = () => {
    const R = new Redis({ port: mp, host: "127.0.0.1" });
    R.on("error", () => { R.command("LATE").catch(() => {}); });
    R.command("PING").catch(() => {});
    const P = new PostgreSQL({ port: mp, host: "127.0.0.1", user: "u" });
    P.on("error", () => { P.query("SELECT late").catch(() => {}); });
    P.query("SELECT 1").catch(() => {});
    const D = new DNSResolver({ server: "10.255.255.1", port: 53, timeoutMs: 60000 });
    D.query("never.test", 1, () => { D.close(); });
    const U = new UDPSocket({ port: 0, host: "127.0.0.1" });
    U.start({ message: (d) => { U.send(d, "127.0.0.1", 1); } });
    let C;
    C = TCPServer.connect({ host: "10.255.255.1", port: 54321 }, {
      connect: () => { if (C) C.close(); },
    });
  };
  for (let i = 0; i < cycles; i++)
    churnOne();
  churnOne();
  throw new Error("teardown churn: uncaught with cyclic live resources");
}

/* ===================== parent: one child per door ===================== */

async function readAll(src) {
  const buf = new Uint8Array(65536);
  let out = "";
  for (;;) {
    const k = await src.read(buf);
    if (k === 0) break;
    out += new TextDecoder().decode(buf.subarray(0, k));
  }
  return out;
}

let n = 0, fails = 0;
function check(c, m) {
  n++;
  if (!c) { std.err.puts("FAIL: " + m + "\n"); fails++; }
}

const EXPECT = {
  exit:     { code: 0,        signal: null },
  uncaught: { code: 1,        signal: null },
  gc:       { code: 0,        signal: null },
  close:    { code: 0,        signal: null },
  kill:     { code: null,     signal: "SIGTERM" },
  /* The cyclic exit is CLOSED behaviour: the runtime's shutdown sweep fails
     the parked capability, so the uncaught door leaves through the top with a
     clean nonzero exit -- no abort, nothing settles from a finalizer. (See
     uncaughtCyclicChild.) */
  "uncaught-cyclic": { code: 1, signal: null },
};

async function runMode(mode, cycles) {
  const exe = args()[0];
  const p = new Spawn(exe, ["--std", SELF, "--child", mode, String(cycles)]);
  const [o, e, r] = await Promise.all([readAll(p.stdout), readAll(p.stderr), p.wait()]);
  const want = EXPECT[mode];
  check(r.code === want.code && r.signal === want.signal,
        mode + ": exit shape (got code=" + r.code + " signal=" + r.signal +
        ", want code=" + want.code + " signal=" + want.signal + ")");
  /* The sanitizer word-gate: an ASan/LSan/UBSan report, an engine assert or a
     crash message in the child's stderr fails the row -- on a plain binary
     this simply finds nothing. For the cyclic door the same gate is the pin's
     other half: an engine without the shutdown sweep still aborts on the
     gc_obj_list assert, and the assert text in the child's stderr fails the
     row naming the excerpt. */
  const bad = /AddressSanitizer|LeakSanitizer|runtime error|Assertion|assert failed|Segmentation|abort/i;
  const hit = e.match(bad);
  check(!hit, mode + ": child stderr is clean (found: " +
        (hit ? e.slice(Math.max(0, hit.index - 120), hit.index + 200).replace(/\n/g, " | ") : "nothing") + ")");
  if (fails && o) std.err.puts("child stdout [" + mode + "]: " + o + "\n");
}

async function main() {
  const t0 = Date.now();
  const cycles = parseInt(scriptArgs[1] || "25", 10);
  for (const mode of ["close", "gc", "exit", "uncaught", "kill", "uncaught-cyclic"])
    await runMode(mode, cycles);
  print("teardown-churn: checks=" + n + " fails=" + fails +
        " ms=" + (Date.now() - t0));
  std.exit(fails ? 1 : 0);
}

/* The watchdog is the parent's: a wedged child wedges the row otherwise. */
if (scriptArgs[1] === "--child") {
  const cmode = scriptArgs[2] || "exit";
  const ccycles = parseInt(scriptArgs[3] || "25", 10);
  if (cmode === "uncaught" || cmode === "uncaught-cyclic") {
    /* Deliberately NOT wrapped in a promise: the throw must reach the top. */
    if (cmode === "uncaught")
      uncaughtChild(ccycles);
    else
      uncaughtCyclicChild(ccycles);
    std.exit(77);   /* unreachable: the throw above must leave through the top */
  }
  child(cmode, ccycles)
    .then(rc => std.exit(rc))
    .catch(e => {
      std.err.puts("child threw: " + (e && e.stack ? e.stack : e) + "\n");
      std.exit(1);
    });
} else {
  const WATCH_MS = parseInt(std.getenv("TEARDOWN_WATCH_MS") || "120000", 10);
  const started = Date.now();
  const watchdog = setInterval(() => {
    if (Date.now() - started < WATCH_MS) return;
    clearInterval(watchdog);
    std.err.puts("FAIL: test_net_teardown_churn stalled: no child finished " +
                 "for " + WATCH_MS + "ms -- a teardown wedged the exit path\n");
    std.exit(2);
  }, 5000);
  main().catch(e => {
    clearInterval(watchdog);
    std.err.puts("FAIL: parent error: " + (e && e.stack ? e.stack : e) + "\n");
    std.exit(1);
  });
}
