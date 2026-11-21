// flags: --std
// timeout: 60
// covers: dyna:net.UDPSocket
// A failed sendto(2) used to throw a bare InternalError "UDPSocket: send failed" with no errno, so an
// EPIPE (seen by the WebRTC work on macOS when the Application Firewall's block-all-incoming mode drops
// LAN traffic), an ENOBUFS/EAGAIN (send buffer full) and an EMSGSIZE (datagram over the OS limit) all
// looked the same. The error now names strerror and the code, and carries .errno and .code. A failed
// send must not wedge the socket: the same socket still sends and receives afterwards.
// The errno is provoked deterministically with an oversized datagram: EMSGSIZE on every OS for 70000
// bytes (over the 65507-byte IPv4 UDP payload limit; macOS also caps at net.inet.udp.maxdgram, 9216).
// Mutant (red): the send path throws without reading errno (code/errno missing).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_udp_socket_send_errno");
import * as std from "std";
import { UDPSocket } from "dyna:net";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const watchdog = setInterval(() => {}, 1000);

async function main() {
  await __kitS.section("EMSGSIZE surfaces errno/code and keeps the socket usable", async () => {
    const r = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
    const got = [];
    let wake = null;
    r.start({ message: (d) => { got.push(d.length); if (wake) wake(); } });
    s.start({});
    for (const size of [70000, 65508, 200000]) {
      let err = null;
      try { s.send(new Uint8Array(size), "127.0.0.1", r.port); } catch (e) { err = e; }
      check(err !== null, size + ": oversized send throws");
      check(err instanceof InternalError, size + ": error type stays InternalError, got " + (err && err.name));
      check(err && err.code === "EMSGSIZE", size + ": code EMSGSIZE, got " + (err && err.code));
      check(err && typeof err.errno === "number" && err.errno > 0, size + ": numeric errno, got " + (err && err.errno));
      check(err && /^UDPSocket: send failed: .+ \(EMSGSIZE\)$/.test(err.message), size + ": message names strerror and code: " + (err && err.message));
      const ok = s.send(new Uint8Array([size & 255, 7, 7]), "127.0.0.1", r.port);
      check(ok === 3, size + ": a send after the failure returns 3, got " + ok);
    }
    const t0 = Date.now();
    while (got.length < 3 && Date.now() - t0 < 5000) await new Promise((res) => { wake = res; setTimeout(res, 50); });
    check(got.length === 3 && got.every((x) => x === 3), "all 3 follow-up datagrams arrived intact: " + JSON.stringify(got));
    let back = 0;
    s.close();
    const s2 = new UDPSocket({ port: 0, host: "127.0.0.1" });
    s2.start({ message: () => { back++; if (wake) wake(); } });
    r.send(new Uint8Array([1]), "127.0.0.1", s2.port);
    const t1 = Date.now();
    while (back < 1 && Date.now() - t1 < 5000) await new Promise((res) => { wake = res; setTimeout(res, 50); });
    check(back === 1, "the receiving socket still sends after the peer's failures");
    r.close();
    s2.close();
  });

  await __kitS.section("argument errors keep their own types", async () => {
    const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
    let e1 = null, e2 = null;
    try { s.send(new Uint8Array(1), "not-an-ip", 9); } catch (e) { e1 = e; }
    try { s.send(new Uint8Array(1), "127.0.0.1", 0); } catch (e) { e2 = e; }
    check(e1 instanceof TypeError && e1.code === undefined, "bad host is a TypeError without code");
    check(e2 instanceof RangeError && e2.code === undefined, "bad port is a RangeError without code");
    s.close();
  });

  clearInterval(watchdog);
  if (!__kitS.listing) print("udp-send-errno: checks=" + n + " fails=" + fails);
  __kitS.end();
  std.exit(fails ? 1 : 0);
}

await main().catch((e) => {
  clearInterval(watchdog);
  std.err.puts("FAIL: uncaught: " + (e && e.stack ? e.stack : e) + "\n");
  std.exit(1);
});
