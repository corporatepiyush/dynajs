// flags: --std
// timeout: 60
// covers: dyna:net.UDPSocket
// A UDP socket is drained at most a batch of datagrams per reactor wake-up. A handler that sends
// a datagram to its own socket for each one it receives keeps the receive queue non-empty forever;
// without the batch the drain never returned to the loop and a pending timer starved. The queue is
// pre-filled with 256: loopback delivery is not immediate, so one datagram in flight never starves
// anything. The handler stops feeding itself after CAP datagrams, so the starving build fails here
// instead of hanging the runner.
// The burst case is windowed: it sends W = 128 datagrams (two reactor batches of 64) in one
// synchronous run, then waits until all of them arrived before the next window. So every window
// needs a second wake-up for its tail with nothing new arriving to cause one, and no more than
// 128 two-byte datagrams are ever queued, under half the loopback rcvbuf. It used to send 128 per
// 1 ms tick unpaced; on a loaded host the receiver fell behind, the rcvbuf overran and the kernel
// dropped datagrams (1949/2000 on the uring leg), a loss of the test's own making. On Linux the
// failure message carries the UDP RcvbufErrors delta, so a kernel drop is told from a lost wake-up.
// Mutant (red): the read filter registered edge-triggered, so a window's tail past one batch
// never gets its wake-up.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_udp_socket_budget");
import * as std from "std";
import { UDPSocket } from "dyna:net";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const one = new Uint8Array([1]);

await __kitS.section("block L26", async () => {
  const CAP = 200000;
  const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
  let got = 0, fired = -1, feeding = true;
  const done = new Promise((resolve) => {
    s.start({
      message: () => {
        got++;
        if (got >= CAP) feeding = false;
        if (feeding) s.send(one, "127.0.0.1", s.port);
        else if (fired >= 0) resolve();
      },
    });
    setTimeout(() => {
      fired = got;
      feeding = false;
      setTimeout(resolve, 50);
    }, 10);
  });
  for (let i = 0; i < 256; i++) s.send(one, "127.0.0.1", s.port);
  await done;
  ok(fired >= 0 && fired < CAP, `a timer fires while a handler keeps its own socket readable (timer saw ${fired}, cap ${CAP})`);
  ok(got > 0, `the self-feeding handler ran (${got})`);
  s.close();
});

function udpRcvbufErrors() {
  const t = std.loadFile("/proc/net/snmp");
  if (!t) return null;
  const rows = t.split("\n").filter((l) => l.startsWith("Udp: "));
  if (rows.length < 2) return null;
  const h = rows[0].trim().split(/\s+/), v = rows[1].trim().split(/\s+/);
  const i = h.indexOf("RcvbufErrors");
  return i < 0 ? null : Number(v[i]);
}

await __kitS.section("block L62", async () => {
  const N = 2000, W = 128, STALL_MS = 5000;
  const rx = new UDPSocket({ port: 0, host: "127.0.0.1" });
  const tx = new UDPSocket({ port: 0, host: "127.0.0.1" });
  const seen = new Uint8Array(N);
  let got = 0, sent = 0, stalled = -1;
  rx.start({
    message: (d) => {
      const i = d[0] | (d[1] << 8);
      if (i < N && !seen[i]) { seen[i] = 1; got++; }
    },
  });
  const rb0 = udpRcvbufErrors();
  while (sent < N) {
    const end = Math.min(N, sent + W);
    for (; sent < end; sent++) tx.send(new Uint8Array([sent & 0xff, sent >> 8]), "127.0.0.1", rx.port);
    const until = Date.now() + STALL_MS;
    while (got < sent && Date.now() < until) await new Promise((r) => setTimeout(r, 1));
    if (got < sent) { stalled = sent; break; }
  }
  const rb = rb0 === null ? "" : `, UDP RcvbufErrors +${udpRcvbufErrors() - rb0}`;
  ok(got === N, `a burst larger than one batch is delivered whole across wake-ups (${got}/${N}, stalled at ${stalled}${rb})`);
  rx.close();
  tx.close();
});
__kitS.end();

print(`test_udp_budget: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
