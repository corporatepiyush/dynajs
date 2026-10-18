// flags: --std
// timeout: 120
import * as std from "std";
import { memoryUsage } from "dyna:sys";
import { UDPSocket } from "dyna:net";

function nativeSize() { return memoryUsage().nativeSize; }

function bytes(s) {
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
  return a;
}

let n = 0, fails = 0;
function check(c, m) {
  n++;
  if (!c) { std.err.puts("FAIL: " + m + "\n"); fails++; }
}

const WATCH_MS = parseInt(scriptArgs[1] || "20000", 10);
let progressAt = Date.now();
let phase = "start";
function progress(p) { phase = p; progressAt = Date.now(); }
const watchdog = setInterval(() => {
  const idle = Date.now() - progressAt;
  if (idle < WATCH_MS) return;
  clearInterval(watchdog);
  std.err.puts("FAIL: test_net_udp_selfclose stalled: no progress for " + idle +
               "ms in scenario: " + phase + " -- a datagram path wedged " +
               "instead of completing; failing bounded instead of hanging\n");
  std.err.flush();
  std.exit(2);
}, Math.max(250, WATCH_MS >> 2));

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

let S;

const RECYCLE_ROUNDS = 25;
function recycleRound() {
  return new Promise((done) => {
    const Sx = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const Rx = new UDPSocket({ port: 0, host: "127.0.0.1" });
    let closed = 0, finished = 0;
    Rx.start({ message: () => {
      if (closed) return;
      closed = 1;
      Rx.close();
      Sx.close();
      setTimeout(() => { if (!finished) { finished = 1; done(); } }, 30);
    } });
    Sx.send(bytes("x"), "127.0.0.1", Rx.port);
  });
}

function ledgerSeesShells() {
  const before = nativeSize();
  const t = new UDPSocket({ port: 0, host: "127.0.0.1" });
  const open = nativeSize();
  t.close();
  const afterClose = nativeSize();
  return { before, open, afterClose };
}

async function reactorRecycle() {
  const s = ledgerSeesShells();
  check(s.open > s.before, "S0: the native ledger counts a live socket shell");
  check(s.afterClose < s.open, "S0: ...and returns the shell's bytes when the socket closes");
  await recycleRound();
  std.gc();
  await sleep(10);
  const base = nativeSize();
  for (let round = 0; round < RECYCLE_ROUNDS; round++)
    await recycleRound();
  await sleep(30);
  const delta = nativeSize() - base;
  check(delta === 0, "S0: reactor recycle leaked " + delta + " native bytes over " +
        RECYCLE_ROUNDS + " rounds -- a shell parked after the reactor was freed " +
        "is never flushed (the graveyard flush hook is not re-armed)");
}

async function main() {

  progress("S0: reactor recycle re-arms the graveyard flush");
  await reactorRecycle();

  S = new UDPSocket({ port: 0, host: "127.0.0.1" });

  progress("S1: send-then-close-in-callback");
  for (let round = 0; round < 25; round++) {
    let got = 0, closed = 0, r2got = 0, R2 = null;
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const rp = R.port;
    R.start({ message: (data, from) => {
      got++;
      if (closed === 0) {
        closed = 1;
        R.send(bytes("pong"), from.address, from.port);
        R.close();
        R2 = new UDPSocket({ port: 0, host: "127.0.0.1" });
        R2.start({ message: () => { r2got++; } });
      }
    } });
    for (let k = 0; k < 8; k++)
      S.send(bytes("burst" + k), "127.0.0.1", rp);
    await sleep(15);
    check(closed === 1, "S1." + round + ": handler ran and self-closed");
    check(got <= 8, "S1." + round + ": no delivery past close (got " + got + ")");
    if (R2) {
      S.send(bytes("hello-r2"), "127.0.0.1", R2.port);
      await sleep(10);
      check(r2got === 1, "S1." + round + ": fd-reuse successor receives (got " + r2got + ")");
      R2.close();
    }
  }

  progress("S2: recv-parked-then-close");
  for (let round = 0; round < 50; round++) {
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    R.start({ message: () => { fails++; } });
    R.close();
    R.close();
  }
  await sleep(10);
  check(true, "S2: parked receive closed cleanly x50");

  progress("S3: close-then-late-completion");
  for (let round = 0; round < 25; round++) {
    let got = 0;
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const rp = R.port;
    R.start({ message: () => { got++; } });
    for (let k = 0; k < 8; k++)
      S.send(bytes("late" + k), "127.0.0.1", rp);
    setTimeout(() => { R.close(); }, 0);
    await sleep(15);
    check(got <= 8, "S3." + round + ": late completions bounded (got " + got + ")");
  }

  progress("S4: bystander close from a foreign handler");
  for (let round = 0; round < 25; round++) {
    let bgot = 0, agot = 0;
    const A = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const B = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const bp = B.port;
    B.start({ message: () => { bgot++; } });
    A.start({ message: () => {
      agot++;
      B.close();
      const C = new UDPSocket({ port: 0, host: "127.0.0.1" });
      C.start({ message: () => {} });
      setTimeout(() => C.close(), 10);
    } });
    for (let k = 0; k < 8; k++)
      S.send(bytes("bystander" + k), "127.0.0.1", bp);
    S.send(bytes("kick"), "127.0.0.1", A.port);
    await sleep(15);
    check(agot >= 1, "S4." + round + ": killer handler ran");
    check(bgot <= 8, "S4." + round + ": bystander completions bounded (got " + bgot + ")");
    A.close();
  }

  progress("S5: coercion-close in send()");
  for (let round = 0; round < 50; round++) {
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    let threw = null;
    try {
      R.send({ toString() { R.close(); return "payload"; } }, "127.0.0.1", 9);
    } catch (e) { threw = e; }
    check(threw !== null, "S5." + round + ": send() after coercion-close throws (got " +
          (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    R.close();
  }

  progress("S6: getter-close in start()");
  for (let round = 0; round < 50; round++) {
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    let threw = null;
    try {
      R.start({ get message() { R.close(); return () => {}; } });
    } catch (e) { threw = e; }
    check(threw !== null, "S6." + round + ": start() after getter-close throws (got " +
          (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    R.close();
  }

  progress("S7: GC-drop with parked receive");
  for (let round = 0; round < 25; round++) {
    {
      const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
      R.start({ message: () => { fails++; } });
    }
    if ((round & 7) === 7) std.gc();
  }
  std.gc();
  await sleep(10);
  check(true, "S7: GC-dropped sockets dispose cleanly x25");

  clearInterval(watchdog);
  S.close();
  print("udp-selfclose: checks=" + n + " fails=" + fails);
  std.exit(fails ? 1 : 0);
}

main().catch(e => {
  clearInterval(watchdog);
  std.err.puts("FAIL: uncaught: " + (e && e.stack ? e.stack : e) + "\n");
  std.exit(1);
});
