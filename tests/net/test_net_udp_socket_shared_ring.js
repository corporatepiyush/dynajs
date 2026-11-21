// covers: dyna:net.UDPSocket
// flags: --std
// timeout: 90
// Every UDP socket on the io_uring reactor receives through one shared ring of 64 provided
// buffers. When a burst across many sockets empties the ring, a socket's multishot receive ends
// with ENOBUFS. The reactor used to re-arm it at most 64 times in a row, then hand -ENOBUFS to the
// socket's callback and disarm it; the UDP layer drops errors, so the socket went deaf for good
// with no error anywhere. ENOBUFS now always re-arms: the ring is empty only while completions
// that hold its buffers wait to be reaped, and the re-arm is submitted after the reap that
// recycles them, so every ENOBUFS is paired with progress and the re-arm cannot spin.
// Measured on the uring leg (Linux 7.1, 2-CPU container, 3 busy loops): with the give-up, 17-26
// of 128 sockets deaf in 3 of 3 runs, in runs of about 8 adjacent sockets. With it removed, 200k to
// 480k ENOBUFS re-arms per run, and a residual 1 of 128 deaf in a third of the runs that was not the
// ring: that socket had no completion and nothing queued, because a port-0 sender had been handed
// its port (see test_net_udp_socket_port_unique). With both fixed, 0 deaf in 12 of 12 runs.
// The probes wait until the flood's backlog has drained (no flood datagram for 300 ms), so a
// probe is never dropped by a still-full rcvbuf. Elsewhere (epoll, kqueue) this is a liveness
// check of the same scenario.
// Mutant (red on uring): the old give-up after 64 re-arms.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_udp_socket_shared_ring");
import * as os from "os";
import { UDPSocket } from "dyna:net";
import { args } from "dyna:sys";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const K = 128, FLOOD_MS = 3000;
const dir = makeTempDir("dyna-udp-ring-");
const flood = dir.join("flood.js");
writeFile(flood,
    "import { UDPSocket } from 'dyna:net';\n" +
    "const a = scriptArgs.slice(1).map(Number), ms = a.pop();\n" +
    "const tx = new UDPSocket({ port: 0, host: '127.0.0.1' });\n" +
    "const d = new Uint8Array(512), end = Date.now() + ms;\n" +
    "while (Date.now() < end) for (const p of a) { try { tx.send(d, '127.0.0.1', p); } catch (e) {} }\n" +
    "tx.close();\n");

await __kitS.section("the flood child started", async () => {
const rx = [], flooded = new Array(K).fill(0), probed = new Array(K).fill(0);
for (let k = 0; k < K; k++) {
    const s = new UDPSocket({ port: 0, host: "127.0.0.1" });
    s.start({ message: (d) => { if (d.length === 1) probed[k]++; else flooded[k]++; } });
    rx.push(s);
}
const pid = os.exec([args()[0], "--std", String(flood), ...rx.map((s) => String(s.port)), String(FLOOD_MS)], { block: false });
ok(pid > 0, "the flood child started");
let status = null;
const deadline = Date.now() + FLOOD_MS + 30000;
while (Date.now() < deadline) {
    const [p, st] = os.waitpid(pid, os.WNOHANG);
    if (p === pid) { status = st; break; }
    await sleep(50);
}
ok(status !== null, "the flood child exited");
const total = () => flooded.reduce((x, y) => x + y, 0);
for (let last = -1, quiet = Date.now() + 20000; Date.now() < quiet;) {
    const now = total();
    if (now === last) break;
    last = now;
    await sleep(300);
}
const tx = new UDPSocket({ port: 0, host: "127.0.0.1" });
for (let i = 0; i < 10; i++) {
    for (const s of rx) tx.send(new Uint8Array([7]), "127.0.0.1", s.port);
    await sleep(50);
}
for (let t = Date.now() + 5000; Date.now() < t && probed.some((p) => p === 0);) await sleep(50);
const deaf = probed.map((p, k) => (p === 0 ? k : -1)).filter((k) => k >= 0);
ok(flooded.some((n) => n > 0), "the flood reached the sockets (max " + Math.max(...flooded) + " each)");
ok(deaf.length === 0, "every socket still receives after the flood (" + deaf.length + " of " + K + " deaf: " + deaf.slice(0, 12).join(",") + ")");
for (const s of rx) s.close();
tx.close();
removeAll(dir);
});
__kitS.end();

print(`test_net_udp_socket_shared_ring: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_net_udp_socket_shared_ring failed");
