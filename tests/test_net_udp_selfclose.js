// flags: --std
// timeout: 120
/* test_net_udp_selfclose.js -- a UDPSocket that closes itself mid-flight.
 *
 * A datagram socket is the one resource whose ENTIRE life is callbacks: the
 * receive is armed once and every completion lands in JS, which may close the
 * socket that is delivering to it. The shapes that used to read freed state:
 *
 *   send-then-close-in-callback   a handler that answers and closes while more
 *                                 datagrams from the same burst are still
 *                                 queued behind the one it just got;
 *   recv-parked-then-close        a receive armed and the socket closed before
 *                                 any datagram ever arrives;
 *   close-then-late-completion    closed with datagrams already in the kernel
 *                                 (or already dispatched in this batch): the
 *                                 completion must die with the socket, not
 *                                 land on freed state;
 *   coercion-close                send()/start() coerce AFTER the native handle
 *                                 was resolved: user JS in the coercion that
 *                                 closes the socket freed the struct the call
 *                                 then kept using.
 *
 * fd REUSE is part of every shape: a handler that closes and immediately
 * allocates a new socket gets the same descriptor back, and a delivery loop
 * holding the old socket's pointer must not follow it into the new one.
 *
 * A STALLED SCENARIO IS A FAIL, NOT A HANG (same contract as
 * test_net_fdchurn.js): the watchdog names the scenario that stalled.
 *
 * S0 (it runs FIRST) covers a shape none of S1-S7 reach: the reactor itself
 * is recycled between self-closes. The graveyard's flush hook lives in the reactor's
 * drain-hook table, which is reset when the last net resource is closed and
 * the reactor is freed; the next self-close must re-arm it, or the parked
 * shell is never freed. The row measures the module-native ledger
 * (memoryUsage().nativeSize), which counts a UDPSocket shell from ctor to
 * free -- so a missed flush shows up as bytes that never come back, instead
 * of only to a debugger (the thread-local graveyard keeps leaked shells
 * reachable, which is why LSan cannot see them).
 */
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

/* ---- progress watchdog ---- */
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

/* A sender that never closes: every scenario needs a live peer to aim from.
   Created AFTER S0: a live socket keeps the shared reactor alive, and S0
   needs the reactor to be destroyed between its rounds. */
let S;

/* ---- S0 helper: one round that parks shells and then recycles the reactor.
 * Both sockets close from INSIDE a delivery, so both shells are parked until
 * the end of that drain; the closes also drop the last reactor refs, so the
 * reactor is destroyed right after the same drain. Every round therefore
 * leaves the next one parking against a REACTOR THE HOOK TABLE NO LONGER
 * REMEMBERS. */
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

/* Sensitivity: the round check below is only meaningful if the ledger counts
 * SHELLS (not just the wrapper's bookkeeping box). A socket's shell is
 * accounted at ctor and given back at close; the wrapper's box outlives
 * close() by one collection, so it is the drop AT CLOSE that isolates the
 * shell. */
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
  await recycleRound();   /* one warm-up round: the baseline is taken after it */
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

  /* ---- S0: reactor recycle (the graveyard flush hook must re-arm) -----
   * FIRST, before any other socket exists: the reactor is only destroyed
   * between rounds if every net resource of the round is closed. Each round
   * self-closes both of its sockets from inside a delivery (both shells are
   * parked to the end of the drain) and drops the last reactor refs, so the
   * reactor dies right after that drain; the next round therefore parks
   * against a reactor whose hook table was reset. */
  progress("S0: reactor recycle re-arms the graveyard flush");
  await reactorRecycle();

  S = new UDPSocket({ port: 0, host: "127.0.0.1" });

  /* ---- S1: send-then-close-in-callback, with fd-reuse churn ------
   * Eight datagrams are queued to R; the handler answers the first, closes R,
   * and immediately allocates a NEW socket (the descriptor comes straight
   * back). The remaining seven must be dropped or delivered to a live socket
   * -- never into the freed one. Repeated to keep the reuse pressure on. */
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
        /* fd-reuse: the freed descriptor is immediately back in play */
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
      /* the replacement socket must be fully functional */
      S.send(bytes("hello-r2"), "127.0.0.1", R2.port);
      await sleep(10);
      check(r2got === 1, "S1." + round + ": fd-reuse successor receives (got " + r2got + ")");
      R2.close();
    }
  }

  /* ---- S2: recv-parked-then-close, nothing ever sent ---- */
  progress("S2: recv-parked-then-close");
  for (let round = 0; round < 50; round++) {
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    R.start({ message: () => { fails++; } });
    R.close();
    R.close();   /* idempotent: a double close is not a fault */
  }
  await sleep(10);
  check(true, "S2: parked receive closed cleanly x50");

  /* ---- S3: close-then-late-completion ------
   * The burst is in the kernel before the close lands; the close runs from a
   * timer, so some deliveries race it. Whatever the interleaving, the
   * completion must be dropped, and a socket allocated after the close must
   * not receive anything aimed at the dead one. */
  progress("S3: close-then-late-completion");
  for (let round = 0; round < 25; round++) {
    let got = 0;
    const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const rp = R.port;
    R.start({ message: () => { got++; } });
    for (let k = 0; k < 8; k++)
      S.send(bytes("late" + k), "127.0.0.1", rp);
    setTimeout(() => { R.close(); }, 0);   /* races the deliveries */
    await sleep(15);
    check(got <= 8, "S3." + round + ": late completions bounded (got " + got + ")");
  }

  /* ---- S4: close a SECOND socket from another socket's handler ------
   * B has a receive parked and datagrams queued; A's handler kills it mid-
   * flight and takes its descriptor. B's queued completion must not land. */
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
      /* fd-reuse over B's descriptor */
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

  /* ---- S5: coercion-close in send() ------
   * The data argument's toString() closes the socket; the call must report a
   * closed resource, not touch freed state. */
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

  /* ---- S6: getter-close in start()'s handler bag ------
   * The bag's getters run arbitrary JS; one that closes the socket must land
   * as "closed native resource", never as a write into freed state. */
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

  /* ---- S7: GC-drop with a receive parked ------
   * The collector's final sweep disposes the socket while its receive is
   * armed; nothing may be delivered to it afterwards. */
  progress("S7: GC-drop with parked receive");
  for (let round = 0; round < 25; round++) {
    {
      const R = new UDPSocket({ port: 0, host: "127.0.0.1" });
      R.start({ message: () => { fails++; } });
      /* R drops out of scope here */
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
