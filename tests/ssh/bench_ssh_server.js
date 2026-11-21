// flags: --std
// dyna:ssh SSHServer bench (rule 11: CPU and memory). The server runs in this process; the clients are OpenSSH ssh children, so the CPU
// and RSS measured here are the server's alone. Sections (SSH_BENCH_SECTIONS, default all; run "rss" and "plateau" each in a fresh
// process, since peak RSS is a high-water mark):
//   throughput: one session writes SIZE bytes (1 MiB writes, each awaited) to `ssh ... > /dev/null`, and one reads SIZE bytes of stdin
//               fed by `head -c SIZE /dev/zero | ssh ...`;
//               MB/s and server CPU per byte (ns/B, user+sys of this process) per cipher;
//   rss:        per concurrent authenticated connection with one open session at 1..64: peak-RSS growth and current RSS growth;
//   plateau:    ROUNDS rounds of CYCLES connect/exec/close cycles: peak and current RSS, JS heap and object count per round.
// Env: SSH_BENCH_MB (256), SSH_BENCH_ROUNDS (8), SSH_BENCH_CYCLES (40), SSH_BENCH_CONNS ("1,16,64").
import { SSHServer } from "dyna:ssh";
import * as std from "std";
import * as sys from "dyna:sys";
import { HAVE_SSH, tmpDir, keygen, write, pubBlob, sshCli, sleep, collect } from "./ssh_util.js";

if (!HAVE_SSH) { console.log("bench_ssh_server: no OpenSSH >= 9.9 client; skipped"); std.exit(0); }
const MB = Number(std.getenv("SSH_BENCH_MB") || 256);
const ROUNDS = Number(std.getenv("SSH_BENCH_ROUNDS") || 8);
const CYCLES = Number(std.getenv("SSH_BENCH_CYCLES") || 40);
const CONNS = (std.getenv("SSH_BENCH_CONNS") || "1,16,64").split(",").map(Number);
const SECTIONS = new Set((std.getenv("SSH_BENCH_SECTIONS") || "throughput,rss,plateau").split(","));
const SIZE = MB << 20;
const cpu = () => { const u = sys.cpuUsage(); return u.user + u.system; };
const rss = () => sys.rusage().maxrss;
const live = () => Number(sys.Exec("ps", ["-o", "rss=", "-p", String(sys.pid())]).stdout.trim()) * 1024;
const now = () => performance.now();
const f1 = (x) => x.toFixed(1);
const MiB = (x) => f1(x / 1048576);

const dir = tmpDir("srvbench");
const host = keygen(dir + "/host");
const user = keygen(dir + "/user");
const chunk = new Uint8Array(1 << 20);
let held = [];

async function onSession(s) {
    const cmd = s.command || "";
    if (cmd === "down") { for (let n = 0; n < SIZE; n += chunk.length) await s.write(chunk); return; }
    if (cmd === "up") {
        const r = s.stdin.getReader();
        let n = 0;
        for (;;) { const { value, done } = await r.read(); if (done) break; n += value.byteLength; }
        await s.write(String(n) + "\n");
        return;
    }
    if (cmd === "hold") { held.push(s); await s.write("held\n"); return new Promise(() => {}); }
    await s.write("x\n");
}

const srv = SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession, dyna: { maxConnections: 1024, rekeyBytes: 4294967296 } });
const kh = dir + "/kh";
write(kh, "[127.0.0.1]:" + srv.port + " " + pubBlob(host.pub) + "\n");
const args = (cmd, cipher) => ["-F", "/dev/null", "-p", String(srv.port), "-i", dir + "/user", "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
    "-o", "UserKnownHostsFile=" + kh, "-o", "StrictHostKeyChecking=yes", "-o", "LogLevel=ERROR", "-o", "RekeyLimit=4G",
    "-c", cipher || "chacha20-poly1305@openssh.com", "bench@127.0.0.1", cmd];

async function sshShell(pre, cmd, cipher, post) {
    const p = new sys.Spawn("/bin/sh", ["-c", pre + "ssh " + args(cmd, cipher).map((x) => "'" + x + "'").join(" ") + post]);
    const out = new TextDecoder().decode(await collect(p.stdout));
    const r = await p.wait();
    p.close();
    if (r.code !== 0) throw new Error("ssh failed: " + r.code);
    return out;
}

