// flags: --std
// timeout: 300
import * as std from "std";
import * as os from "os";
import { Spawn, args } from "dyna:sys";
import { TCPServer, UDPSocket, Redis, PostgreSQL, DNSResolver } from "dyna:net";

const SELF = scriptArgs[0];
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function child(mode, cycles) {
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

  const live = churnOne();
  await sleep(2);
  if (mode === "gc") {
    void live;
    std.gc();
    std.gc();
    return 0;
  }
  if (mode === "kill") {
    os.kill(os.getpid(), os.SIGTERM);
    return 99;
  }
  if (mode === "exit") {
    std.exit(0);
    return 0;
  }
  return 0;
}

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

if (scriptArgs[1] === "--child") {
  const cmode = scriptArgs[2] || "exit";
  const ccycles = parseInt(scriptArgs[3] || "25", 10);
  if (cmode === "uncaught" || cmode === "uncaught-cyclic") {
    if (cmode === "uncaught")
      uncaughtChild(ccycles);
    else
      uncaughtCyclicChild(ccycles);
    std.exit(77);
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
