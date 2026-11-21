// covers: dyna:net.TCPServer
// timeout: 30
// TCPConn.write coalesces the writes a handler makes while the reactor dispatches one event: on a
// connection whose owner has no drain handler they go to the reactor's per-fd hold (DYN_AIO_HOLD,
// the path the HTTP App and TLS writes already use) and leave in one send when the dispatch ends.
// Ten 20-byte writes per request were ten send syscalls; now they are one (macOS, 3000 round trips:
// 11 sends per round trip -> 2 counting the client's, 161 ms -> 90 ms). Held bytes are not yet taken
// by the kernel, so bufferedAmount reports them, and a close flushes them before the FIN.
// Measured (macOS, 12 interleaved reps, each started at host load < 12, identical-binary control row;
// median throughput ratio to base, control in brackets): 10 small writes per request 1.792 (0.902);
// 1 write per request 1.122 (0.926); TCP echo 64 B x depth 32 4.515 (1.056), 16 KiB x 32 1.350
// (0.939); 64 KiB x 8 0.844 (0.912, quartiles 0.666-1.162, the losing row: the hold copies a write up
// to 64 KiB, inside the control spread); HTTP App (does not use TCPConn) 0.904 (0.827).
// An owner WITH a drain handler keeps sending at once: drain fires exactly once per emptying after a
// write left bytes queued, and a held write would count as queued and fire a drain after every
// handler that wrote (test_net_tcp pins the exactly-once rule under real backpressure).
// Writes outside a dispatch (timers, promise jobs) are not held: nothing would flush them before the
// loop sleeps. Pinned here: (1) no drain handler: bufferedAmount after a small write inside the data
// handler equals the bytes written so far, and the peer receives every byte in order, including a
// later write from a timer and bytes written just before close; (2) a drain handler: bufferedAmount
// after the same write is 0 and no drain fires; (3) a write of 16 KiB or more is never copied into the
// hold: it flushes what is held and goes out directly (holding gains nothing for it, and copying 64 KiB
// cost the 64 KiB echo row), while 16 KiB - 1 is still held; 1 B, 16 KiB, 16 KiB - 1 and 100 KiB in one
// handler arrive intact and in order.
import { TCPServer } from "dyna:net";

import { suite } from "../lib/kit.js";

const t = suite("test_net_tcp_server_write_hold");
let A = null;
const check = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);

const PIECES = [1, 16384, 16383, 100 * 1024];
const BIG = PIECES.reduce((a, b) => a + b, 0);
const big = new Uint8Array(BIG);
for (let i = 0; i < BIG; i++) big[i] = (i * 7 + 3) & 0xff;

function run(withDrain, done) {
  let amounts = [], bigAmounts = [], drains = 0, got = [], gotBig = 0, bigOk = true, closed = false;
  const handlers = {
    data: (c, b) => {
      const s = String.fromCharCode(...b);
      if (s === "q") {
        c.write("abc");
        amounts.push(c.bufferedAmount);
        c.write("de");
        amounts.push(c.bufferedAmount);
        setTimeout(() => c.write("f"), 0);
      } else if (s === "big") {
        let off = 0;
        for (const len of PIECES) {
          c.write(big.subarray(off, off + len));
          off += len;
          bigAmounts.push(c.bufferedAmount);
        }
      } else if (s === "bye") {
        c.write("last");
        c.close();
      }
    },
  };
  if (withDrain) handlers.drain = () => { drains++; };
  const srv = new TCPServer({ port: 0 });
  srv.start(handlers);
  let phase = 0, text = "";
  const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
    connect: (c) => c.write("q"),
    data: (c, b) => {
      if (phase === 0) {
        text += String.fromCharCode(...b);
        if (text.length >= 6) { got.push(text); text = ""; phase = 1; c.write("big"); }
      } else if (phase === 1) {
        for (let i = 0; i < b.length; i++) if (b[i] !== big[gotBig + i]) bigOk = false;
        gotBig += b.length;
        if (gotBig >= BIG) { phase = 2; c.write("bye"); }
      } else {
        text += String.fromCharCode(...b);
      }
    },
    close: () => {
      if (closed) return;
      closed = true;
      got.push(text);
      srv.close();
      done({ amounts, bigAmounts, drains, got, gotBig, bigOk });
    },
  });
}

const label = (d) => d ? "with a drain handler" : "without a drain handler";
function verify(withDrain, r) {
  const L = label(withDrain);
  if (withDrain) {
    check(r.amounts[0] === 0 && r.amounts[1] === 0,
          L + ": small writes go out at once, bufferedAmount 0 after each (got " + r.amounts.join(",") + ")");
    check(r.drains === 0, L + ": no drain when the kernel took every byte (got " + r.drains + ")");
  } else {
    check(r.bigAmounts[0] === 1 && r.bigAmounts[1] === 0 && r.bigAmounts[2] === 16383,
          L + ": a 1-byte write is held, a 16384-byte write flushes it and goes out directly (not copied into"
          + " the hold), a 16383-byte write is held again; bufferedAmount 1,0,16383 (got " + r.bigAmounts.slice(0, 3).join(",") + ")");
    check(r.amounts[0] === 3 && r.amounts[1] === 5,
          L + ": writes inside the data handler are held until it returns, bufferedAmount 3 then 5 (got "
          + r.amounts.join(",") + ")");
  }
  check(r.got[0] === "abcdef", L + ": held bytes then the timer's byte arrive in order (got " + JSON.stringify(r.got[0]) + ")");
  check(r.gotBig === BIG && r.bigOk, L + ": 1 B, 16 KiB, 16 KiB - 1 and 100 KiB writes inside one handler arrive intact and in order (" + r.gotBig + " bytes)");
  check(r.got[1] === "last", L + ": bytes written just before close arrive before the FIN (got " + JSON.stringify(r.got[1]) + ")");
}

test("without a drain handler", async () => {
  const r0 = await new Promise((res) => run(false, res));
  verify(false, r0);
}, { timeoutMs: 10000 });

test("with a drain handler", async () => {
  const r1 = await new Promise((res) => run(true, res));
  verify(true, r1);
}, { timeoutMs: 10000 });

await t.run();
