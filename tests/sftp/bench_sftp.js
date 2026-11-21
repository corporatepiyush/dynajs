// flags: --std
// dyna:sftp bench (rule 11: CPU and memory). One pinned cipher (chacha20-poly1305@openssh.com), loopback, SIZE-byte file, best of REPS.
// Sections (SFTP_BENCH_SECTIONS, default all; run "rss" and "plateau" each in a fresh process, since peak RSS is a high-water mark):
//   throughput: four pairings, upload and download each: ours->ours (client and server in this process), OpenSSH sftp -> ours server
//               (this process is the server alone), ours client -> OpenSSH sshd internal-sftp (this process is the client alone), and
//               OpenSSH sftp -> OpenSSH sshd (client CPU and RSS from /usr/bin/time -l). MB/s, CPU ns/B of the measured side, peak RSS.
//               Our client streams 1 MiB local chunks through SFTPFile.write()/read(), never holding the file.
//   rss:        per concurrent ours->ours session (one SSH connection + one SFTP session + one open handle that has read 1 MiB) at
//               the CONNS counts: current RSS growth per session (both ends live in this process).
//   plateau:    ROUNDS rounds of CYCLES connect/open/writeFile 64 KiB/readFile/close cycles: current RSS, peak RSS, open fds per round.
// SFTP_BENCH_RSSMODE: "read" (default; as above), "sftp" (no file opened), "ssh" (an authenticated SSH connection, no channel) for the breakdown.
// Env: SFTP_BENCH_MB (256), SFTP_BENCH_REPS (3), SFTP_BENCH_ROUNDS (8), SFTP_BENCH_CYCLES (40), SFTP_BENCH_CONNS ("1,16,64").
import * as std from "std";
import * as os from "os";
import * as sys from "dyna:sys";
import { SSHClient } from "dyna:ssh";
import { SFTPClient } from "dyna:sftp";
import { HAVE_SSH, HAVE_SSHD, tmpDir, startSshd, collect } from "../ssh/ssh_util.js";
import { sftpHost } from "./sftp_util.js";

if (!HAVE_SSH || !HAVE_SSHD || !sys.Which("sftp")) { console.log("bench_sftp: no OpenSSH >= 9.9 ssh/sftp/sshd; skipped"); std.exit(0); }
const MB = Number(std.getenv("SFTP_BENCH_MB") || 256);
const REPS = Number(std.getenv("SFTP_BENCH_REPS") || 3);
const ROUNDS = Number(std.getenv("SFTP_BENCH_ROUNDS") || 8);
const CYCLES = Number(std.getenv("SFTP_BENCH_CYCLES") || 40);
const CONNS = (std.getenv("SFTP_BENCH_CONNS") || "1,16,64").split(",").map(Number);
const SECTIONS = new Set((std.getenv("SFTP_BENCH_SECTIONS") || "throughput,rss,plateau").split(","));
const RSSMODE = std.getenv("SFTP_BENCH_RSSMODE") || "read";
const CIPHER = "chacha20-poly1305@openssh.com";
const SIZE = MB << 20;
const cpu = () => { const u = sys.cpuUsage(); return u.user + u.system; };
const rss = () => sys.rusage().maxrss;
const live = () => Number(sys.Exec("ps", ["-o", "rss=", "-p", String(sys.pid())]).stdout.trim()) * 1024;
const fds = () => os.readdir("/dev/fd")[0].length;
const now = () => performance.now();
const f1 = (x) => x.toFixed(1);
const MiB = (x) => f1(x / 1048576);

const dir = tmpDir("sftpbench");
const root = dir + "/root";
os.mkdir(root, 0o755);
for (const d of ["/h", "/hr", "/hp"]) os.mkdir(dir + d, 0o755);
const src = dir + "/src.bin";
if (sys.Exec("/bin/sh", ["-c", "head -c " + SIZE + " /dev/urandom > '" + src + "'"]).code !== 0) throw new Error("cannot make " + src);
const cmp = (a, b) => sys.Exec("cmp", ["-s", a, b]).code === 0;
const buf = new Uint8Array(1 << 20);

async function ourUp(sftp, remote) {
    const f = std.open(src, "rb");
    const h = await sftp.open(remote, "w");
    for (;;) { const n = f.read(buf.buffer, 0, buf.length); if (n <= 0) break; await h.write(buf.subarray(0, n)); }
    f.close();
    await h.close();
}

async function ourDown(sftp, remote, local) {
    const f = std.open(local, "wb");
    const h = await sftp.open(remote);
    for (let c; (c = await h.read()) !== null;) f.write(c.buffer, c.byteOffset, c.byteLength);
    f.close();
    await h.close();
}

async function best(fn) {
    let b = null;
    for (let i = 0; i < REPS; i++) {
        const c0 = cpu(), t0 = now();
        const v = await fn(i);
        const r = { dt: (now() - t0) / 1000, dc: cpu() - c0, v };
        if (!b || r.dt < b.dt) b = r;
    }
    return b;
}