console.log("bench_ssh_server: sections " + [...SECTIONS].join(",") + "; host " + sys.cpuInfo().model + "; load " + sys.loadAvg().map((x) => x.toFixed(2)).join(" ") + "; baseline peakRSS " + MiB(rss()) + " MiB, current RSS " + MiB(live()) + " MiB");
try {
    if (SECTIONS.has("throughput")) {
        console.log("\n== server throughput, " + MB + " MiB, best of 3 (server CPU = this process) ==");
        console.log("cipher                          dir    MB/s     CPU s   ns/B   peakRSS MB");
        for (const cipher of ["chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com"]) {
            for (const dirn of ["down", "up"]) {
                let best = null;
                for (let rep = 0; rep < 3; rep++) {
                    const c0 = cpu(), t0 = now();
                    if (dirn === "down") await sshShell("exec ", "down", cipher, " > /dev/null");
                    else {
                        const got = await sshShell("head -c " + SIZE + " /dev/zero | ", "up", cipher, "");
                        if (got.trim() !== String(SIZE)) throw new Error("short upload " + got);
                    }
                    const dt = (now() - t0) / 1000, dc = cpu() - c0;
                    if (!best || dt < best.dt) best = { dt, dc };
                }
                console.log(cipher.padEnd(32) + dirn.padEnd(5) + f1(MB / best.dt).padStart(7) + "  " + best.dc.toFixed(3).padStart(8) + "  " + (best.dc * 1e9 / SIZE).toFixed(2).padStart(5) + "  " + MiB(rss()).padStart(8));
            }
        }
    }

    if (SECTIONS.has("rss")) {
        await sshCli(args("x"));
        std.gc();
        const r0 = rss(), l0 = live();
        console.log("\n== memory per concurrent authenticated connection with one open session ==");
        console.log("conns   peakRSS MB   +peakRSS B/conn   curRSS MB   +curRSS B/conn");
        for (const n of CONNS) {
            held = [];
            const kids = [];
            for (let i = 0; i < n; i++) kids.push(new sys.Spawn("ssh", args("hold")));
            for (let i = 0; i < 2000 && held.length < n; i++) await sleep(20);
            if (held.length < n) throw new Error("only " + held.length + " of " + n + " sessions opened");
            await sleep(100);
            std.gc();
            const r = rss(), l = live();
            console.log(String(n).padStart(5) + "   " + MiB(r).padStart(9) + "   " + Math.round((r - r0) / n).toString().padStart(15) + "   " + MiB(l).padStart(9) + "   " + Math.round((l - l0) / n).toString().padStart(14));
            for (const s of held) s.exit(0);
            await Promise.all(kids.map((k) => k.wait()));
            for (const k of kids) k.close();
            held = [];
            await sleep(100);
            std.gc();
        }
        console.log("after closing all: current RSS " + MiB(live()) + " MiB (baseline " + MiB(l0) + "), connections " + srv.stats.connections);
    }

    if (SECTIONS.has("plateau")) {
        console.log("\n== plateau: " + ROUNDS + " rounds x " + CYCLES + " connect/exec/close cycles (8 ssh children at a time) ==");
        console.log("round   peakRSS MB   curRSS KB   JS heap KB   objects   accepted");
        for (let r = 0; r < ROUNDS; r++) {
            for (let i = 0; i < CYCLES; i += 8) {
                const batch = [];
                for (let j = i; j < Math.min(CYCLES, i + 8); j++) batch.push(sshCli(args("x")));
                const res = await Promise.all(batch);
                for (const x of res) if (x.code !== 0) throw new Error("cycle failed: " + x.stderr);
            }
            std.gc();
            const m = sys.memoryUsage();
            console.log(String(r + 1).padStart(5) + "   " + MiB(rss()).padStart(9) + "   " + Math.round(live() / 1024).toString().padStart(9) + "   " + Math.round(m.memoryUsedSize / 1024).toString().padStart(10) + "   " + String(m.objCount).padStart(7) + "   " + srv.stats.accepted);
        }
    }
} finally {
    srv.close();
}
