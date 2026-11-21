// flags: --std
// solo: measures time, CPU or RSS; a parallel wave turns the numbers into noise
// covers: dyna:smtp.SmtpClient
// test_smtp_smtp_client_bench.js -- main-thread cost of dyna:smtp per message, plaintext vs STARTTLS, against
// exim on a private port (separate process, so the server never runs on this thread). S9 S1b, 2026-11-27.
// Owner rule: mail parse/encode/crypto runs on the pool; TLS records still run on the main thread until
// P-OFFMAIN, so both are reported. Per sweep point (N = 16, 256, 2048 messages; 1 KiB and 64 KiB bodies):
// main-thread CPU us/msg (dyna:sys threadCpuNow delta / N, everything on this thread: reactor, TLS, JS),
// the module's own callback us p50/p99, pool handoff p50/p99, peak RSS and RSS growth per message, plus
// three repeated N=256 runs for a leak/plateau check. Assertions are sanity bounds only; the numbers
// are the report (the d.ts records them).
import { suite } from "../lib/kit.js";
import { threadCpuNow, memoryUsage } from "dyna:sys";
import { SmtpClient } from "dyna:smtp";
import { ensureExim, stopStarted } from "../lib/services.js";

const t = suite("test_smtp_smtp_client_bench");
const PORT = 25271;
let ex = null;
t.before(async () => { ex = await ensureExim(PORT); });
t.after(async () => { await stopStarted(); });

async function run(tls, n, size) {
    const body = "Subject: bench\r\n\r\n" + ("b".repeat(78) + "\r\n").repeat(Math.max(1, Math.floor(size / 80)));
    const c = new SmtpClient(tls
        ? { host: "127.0.0.1", port: PORT, servername: "localhost", ca: ex.ca, user: "tim", pass: "pw", maxConnections: 4, maxMessagesPerConnection: 0 }
        : { host: "127.0.0.1", port: PORT, tls: "none", maxConnections: 4, maxMessagesPerConnection: 0 });
    const rss0 = memoryUsage().peakRss;
    const c0 = threadCpuNow(), w0 = Date.now();
    const ps = [];
    for (let i = 0; i < n; i++) ps.push(c.send({ from: "a@b.test", to: "u@test.example", data: body }));
    const rs = await Promise.all(ps);
    const cpu = threadCpuNow() - c0, wall = Date.now() - w0;
    const s = c.stats();
    await c.quit();
    const rss1 = memoryUsage().peakRss;
    const row = { tls: tls ? "starttls" : "plain", n, size, mainUsPerMsg: +(cpu * 1000 / n).toFixed(1), cbP50Us: s.mainP50Us, cbP99Us: s.mainP99Us,
        handoffP50Us: s.handoffP50Us, handoffP99Us: s.handoffP99Us, msgPerSec: Math.round(n * 1000 / Math.max(1, wall)),
        peakRssMiB: +(rss1 / 1048576).toFixed(1), rssGrowthBytesPerMsg: Math.round((rss1 - rss0) / n) };
    console.log("BENCH " + JSON.stringify(row));
    return { ok: rs.every((r) => r.code === 250), row };
}

t.test("sweep: plaintext and STARTTLS, 1 KiB and 64 KiB, N = 16..2048", async (a) => {
    if (!ex || !ex.ok) a.skip("exim: " + (ex ? ex.why : "not started"));
    for (const tls of [false, true])
        for (const size of [1024, 65536])
            for (const n of [16, 256, size > 4096 ? 512 : 2048]) {
                const { ok, row } = await run(tls, n, size);
                a.ok(ok, "every message accepted " + JSON.stringify(row));
                a.ok(row.mainUsPerMsg > 0 && row.mainUsPerMsg < 20000, "main-thread us/msg in a sane range");
            }
});

t.test("plateau: three repeated N=256 runs", async (a) => {
    if (!ex || !ex.ok) a.skip("exim: " + (ex ? ex.why : "not started"));
    const peaks = [];
    for (let i = 0; i < 3; i++) peaks.push((await run(true, 256, 1024)).row.peakRssMiB);
    console.log("BENCH plateau peakRssMiB " + JSON.stringify(peaks));
    a.ok(peaks[2] - peaks[1] < 8, "no growth between the 2nd and 3rd run: " + peaks);
});

await t.run();
