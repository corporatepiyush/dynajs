// flags: --std
// timeout: 60
// covers: dyna:net.UDPSocket
// UDPSocket over IPv6 (an additive extension; IPv4 behaviour is unchanged).
// - opts.host with a ':' binds an IPv6 socket; send() then takes IPv6 or IPv4 literals, an IPv4 one
//   going out v4-mapped because the socket is dual-stack (IPV6_V6ONLY off), so "::" also hears IPv4.
// - message(data, from): from.address is the numeric IPv6 form ("::1", "::ffff:127.0.0.1"), so it
//   can be passed straight back to send().
// - An IPv4 socket refuses an IPv6 host with a TypeError naming the fix; a malformed host is refused.
// Skips (exit 0) only when Linux reports no IPv6 on lo (/proc/net/if_inet6); elsewhere ::1 is
// assumed, so a build without IPv6 UDP fails here instead of skipping.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_udp_socket_ipv6");
import * as std from "std";
import { UDPSocket } from "dyna:net";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));
const throwsT = (f, re, m) => {
  try { f(); ok(false, m + " (no throw)"); }
  catch (e) { ok(e instanceof TypeError && re.test(String(e.message)), m + " -- got " + e); }
};
const dec = (u8) => String.fromCharCode(...u8);

function recvOne(sock, ms = 3000) {
  return new Promise((resolve) => {
    const t = setTimeout(() => resolve(null), ms);
    sock.start({ message: (data, from) => { clearTimeout(t); resolve({ text: dec(data), from }); } });
  });
}

const if6 = std.loadFile("/proc/net/if_inet6");
if (if6 !== null && !/\slo\s*$/m.test(if6)) { print("test_udp_ipv6: SKIP -- no IPv6 on lo"); std.exit(0); }
let s6;
try { s6 = new UDPSocket({ port: 0, host: "::1" }); }
catch (e) { print("FAIL: bind ::1 -- " + e); print("test_udp_ipv6: 0 passed, 1 failed"); std.exit(1); }

await __kitS.section("port 0 on ::1 resolves to a real port", async () => {
  ok(s6.port > 0 && s6.port < 65536, "port 0 on ::1 resolves to a real port");
  const a = new UDPSocket({ port: 0, host: "::1" });
  const got = recvOne(s6);
  eq(a.send("v6 hello", "::1", s6.port), 8, "send to ::1 returns the bytes sent");
  const r = await got;
  ok(r !== null, "::1 -> ::1 datagram arrives");
  if (r) {
    eq(r.text, "v6 hello", "payload intact");
    eq(r.from.address, "::1", "from.address is the IPv6 literal");
    eq(r.from.port, a.port, "from.port is the sender's port");
    const back = recvOne(a);
    s6.send("reply", r.from.address, r.from.port);
    const rb = await back;
    ok(rb !== null && rb.text === "reply", "from.address/from.port round-trip into send()");
  }
  a.close();
});

await __kitS.section("from v4", async () => {
  const any6 = new UDPSocket({ port: 0, host: "::" });
  const v4 = new UDPSocket({ port: 0, host: "127.0.0.1" });
  const got = recvOne(any6);
  v4.send("from v4", "127.0.0.1", any6.port);
  const r = await got;
  ok(r !== null, "a socket bound to :: hears an IPv4 sender (dual-stack)");
  if (r) {
    eq(r.from.address, "::ffff:127.0.0.1", "the IPv4 sender shows v4-mapped");
    eq(r.from.port, v4.port, "v4-mapped from.port");
    const back = recvOne(v4);
    any6.send("to v4", "127.0.0.1", v4.port);
    const rb = await back;
    ok(rb !== null && rb.text === "to v4", "an IPv6 socket sends to an IPv4 literal (v4-mapped)");
    if (rb) eq(rb.from.address, "127.0.0.1", "the IPv4 socket sees the plain IPv4 source");
  }
  any6.close();
  v4.close();
});

await __kitS.section("IPv4 socket refuses an IPv6 host", async () => {
  const v4 = new UDPSocket({ port: 0, host: "127.0.0.1" });
  throwsT(() => v4.send("x", "::1", 9), /IPv6 host needs a UDPSocket bound to an IPv6 host/, "IPv4 socket refuses an IPv6 host");
  throwsT(() => v4.send("x", "not-an-ip", 9), /IPv4 or IPv6 address/, "IPv4 socket refuses a hostname");
  throwsT(() => s6.send("x", "zz::1", 9), /IPv4 or IPv6 address/, "IPv6 socket refuses a malformed IPv6 host");
  throwsT(() => s6.send("x", "localhost", 9), /IPv4 or IPv6 address/, "IPv6 socket refuses a hostname");
  let threw = false;
  try { new UDPSocket({ port: 0, host: "::zz" }); } catch (e) { threw = true; }
  ok(threw, "binding a malformed IPv6 host throws");
  const got = recvOne(v4);
  const s4 = new UDPSocket({ port: 0 });
  s4.send("plain", "127.0.0.1", v4.port);
  const r = await got;
  ok(r !== null && r.text === "plain" && r.from.address === "127.0.0.1", "IPv4-only path unchanged");
  s4.close();
  v4.close();
});
__kitS.end();
s6.close();

print(`test_udp_ipv6: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
