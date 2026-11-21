// flags: --std
// solo: measures time, CPU or RSS; a parallel wave turns the numbers into noise
// covers: dyna:smtp.SmtpServer
// test_smtp_smtp_server_bench.js -- main-thread cost of SmtpServer per message, plaintext vs STARTTLS. The
// client is this file re-run as a child process (args()[0]), so only the server runs on this thread.
// S9 S2b, 2026-11-27. Per sweep point (N = 16, 256, 2048 messages; 1 KiB and 64 KiB bodies; onData gets the
// whole body): main-thread CPU us/msg (threadCpuNow delta / N: reactor, TLS records, hooks, JS), the module's
// own callback us p50/p99, pool handoff p50/p99, msg/s, peak RSS and RSS growth per message, then three
// repeated N=256 runs for a leak/plateau check. Assertions are sanity bounds; the numbers go to the d.ts.
import { threadCpuNow, memoryUsage, args, Spawn } from "dyna:sys";
import { SmtpClient, SmtpServer } from "dyna:smtp";
import * as std from "std";

const dec = new TextDecoder();
const BIN = args()[0], SELF = scriptArgs[0];

if (scriptArgs[1] === "--client") {
    const [, , port, n, size, tls, ca] = scriptArgs;
    const body = "Subject: bench\r\n\r\n" + ("b".repeat(78) + "\r\n").repeat(Math.max(1, Math.floor(+size / 80)));
    const c = new SmtpClient(tls === "starttls"
        ? { host: "127.0.0.1", port: +port, servername: "localhost", ca, maxConnections: 4, maxMessagesPerConnection: 0, mechanisms: ["PLAIN"] }
        : { host: "127.0.0.1", port: +port, tls: "none", maxConnections: 4, maxMessagesPerConnection: 0 });
    const ps = [];
    for (let i = 0; i < +n; i++) ps.push(c.send({ from: "a@b.test", to: "u@test.example", data: body }));
    const rs = await Promise.all(ps);
    await c.quit();
    c.close();
    console.log("CLIENT " + rs.filter((r) => r.code === 250).length);
} else {
    const { suite } = await import("../lib/kit.js");
    const { testCert, stopStarted } = await import("../lib/services.js");
    const t = suite("test_smtp_smtp_server_bench");
    const cert = testCert();
    t.after(async () => { await stopStarted(); });

    async function run(tls, n, size) {
        let got = 0, bytes = 0;
        const s = new SmtpServer(tls ? { port: 0, maxMessagesPerSession: 1000000, tls: "starttls", cert: cert.cert, key: cert.key, onData(sess, m) { got++; bytes += m.data.length; } }
            : { port: 0, maxMessagesPerSession: 1000000, tls: "none", onData(sess, m) { got++; bytes += m.data.length; } });
        s.listen();
        const rss0 = memoryUsage().peakRss;
        const c0 = threadCpuNow(), w0 = Date.now();
        const p = new Spawn(BIN, ["--std", SELF, "--client", String(s.port), String(n), String(size), tls ? "starttls" : "plain", tls ? cert.ca : ""], { timeoutMs: 120000 });
        let out = "";
        for await (const ch of p.stdout) out += dec.decode(ch);
        await p.wait();
        const cpu = threadCpuNow() - c0, wall = Date.now() - w0;
        const st = s.stats();
        s.close();
        const rss1 = memoryUsage().peakRss;
        const row = { tls: tls ? "starttls" : "plain", n, size, mainUsPerMsg: +(cpu * 1000 / n).toFixed(1), cbP50Us: st.mainP50Us, cbP99Us: st.mainP99Us,
            handoffP50Us: st.handoffP50Us, handoffP99Us: st.handoffP99Us, msgPerSec: Math.round(n * 1000 / Math.max(1, wall)),
            peakRssMiB: +(rss1 / 1048576).toFixed(1), rssGrowthBytesPerMsg: Math.round((rss1 - rss0) / n) };
        console.log("BENCH " + JSON.stringify(row));
        return { ok: got === n && out.includes("CLIENT " + n) && bytes > 0, row };
    }

    for (const tls of [false, true])
        t.test((tls ? "STARTTLS" : "plaintext") + " sweep N=16/256/2048, 1 KiB and 64 KiB", async (a) => {
            if (tls && !cert) return a.skip("openssl missing");
            for (const size of [1024, 65536])
                for (const n of [16, 256, 2048]) {
                    const { ok, row } = await run(tls, n, size);
                    a.ok(ok, "all delivered " + JSON.stringify(row));
                    a.ok(row.mainUsPerMsg < 5000, "sanity main us/msg " + row.mainUsPerMsg);
                }
        });

    t.test("plateau: three repeated N=256 1 KiB runs", async (a) => {
        const peaks = [];
        for (let i = 0; i < 3; i++) peaks.push((await run(false, 256, 1024)).row.peakRssMiB);
        console.log("PLATEAU " + JSON.stringify(peaks));
        a.ok(peaks[2] - peaks[1] < 8, "peak RSS plateaus: " + peaks);
    });

    await t.run();
}