async function osftp(port, user, kh, key, lines, timed) {
    write(dir + "/batch", lines.join("\n") + "\n");
    const args = ["-F", "/dev/null", "-b", dir + "/batch", "-P", String(port), "-i", key, "-c", CIPHER, "-o", "IdentitiesOnly=yes",
        "-o", "BatchMode=yes", "-o", "UserKnownHostsFile=" + kh, "-o", "StrictHostKeyChecking=yes", "-o", "LogLevel=ERROR", "-o", "RekeyLimit=4G",
        user + "@127.0.0.1"];
    const p = new sys.Spawn("/bin/sh", ["-c", "/usr/bin/time -l sftp " + args.map((x) => "'" + x + "'").join(" ") + " > /dev/null"]);
    let r;
    try { const err = new TextDecoder().decode(await collect(p.stderr)); r = { code: (await p.wait()).code, stderr: err }; } finally { p.close(); }
    const m = /([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys/.exec(r.stderr);
    const rs = /(\d+)\s+maximum resident set size/.exec(r.stderr);
    if (r.code !== 0 || !m) throw new Error("sftp failed: " + r.stderr.slice(0, 400));
    return { dt: +m[1], cpu: +m[2] + +m[3], maxrss: rs ? +rs[1] : 0 };
}

function write(path, text) { const f = std.open(path, "w"); f.puts(text); f.close(); }
function row(pair, dirn, side, r, extra) {
    console.log(pair.padEnd(26) + dirn.padEnd(6) + f1(MB / r.dt).padStart(8) + "  " + side.padEnd(16) + (r.dc * 1e9 / SIZE).toFixed(2).padStart(6) + "  " + (extra || ""));
}

console.log("bench_sftp: sections " + [...SECTIONS].join(",") + "; " + MB + " MiB, " + CIPHER + ", best of " + REPS + "; host " + sys.cpuInfo().model
    + "; load " + sys.loadAvg().map((x) => x.toFixed(2)).join(" ") + "; baseline peakRSS " + MiB(rss()) + " MiB, current RSS " + MiB(live()) + " MiB, fds " + fds());

const hosts = [];
let sshd = null;
try {
    if (SECTIONS.has("throughput")) {
        const h = sftpHost(dir + "/h", { root }, { dyna: { rekeyBytes: 4294967296 } });
        hosts.push(h);
        sshd = await startSshd({ tag: "sftpbench", ciphers: CIPHER, logLevel: "INFO", extra: ["Subsystem sftp internal-sftp", "RekeyLimit 4G"] });
        console.log("pairing                   dir      MB/s  measured side     ns/B  notes");
        {
            const ssh = await SSHClient.connect({ ...h.base, dyna: { rekeyBytes: 4294967296 } });
            const sftp = await SFTPClient.open(ssh);
            const up = await best(() => ourUp(sftp, "up.bin"));
            row("ours -> ours", "up", "client+server", up, "maxInflight " + sftp.stats.maxInflight + (cmp(src, root + "/up.bin") ? "" : " MISMATCH"));
            const down = await best(() => ourDown(sftp, "up.bin", dir + "/down.bin"));
            row("ours -> ours", "down", "client+server", down, cmp(src, dir + "/down.bin") ? "" : "MISMATCH");
            await sftp.close();
            ssh.close();
        }
        {
            const key = dir + "/h/user";
            const up = await best(() => osftp(h.srv.port, "alice", h.kh, key, ["put " + src + " up2.bin"]));
            const o1 = up.v;
            row("openssh sftp -> ours", "up", "ours server", up, "openssh client " + (o1.cpu * 1e9 / SIZE).toFixed(2) + " ns/B, peakRSS " + MiB(o1.maxrss) + " MiB" + (cmp(src, root + "/up2.bin") ? "" : " MISMATCH"));
            const down = await best(() => osftp(h.srv.port, "alice", h.kh, key, ["get up2.bin " + dir + "/down2.bin"]));
            const o2 = down.v;
            row("openssh sftp -> ours", "down", "ours server", down, "openssh client " + (o2.cpu * 1e9 / SIZE).toFixed(2) + " ns/B, peakRSS " + MiB(o2.maxrss) + " MiB" + (cmp(src, dir + "/down2.bin") ? "" : " MISMATCH"));
        }
        {
            const ssh = await SSHClient.connect({ ...sshd.base, dyna: { rekeyBytes: 4294967296 } });
            const sftp = await SFTPClient.open(ssh);
            const up = await best(() => ourUp(sftp, dir + "/up3.bin"));
            row("ours -> openssh sshd", "up", "ours client", up, cmp(src, dir + "/up3.bin") ? "" : "MISMATCH");
            const down = await best(() => ourDown(sftp, dir + "/up3.bin", dir + "/down3.bin"));
            row("ours -> openssh sshd", "down", "ours client", down, cmp(src, dir + "/down3.bin") ? "" : "MISMATCH");
            await sftp.close();
            ssh.close();
        }
        {
            const ou = [], od = [];
            for (let i = 0; i < REPS; i++) ou.push(await osftp(sshd.port, sys.getEnv("USER") || "root", sshd.known, sshd.dir + "/user", ["put " + src + " " + dir + "/up4.bin"]));
            for (let i = 0; i < REPS; i++) od.push(await osftp(sshd.port, sys.getEnv("USER") || "root", sshd.known, sshd.dir + "/user", ["get " + dir + "/up4.bin " + dir + "/down4.bin"]));
            const pick = (a) => a.reduce((x, y) => (y.dt < x.dt ? y : x));
            const u = pick(ou), d = pick(od);
            row("openssh -> openssh sshd", "up", "openssh client", { dt: u.dt, dc: u.cpu }, "peakRSS " + MiB(u.maxrss) + " MiB" + (cmp(src, dir + "/up4.bin") ? "" : " MISMATCH"));
            row("openssh -> openssh sshd", "down", "openssh client", { dt: d.dt, dc: d.cpu }, "peakRSS " + MiB(d.maxrss) + " MiB" + (cmp(src, dir + "/down4.bin") ? "" : " MISMATCH"));
        }
        for (const x of ["up3.bin", "down3.bin", "up4.bin", "down4.bin", "down.bin", "down2.bin"]) try { os.remove(dir + "/" + x); } catch {}
        for (const x of ["up.bin", "up2.bin"]) try { os.remove(root + "/" + x); } catch {}
        console.log("throughput: peakRSS " + MiB(rss()) + " MiB (high-water of every pairing above in this process)");
        sshd.close();
        sshd = null;
    }

    if (SECTIONS.has("rss")) {
        const h = sftpHost(dir + "/hr", { root }, { dyna: { maxConnections: 1024 } });
        hosts.push(h);
        os.remove(src);
        const f = std.open(root + "/one.bin", "wb");
        f.write(buf.buffer, 0, buf.length);
        f.close();
        const held = [];
        const l0 = live(), r0 = rss();
        console.log("mode " + RSSMODE);
        console.log("sessions  liveRSS MB  per-session KiB  peakRSS growth MB");
        for (const n of CONNS) {
            while (held.length < n) {
                const ssh = await SSHClient.connect(h.base);
                if (RSSMODE === "ssh") { held.push({ ssh, sftp: { close: async () => {} }, fh: { close: async () => {} } }); continue; }
                const sftp = await SFTPClient.open(ssh);
                const fh = RSSMODE === "sftp" ? { close: async () => {} } : await sftp.open("one.bin");
                let got = RSSMODE === "sftp" ? 1 << 20 : 0;
                while (got < (1 << 20)) { const c = await fh.read(); if (c === null) break; got += c.length; }
                held.push({ ssh, sftp, fh });
            }
            await new Promise((r) => setTimeout(r, 50));
            const l = live();
            console.log(String(n).padStart(8) + MiB(l).padStart(12) + f1((l - l0) / n / 1024).padStart(17) + MiB(rss() - r0).padStart(19));
        }
        for (const x of held) { await x.fh.close(); await x.sftp.close(); x.ssh.close(); }
    }

    if (SECTIONS.has("plateau")) {
        const h = sftpHost(dir + "/hp", { root });
        hosts.push(h);
        const small = new Uint8Array(65536).map((_, i) => (i * 7) & 255);
        let served = 0;
        console.log("round  liveRSS MB  peakRSS MB  fds  served  JS heap, objects");
        for (let r = 0; r < ROUNDS; r++) {
            for (let i = 0; i < CYCLES; i++) {
                const ssh = await SSHClient.connect(h.base);
                const sftp = await SFTPClient.open(ssh);
                await sftp.writeFile("p.bin", small);
                const back = await sftp.readFile("p.bin");
                if (back.length !== small.length) throw new Error("plateau read " + back.length);
                await sftp.close();
                ssh.close();
            }
            await new Promise((res) => setTimeout(res, 50));
            served += h.served.length;
            h.served.length = 0;
            std.gc();
            const m = sys.memoryUsage();
            console.log(String(r).padStart(5) + MiB(live()).padStart(12) + MiB(rss()).padStart(12) + String(fds()).padStart(5) + String(served).padStart(8) + "  " + "JS heap KB " + Math.round(m.memoryUsedSize / 1024) + "  objects " + m.objCount);
        }
    }
} finally {
    if (sshd) sshd.close();
    for (const h of hosts) h.close();
}
console.log("bench_sftp: done; load " + sys.loadAvg().map((x) => x.toFixed(2)).join(" "));
