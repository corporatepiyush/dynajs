// flags: --std
// solo: measures time, CPU or RSS; a parallel wave turns the numbers into noise
// covers: integration dyna:dkim.sign dyna:dkim.verify -- every case signs and then verifies the same message
// test_dkim_bench.js -- main-thread cost and memory of sign/verify (S9 M3, 2026-11-27). Per sweep point
// (rsa-sha256 2048 and ed25519-sha256; bodies 1 KiB, 64 KiB, 1 MiB; N = 16, 256, 2048 concurrent calls, N <= 64
// at 1 MiB): main-thread CPU us/op (threadCpuNow delta / N: option parsing, the message copy, lookup calls,
// result objects; parse, canonicalization, hashing and crypto are on the pool), ops/s, per-call latency
// p50/p99 at concurrency 1 (pool handoff + work), peak RSS and RSS growth per op, then eight repeated N=256
// runs (std.gc() before each, so the high-water mark is not GC timing) for a leak/plateau check. Assertions are sanity bounds; the numbers go to the report.
import * as std from "std";
import { suite } from "../lib/kit.js";
import { sign, verify } from "dyna:dkim";
import { RSA, Ed25519Generate } from "dyna:crypto";
import { threadCpuNow, memoryUsage } from "dyna:sys";

const t = suite("test_dkim_bench");
const RK = RSA.generate(2048);
const ED = Ed25519Generate();
const KEYS = {
    "r._domainkey.example.com": "v=DKIM1; p=" + RK.publicKey.replace(/-----[^-]+-----|\s/g, ""),
    "e._domainkey.example.com": "v=DKIM1; k=ed25519; p=" + btoa(String.fromCharCode(...ED.publicKey)),
};
const lookup = (n) => KEYS[n] ?? null;
const ALG = { rsa: { selector: "r", privateKey: RK.privateKey }, ed25519: { selector: "e", privateKey: ED.privateKey } };
const body = (size) => "From: a@example.com\r\nTo: b@example.net\r\nSubject: bench\r\n\r\n" + ("b".repeat(78) + "\r\n").repeat(Math.max(1, Math.floor(size / 80)));
const pct = (xs, p) => { const s = xs.slice().sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
const rows = [];

async function point(alg, size, n) {
    const msg = body(size), o = { domain: "example.com", ...ALG[alg] };
    const rss0 = memoryUsage().peakRss;
    let c0 = threadCpuNow(), w0 = Date.now();
    const sigs = await Promise.all(Array.from({ length: n }, () => sign(msg, o)));
    const signCpu = threadCpuNow() - c0, signWall = Date.now() - w0;
    const signed = sigs.map((s) => s + msg);
    c0 = threadCpuNow(); w0 = Date.now();
    const vs = await Promise.all(signed.map((m) => verify(m, { lookup })));
    const verCpu = threadCpuNow() - c0, verWall = Date.now() - w0;
    const lat = [];
    for (let i = 0; i < Math.min(n, 64); i++) {
        const s0 = performance.now();
        await verify(signed[i], { lookup });
        lat.push((performance.now() - s0) * 1000);
    }
    const rss1 = memoryUsage().peakRss;
    const row = { alg, size, n, signMainUs: +(signCpu * 1000 / n).toFixed(1), verifyMainUs: +(verCpu * 1000 / n).toFixed(1),
        signOps: Math.round(n * 1000 / Math.max(1, signWall)), verifyOps: Math.round(n * 1000 / Math.max(1, verWall)),
        verifyLatP50Us: Math.round(pct(lat, 0.5)), verifyLatP99Us: Math.round(pct(lat, 0.99)),
        peakRssMiB: +(rss1 / 1048576).toFixed(1), rssGrowthBytesPerOp: Math.round((rss1 - rss0) / n) };
    console.log("BENCH " + JSON.stringify(row));
    rows.push(row);
    return vs.every((v) => v.length === 1 && v[0].status === "pass");
}

t.test("plateau: eight N=256 runs", async (a) => {
    const g = [];
    for (let i = 0; i < 8; i++) {
        std.gc();
        await point("ed25519", 65536, 256);
        g.push(rows[rows.length - 1].peakRssMiB);
    }
    console.log("PLATEAU peakRssMiB " + JSON.stringify(g));
    const steps = [g[5] - g[4], g[6] - g[5], g[7] - g[6]];
    a.ok(!steps.every((x) => x > 1), "peak RSS plateaus after warm-up (a leak grows >1 MiB on every one of the last 3 runs; one high-water step is not a leak): " + JSON.stringify(g));
    a.ok(g[7] - g[3] < 64, "peak RSS growth after warm-up stays under 64 MiB: " + JSON.stringify(g));
}, { timeoutMs: 120000 });

for (const alg of ["rsa", "ed25519"])
    t.test(alg + " sweep N=16/256/2048 x 1 KiB/64 KiB/1 MiB", async (a) => {
        for (const size of [1024, 65536, 1048576])
            for (const n of size > 65536 ? [16, 64] : [16, 256, 2048])
                a.ok(await point(alg, size, n), alg + " " + size + " " + n + " all pass");
        const small = rows.filter((r) => r.alg === alg && r.size === 1024 && r.n === 2048)[0];
        a.ok(small.verifyMainUs < 200 && small.signMainUs < 200, "main-thread cost stays small: " + JSON.stringify(small));
    }, { timeoutMs: 170000 });

await t.run();
