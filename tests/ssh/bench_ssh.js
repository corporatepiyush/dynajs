// flags: --std
// dyna:ssh bench (rule 11: CPU and memory) against a private sshd on 127.0.0.1. Sections (SSH_BENCH_SECTIONS, default all;
// run "rss" and "plateau" each in a fresh process, since peak RSS is a high-water mark):
//   throughput: exec download (`head -c SIZE /dev/zero`) MB/s and client CPU per byte (ns/B, user+sys of this process) per
//               cipher, next to the OpenSSH client (`ssh ... > /dev/null` under /usr/bin/time -l) for the same transfer;
//   handshake:  connect (TCP + mlkem768x25519 KEX + ed25519 auth) and connect+exec("true"), median/p90, vs `ssh host true`;
//   rss:        per concurrent authenticated connection at 1..256: peak-RSS growth and current RSS (ps rss; the C engine allocates with libc, outside the JS heap counters);
//   plateau:    ROUNDS rounds of CYCLES connect/exec/close cycles: peak and current RSS, JS heap and object count per round.
// Env: SSH_BENCH_MB (256), SSH_BENCH_ROUNDS (10), SSH_BENCH_CYCLES (100), SSH_BENCH_CONNS ("1,16,64,256").
import { SSHClient } from "dyna:ssh";
import * as std from "std";
import * as sys from "dyna:sys";
import { HAVE_SSHD, startSshd, run } from "./ssh_util.js";

