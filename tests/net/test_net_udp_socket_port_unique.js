// covers: dyna:net.UDPSocket.port
// timeout: 60
// A UDPSocket bound to port 0 must get a port no other socket holds. Every UDP socket used to set
// SO_REUSEADDR, and on Linux a UDP socket with SO_REUSEADDR does not count as holding its port
// when the kernel picks an ephemeral one for another SO_REUSEADDR socket on the same address. So a
// second port-0 socket could be handed a live socket's port, and the later bind took that port's
// unicast datagrams: the first socket went silently deaf. Found as 1 of 128 sockets deaf in about
// a third of test_net_udp_socket_shared_ring runs on the uring leg (the deaf socket had no CQE
// and nothing queued: the datagrams went to the flood sender bound later). Now SO_REUSEADDR is
// set only for an explicit port.
// Measured (Linux 7.1, uring leg): with SO_REUSEADDR on port 0, 1-4 of these 400 ports shared and
// 1-4 receivers deaf in 3 of 3 runs; without it, 0 in 3 of 3.
// Mutant (red on Linux): SO_REUSEADDR set for port 0 again. macOS/BSD never share a unicast UDP
// port on SO_REUSEADDR alone, so there this is a plain uniqueness check.
// The 400 receivers bind in the kit's before hook (never under --list) and
// close in after; each case checks one property of that set.
import { UDPSocket } from "dyna:net";
import { suite } from "../lib/kit.js";

const t = suite("test_net_udp_socket_port_unique");
let A = null;
const ok = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const N = 400;
const socks = [], got = new Array(N).fill(0), byPort = new Map();
let tx = null;
t.before(() => {
    for (let k = 0; k < N; k++) {
        const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
        s.start({ message: () => { got[k]++; } });
        socks.push(s);
    }
    for (const s of socks) byPort.set(s.port, (byPort.get(s.port) || 0) + 1);
});
t.after(() => {
    for (const s of socks) s.close();
    if (tx) tx.close();
});

test("port-0 sockets on one address get distinct ports", () => {
    const dup = [...byPort].filter(([, c]) => c > 1).map(([p]) => p);
    ok(dup.length === 0, `${N} port-0 sockets on one address get ${N} distinct ports (shared: ${dup.slice(0, 8).join(",")})`);
});

test("a later port-0 sender does not take a receiver's port", () => {
    tx = new UDPSocket({ port: 0, host: "127.0.0.1" });
    ok(!byPort.has(tx.port), `a later port-0 sender does not take a receiver's port (${tx.port})`);
});

test("every receiver hears a datagram sent to its port", async () => {
    for (let round = 0; round < 3 && got.some((g) => g === 0); round++) {
        socks.forEach((s, k) => { if (!got[k]) tx.send(new Uint8Array([1]), "127.0.0.1", s.port); });
        for (let t = Date.now() + 1000; Date.now() < t && got.some((g) => g === 0);) await sleep(10);
    }
    const deaf = got.map((g, k) => (g ? -1 : k)).filter((k) => k >= 0);
    ok(deaf.length === 0, `every receiver hears a datagram sent to its port (${deaf.length} deaf)`);
}, { dependsOn: ["a later port-0 sender does not take a receiver's port"] });

await t.run();
