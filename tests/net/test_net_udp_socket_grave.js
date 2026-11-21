// flags: --std
// timeout: 300
// covers: dyna:net.UDPSocket
import * as std from "std";
import { Spawn, args } from "dyna:sys";
import { UDPSocket } from "dyna:net";
import { suite } from "../lib/kit.js";

const SELF = scriptArgs[0];
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

function recycleRound() {
  return new Promise((done) => {
    const Sx = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const Rx = new UDPSocket({ port: 0, host: "127.0.0.1" });
    let closed = 0;
    Rx.start({ message: () => {
      if (closed) return;
      closed = 1;
      Rx.close();
      Sx.close();
      setTimeout(done, 5);
    } });
    Sx.send(new Uint8Array([120]), "127.0.0.1", Rx.port);
  });
}

async function childRecycleExit(rounds) {
  for (let i = 0; i < rounds; i++)
    await recycleRound();
  return 0;
}

async function childStaleBurst() {
  for (let round = 0; round < 25; round++) {
    const ok = await new Promise((done) => {
      const S = new UDPSocket({ port: 0, host: "127.0.0.1" });
      const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
      const rp = R.port;
      let closed = 0, r2got = 0, R2 = null;
      R.start({ message: () => {
        if (closed) return;
        closed = 1;
        R.close();
        R2 = new UDPSocket({ port: 0, host: "127.0.0.1" });
        R2.start({ message: () => { r2got++; } });
      } });
      for (let k = 0; k < 8; k++)
        S.send(new Uint8Array([98, k]), "127.0.0.1", rp);
      setTimeout(() => {
        if (!R2) { S.close(); done(true); return; }
        S.send(new Uint8Array([104, 105]), "127.0.0.1", R2.port);
        setTimeout(() => {
          const got = r2got >= 1;
          S.close();
          R2.close();
          done(got);
        }, 20);
      }, 20);
    });
    if (!ok) throw new Error("stale-burst: fd-reuse successor lost its traffic");
  }
  return 0;
}

async function childAbruptExit() {
  await new Promise((done) => {
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    R.start({ message: () => {
      R.close();
      std.exit(0);
    } });
    R.send(new Uint8Array([1]), "127.0.0.1", R.port);
    setTimeout(done, 5000);
  });
  return 99;
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

let A = null;
const check = (c, m) => A.ok(c, m);

const BAD = /AddressSanitizer|LeakSanitizer|runtime error|Assertion|assert failed|Segmentation|abort|double free|pointer being freed/i;

async function runRow(childArgs, want, label) {
  const p = new Spawn(args()[0], ["--std", SELF].concat(childArgs));
  const [o, e, r] = await Promise.all([readAll(p.stdout), readAll(p.stderr), p.wait()]);
  const okCode = check(r.code === want, label + ": exit shape (got " + r.code + ", want " + want + ")");
  const hit = e.match(BAD);
  const okErr = check(!hit, label + ": child stderr is clean (found: " +
        (hit ? e.slice(Math.max(0, hit.index - 120), hit.index + 200).replace(/\n/g, " | ") : "nothing") + ")");
  if ((!okCode || !okErr) && o) std.err.puts("child stdout [" + label + "]: " + o + "\n");
}


if (scriptArgs[1] === "--child") {
  const mode = scriptArgs[2] || "recycle-exit";
  const rounds = parseInt(scriptArgs[3] || "25", 10);
  let p;
  if (mode === "recycle-exit") p = childRecycleExit(rounds);
  else if (mode === "stale-burst") p = childStaleBurst();
  else if (mode === "abrupt-exit") p = childAbruptExit();
  else { std.err.puts("unknown child mode " + mode + "\n"); std.exit(2); }
  p.then(rc => std.exit(rc))
   .catch(e => {
     std.err.puts("child threw: " + (e && e.stack ? e.stack : e) + "\n");
     std.exit(1);
   });
} else {
  // Each case runs one child process (the parent itself opens no socket), so
  // nothing starts under --list.
  const t = suite("test_net_udp_socket_grave");
  const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
  const rounds = parseInt(scriptArgs[1] || "25", 10);
  test("a recycle loop exits cleanly", async () => {
    await runRow(["--child", "recycle-exit", String(rounds)], 0, "recycle-exit");
  });
  test("an fd-reuse successor keeps its traffic through a stale burst", async () => {
    await runRow(["--child", "stale-burst"], 0, "stale-burst");
  });
  test("std.exit from a message handler exits cleanly", async () => {
    await runRow(["--child", "abrupt-exit"], 0, "abrupt-exit");
  });
  const started = Date.now();
  const watchdog = setInterval(() => {
    if (Date.now() - started < 120000) return;
    clearInterval(watchdog);
    std.err.puts("FAIL: test_net_udp_grave stalled: a child wedged the exit path\n");
    std.exit(2);
  }, 5000);
  try { await t.run(); } finally { clearInterval(watchdog); }
}