if (!HAVE_SSHD) { console.log("bench_ssh: no sshd; skipped"); std.exit(0); }
const MB = Number(std.getenv("SSH_BENCH_MB") || 256);
const ROUNDS = Number(std.getenv("SSH_BENCH_ROUNDS") || 10);
const CYCLES = Number(std.getenv("SSH_BENCH_CYCLES") || 100);
const CONNS = (std.getenv("SSH_BENCH_CONNS") || "1,16,64,256").split(",").map(Number);
const SECTIONS = new Set((std.getenv("SSH_BENCH_SECTIONS") || "throughput,handshake,rss,plateau").split(","));
const SIZE = MB << 20;
const cpu = () => { const u = sys.cpuUsage(); return u.user + u.system; };
const rss = () => sys.rusage().maxrss;
const live = () => Number(sys.Exec("ps", ["-o", "rss=", "-p", String(sys.pid())]).stdout.trim()) * 1024;
const now = () => performance.now();
const pct = (a, p) => { const s = a.slice().sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
const f1 = (x) => x.toFixed(1);
const MiB = (x) => f1(x / 1048576);

function opensshTime(d, cipher, cmd) {
    const args = ["-l", "ssh", "-F", "/dev/null", "-o", "BatchMode=yes", "-o", "UserKnownHostsFile=" + d.known, "-o", "StrictHostKeyChecking=yes",
        "-c", cipher, "-i", d.dir + "/user", "-p", String(d.port), "127.0.0.1", cmd];
    const r = sys.Exec("/bin/sh", ["-c", "/usr/bin/time " + args.map((x) => "'" + x + "'").join(" ") + " > /dev/null"]);
    const m = /([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys/.exec(r.stderr);
    const rs = /(\d+)\s+maximum resident set size/.exec(r.stderr);
    if (r.code !== 0 || !m) throw new Error("openssh client failed: " + r.stderr.slice(0, 400));
    return { real: +m[1], cpu: +m[2] + +m[3], maxrss: rs ? +rs[1] : 0 };
}

async function download(c) {
    const x = await c.exec("head -c " + SIZE + " /dev/zero");
    const rd = x.stdout.getReader();
    let n = 0;
    for (;;) { const { value, done } = await rd.read(); if (done) break; n += value.byteLength; }
    await x.exit;
    if (n !== SIZE) throw new Error("short transfer " + n);
}

console.log("bench_ssh: sections " + [...SECTIONS].join(",") + "; host " + sys.cpuInfo().model + "; load " + sys.loadAvg().map((x) => x.toFixed(2)).join(" ") + "; baseline peakRSS " + MiB(rss()) + " MiB, current RSS " + MiB(live()) + " MiB");
const servers = [];
let main = null;
const server = async () => { if (!main) { main = await startSshd({ tag: "bench", logLevel: "INFO" }); servers.push(main); } return main; };
try {
    if (SECTIONS.has("throughput")) {
        console.log("\n== exec download throughput, " + MB + " MiB, best of 3 (client side) ==");
        console.log("cipher                          client   MB/s     CPU s   ns/B   peakRSS MB");
        for (const cipher of ["chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com"]) {
            const d = await startSshd({ tag: "bench", ciphers: cipher, logLevel: "INFO", extra: ["RekeyLimit 4G"] });
            servers.push(d);
            const c = await SSHClient.connect({ ...d.base, dyna: { rekeyBytes: 4294967296 } });
            await run(c, "true");
            let best = null;
            for (let rep = 0; rep < 3; rep++) {
                const c0 = cpu(), t0 = now();
                await download(c);
                const dt = (now() - t0) / 1000, dc = cpu() - c0;
                if (!best || dt < best.dt) best = { dt, dc };
            }
            c.close();
            console.log(cipher.padEnd(32) + "dynajs  " + f1(MB / best.dt).padStart(7) + "  " + best.dc.toFixed(3).padStart(8) + "  " + (best.dc * 1e9 / SIZE).toFixed(2).padStart(5) + "  " + MiB(rss()).padStart(8));
            let ob = null;
            for (let rep = 0; rep < 3; rep++) {
                const o = opensshTime(d, cipher, "head -c " + SIZE + " /dev/zero");
                if (!ob || o.real < ob.real) ob = o;
            }
            console.log(cipher.padEnd(32) + "openssh " + f1(MB / ob.real).padStart(7) + "  " + ob.cpu.toFixed(3).padStart(8) + "  " + (ob.cpu * 1e9 / SIZE).toFixed(2).padStart(5) + "  " + MiB(ob.maxrss).padStart(8));
        }
    }

    if (SECTIONS.has("handshake")) {
        const d = await server();
        console.log("\n== handshake latency (mlkem768x25519-sha256, ed25519, chacha20-poly1305), 40 runs ==");
        const hs = [], hx = [];
        for (let i = 0; i < 40; i++) {
            let t0 = now();
            const c = await SSHClient.connect(d.base);
            hs.push(now() - t0);
            t0 = now();
            await run(c, "true");
            hx.push(now() - t0);
            c.close();
        }
        const oh = [];
        for (let i = 0; i < 15; i++) oh.push(opensshTime(d, "chacha20-poly1305@openssh.com", "true").real * 1000);
        console.log("dynajs connect           median " + f1(pct(hs, 0.5)) + " ms  p90 " + f1(pct(hs, 0.9)) + " ms");
        console.log("dynajs connect+exec true median " + f1(pct(hs, 0.5) + pct(hx, 0.5)) + " ms  (exec alone " + f1(pct(hx, 0.5)) + " ms)");
        console.log("openssh `ssh host true`  median " + f1(pct(oh, 0.5)) + " ms  p90 " + f1(pct(oh, 0.9)) + " ms  (includes process start; /usr/bin/time resolution 10 ms)");
    }

    if (SECTIONS.has("rss")) {
        const d = await server();
        const w = await SSHClient.connect(d.base);
        await run(w, "true");
        w.close();
        std.gc();
        const r0 = rss(), l0 = live();
        console.log("\n== memory per concurrent authenticated connection (one exec done on each, connections held open) ==");
        console.log("conns   peakRSS MB   +peakRSS B/conn   curRSS MB   +curRSS B/conn");
        for (const n of CONNS) {
            const cs = [];
            for (let i = 0; i < n; i += 16) {
                const batch = [];
                for (let j = i; j < Math.min(n, i + 16); j++) batch.push(SSHClient.connect(d.base));
                cs.push(...await Promise.all(batch));
            }
            await Promise.all(cs.map((c) => run(c, "true")));
            std.gc();
            const r = rss(), l = live();
            console.log(String(n).padStart(5) + "   " + MiB(r).padStart(9) + "   " + Math.round((r - r0) / n).toString().padStart(15) + "   " + MiB(l).padStart(9) + "   " + Math.round((l - l0) / n).toString().padStart(14));
            for (const c of cs) c.close();
            std.gc();
        }
        console.log("after closing all: current RSS " + MiB(live()) + " MiB (baseline " + MiB(l0) + ")");
    }

    if (SECTIONS.has("plateau")) {
        const d = await server();
        console.log("\n== plateau: " + ROUNDS + " rounds x " + CYCLES + " connect/exec/close cycles ==");
        console.log("round   peakRSS MB   curRSS KB   JS heap KB   objects");
        for (let r = 0; r < ROUNDS; r++) {
            for (let i = 0; i < CYCLES; i++) {
                const c = await SSHClient.connect(d.base);
                await run(c, "echo x");
                c.close();
            }
            std.gc();
            const m = sys.memoryUsage();
            console.log(String(r + 1).padStart(5) + "   " + MiB(rss()).padStart(9) + "   " + Math.round(live() / 1024).toString().padStart(9) + "   " + Math.round(m.memoryUsedSize / 1024).toString().padStart(10) + "   " + m.objCount);
        }
    }
} finally {
    for (const d of servers) d.close();
}
