// flags: --std
// dyna:ssh port-forwarding bench (rule 11: CPU and memory). Loopback, chacha20-poly1305@openssh.com, best of REPS.
// Sections (FWD_BENCH_SECTIONS, default all; run "rss", "sessions" and "plateau" each in a fresh process, since peak RSS is a high-water mark):
//   throughput: SIZE bytes one way through a local forward to a counting target (it answers "got N" after EOF), five pairings:
//               direct (no SSH: the cost of the in-process TCP endpoints), ours client -> ours server (both in this process), ours
//               client -> OpenSSH sshd, OpenSSH ssh -L -> ours server, OpenSSH ssh -L -> OpenSSH sshd. MB/s, this process's CPU ns/B
//               (it always includes the endpoints; subtract the direct row), the ssh client's CPU and peak RSS from /usr/bin/time -l.
//   rss:        CONNS concurrent TCP connections through one local forward (ours -> ours), each after a 1 MiB echo burst, kept open:
//               current RSS growth and client stats.bufferBytes per forwarded connection, with dyna.shrinkBuffers off and on.
//   sessions:   CONNS concurrent SSH connections, each after a 4 MiB exec burst, kept open: RSS growth and bufferBytes per
//               connection, shrinkBuffers off and on.
//   plateau:    ROUNDS rounds of CYCLES connect / forwardLocal / 64 KiB echo / close: current RSS, peak RSS, open fds per round.
// Run each shrink mode in its own process too (FWD_BENCH_SHRINK=0 or 1; default "0,1" runs both, the second on the first's freed heap).
// Env: FWD_BENCH_MB (128), FWD_BENCH_REPS (3), FWD_BENCH_ROUNDS (8), FWD_BENCH_CYCLES (40), FWD_BENCH_CONNS ("16,64,256").
import * as std from "std";
import * as os from "os";
import * as sys from "dyna:sys";
import { TCPServer } from "dyna:net";
import { SSHClient, SSHServer } from "dyna:ssh";
import { HAVE_SSH, HAVE_SSHD, tmpDir, keygen, write, pubBlob, startSshd, collect, readAll } from "./ssh_util.js";
import { echoTarget, halfTarget, exchange } from "./fwd_util.js";

if (!HAVE_SSH || !HAVE_SSHD) { console.log("bench_ssh_forward: no OpenSSH >= 9.9 ssh/sshd; skipped"); std.exit(0); }
const MB = Number(std.getenv("FWD_BENCH_MB") || 128);
const REPS = Number(std.getenv("FWD_BENCH_REPS") || 3);
const ROUNDS = Number(std.getenv("FWD_BENCH_ROUNDS") || 8);
const CYCLES = Number(std.getenv("FWD_BENCH_CYCLES") || 40);
const CONNS = (std.getenv("FWD_BENCH_CONNS") || "16,64,256").split(",").map(Number);
const SHRINK = (std.getenv("FWD_BENCH_SHRINK") || "0,1").split(",").map((x) => x === "1");
const SECTIONS = new Set((std.getenv("FWD_BENCH_SECTIONS") || "throughput,rss,sessions,plateau").split(","));
const CIPHER = "chacha20-poly1305@openssh.com";
const SIZE = MB << 20;
const cpu = () => { const u = sys.cpuUsage(); return u.user + u.system; };
const rss = () => sys.rusage().maxrss;
const live = () => Number(sys.Exec("ps", ["-o", "rss=", "-p", String(sys.pid())]).stdout.trim()) * 1024;
const fds = () => os.readdir("/dev/fd")[0].length;
const now = () => performance.now();
const MiB = (x) => (x / 1048576).toFixed(1);
const KiB = (x) => (x / 1024).toFixed(0);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => { const s = new TCPServer({ port: 0 }); s.start({}); const p = s.port; s.close(); return p; };

const dir = tmpDir("fwdbench");
const host = keygen(dir + "/host");
const user = keygen(dir + "/user");
const half = halfTarget();
const echo = echoTarget();
const servers = [];
const ours = (meta = {}) => {
    const s = SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub + "\n", forward: () => true, dyna: { maxConnections: 4096, maxChannels: 4096, ...meta },
        onSession: async (x) => { if (x.command === "burst") await x.write(new Uint8Array(4 << 20)); await new Promise(() => {}); } });
    servers.push(s);
    write(dir + "/kh" + s.port, "[127.0.0.1]:" + s.port + " " + pubBlob(host.pub) + "\n");
    return s;
};
const base = (s, meta = {}) => ({ host: "127.0.0.1", port: s.port, username: "u", privateKey: user.priv, knownHosts: dir + "/kh" + s.port, dyna: { maxChannels: 4096, ...meta } });
let payload = null;

async function pushThrough(port) {
    if (!payload) { payload = new Uint8Array(SIZE); for (let i = 0; i < SIZE; i += 4096) payload[i] = i >>> 12; }
    const r = await exchange(port, payload, { timeoutMs: 600000 });
    const got = new TextDecoder().decode(r.bytes);
    if (got !== "got " + SIZE) throw new Error("bad answer through port " + port + ": " + got + " " + r.error);
}

async function best(fn) {
    let b = null;
    for (let i = 0; i < REPS; i++) {
        const c0 = cpu(), t0 = now();
        await fn(i);
        const r = { dt: (now() - t0) / 1000, dc: cpu() - c0 };
        if (!b || r.dt < b.dt) b = r;
    }
    return b;
}

function row(pair, r, extra) {
    console.log(pair.padEnd(30) + (MB / r.dt).toFixed(1).padStart(8) + " MB/s  this process " + (r.dc * 1e9 / SIZE).toFixed(2).padStart(6) + " ns/B  " + (extra || ""));
}

async function sshL(cfgArgs, lp, target, timed) {
    const args = cfgArgs.concat(["-N", "-o", "ExitOnForwardFailure=yes", "-c", CIPHER, "-L", "127.0.0.1:" + lp + ":127.0.0.1:" + target]);
    const p = new sys.Spawn("/bin/sh", ["-c", "/usr/bin/time -l ssh " + args.map((x) => "'" + x + "'").join(" ")]);
    for (let i = 0; i < 200; i++) {
        const ok = await new Promise((res) => TCPServer.connect({ host: "127.0.0.1", port: lp }, { connect: (c) => { if (c) c.close(); res(!!c); } }));
        if (ok) break;
        await sleep(25);
    }
    const r = await timed();
    sys.Exec("pkill", ["-TERM", "-P", String(p.pid), "ssh"]);
    const err = new TextDecoder().decode(await collect(p.stderr));
    await p.wait();
    p.close();
    const m = /([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys/.exec(err);
    const rs = /(\d+)\s+maximum resident set size/.exec(err);
    return { r, sshCpu: m ? +m[2] + +m[3] : NaN, sshRss: rs ? +rs[1] : 0 };
}

const optsFile = (kh, port) => ["-F", "/dev/null", "-p", String(port), "-i", dir + "/user", "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
    "-o", "UserKnownHostsFile=" + kh, "-o", "StrictHostKeyChecking=yes", "-o", "LogLevel=ERROR", "-o", "RekeyLimit=4G", "u@127.0.0.1"];

console.log("bench_ssh_forward: sections " + [...SECTIONS].join(",") + "; " + MB + " MiB, " + CIPHER + ", best of " + REPS + "; host " + sys.cpuInfo().model
    + "; load " + sys.loadAvg().map((x) => x.toFixed(2)).join(" ") + "; baseline peakRSS " + MiB(rss()) + " MiB, current RSS " + MiB(live()) + " MiB, fds " + fds());

if (SECTIONS.has("throughput")) {
    row("direct (no ssh)", await best(() => pushThrough(half.port)));
    {
        const s = ours();
        const c = await SSHClient.connect(base(s));
        const f = c.forwardLocal({ host: "127.0.0.1", port: half.port });
        row("ours -> ours", await best(() => pushThrough(f.listenPort)));
        f.close();
        c.close();
    }
    const d = await startSshd({ tag: "fwdbench", ciphers: CIPHER, extra: ["AllowTcpForwarding yes"] });
    try {
        const c = await SSHClient.connect(d.base);
        const f = c.forwardLocal({ host: "127.0.0.1", port: half.port });
        row("ours -> openssh sshd", await best(() => pushThrough(f.listenPort)));
        f.close();
        c.close();
        const s = ours();
        const lp = freePort();
        const o = await sshL(optsFile(dir + "/kh" + s.port, s.port), lp, half.port, () => best(() => pushThrough(lp)));
        row("openssh ssh -L -> ours", o.r, "ssh client " + (o.sshCpu * 1e9 / (SIZE * REPS)).toFixed(2) + " ns/B avg, peakRSS " + MiB(o.sshRss) + " MiB");
        const lp2 = freePort();
        const dargs = ["-F", "/dev/null", "-p", String(d.port), "-i", d.dir + "/user", "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "UserKnownHostsFile=" + d.known,
            "-o", "StrictHostKeyChecking=yes", "-o", "LogLevel=ERROR", "-o", "RekeyLimit=4G", d.base.username + "@127.0.0.1"];
        const o2 = await sshL(dargs, lp2, half.port, () => best(() => pushThrough(lp2)));
        row("openssh ssh -L -> openssh sshd", o2.r, "ssh client " + (o2.sshCpu * 1e9 / (SIZE * REPS)).toFixed(2) + " ns/B avg, peakRSS " + MiB(o2.sshRss) + " MiB");
    } finally {
        d.close();
    }
    console.log("throughput: peakRSS " + MiB(rss()) + " MiB (high-water of every pairing in this process)");
}

function burstConn(port, n) {
    return new Promise((resolve, reject) => {
        let got = 0;
        TCPServer.connect({ host: "127.0.0.1", port }, {
            connect: (conn, err) => { if (!conn) { reject(new Error(String(err))); return; } conn.write(new Uint8Array(n)); },
            data: (conn, bytes) => { got += bytes.byteLength; if (got >= n) resolve(conn); },
            close: () => { if (got < n) reject(new Error("forwarded connection closed after " + got + " bytes")); },
        });
    });
}

if (SECTIONS.has("rss")) {
    for (const shrink of SHRINK) {
        const s = ours({ shrinkBuffers: shrink });
        const c = await SSHClient.connect(base(s, { shrinkBuffers: shrink }));
        const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
        let held = [];
        const l0 = live();
        for (const n of CONNS) {
            while (held.length < n) held.push(await burstConn(f.listenPort, 1 << 20));
            await sleep(200);
            const l = live();
            console.log("rss shrinkBuffers=" + shrink + " forwarded conns " + String(n).padStart(4) + ": RSS growth " + KiB((l - l0) / n) + " KiB/conn, client bufferBytes "
                + KiB(c.stats.bufferBytes / n) + " KiB/conn, current RSS " + MiB(l) + " MiB, peakRSS " + MiB(rss()) + " MiB, fds " + fds());
        }
        for (const h of held) h.close();
        held = [];
        f.close();
        c.close();
        await sleep(300);
    }
}

if (SECTIONS.has("sessions")) {
    for (const shrink of SHRINK) {
        const s = ours({ shrinkBuffers: shrink });
        const held = [];
        const l0 = live();
        for (const n of CONNS) {
            while (held.length < n) {
                const c = await SSHClient.connect(base(s, { shrinkBuffers: shrink }));
                const x = await c.exec("burst");
                const r = x.stdout.getReader();
                let got = 0;
                while (got < 4 << 20) { const { value, done } = await r.read(); if (done) break; got += value.byteLength; }
                held.push(c);
            }
            await sleep(200);
            const l = live();
            let bb = 0;
            for (const c of held) bb += c.stats.bufferBytes;
            console.log("sessions shrinkBuffers=" + shrink + " connections " + String(n).padStart(4) + ": RSS growth " + KiB((l - l0) / n) + " KiB/conn (client and server ends), client bufferBytes "
                + KiB(bb / n) + " KiB/conn, current RSS " + MiB(l) + " MiB, peakRSS " + MiB(rss()) + " MiB");
        }
        for (const c of held) c.close();
        s.close();
        await sleep(300);
    }
}

if (SECTIONS.has("plateau")) {
    const s = ours();
    const data = new Uint8Array(64 << 10);
    for (let round = 0; round < ROUNDS; round++) {
        const t0 = now();
        for (let i = 0; i < CYCLES; i++) {
            const c = await SSHClient.connect(base(s));
            const f = c.forwardLocal({ host: "127.0.0.1", port: echo.port });
            const r = await exchange(f.listenPort, data);
            if (r.bytes.byteLength !== data.byteLength) throw new Error("short echo " + r.bytes.byteLength);
            f.close();
            c.close();
        }
        await sleep(100);
        console.log("plateau round " + round + ": " + CYCLES + " cycles in " + ((now() - t0) / 1000).toFixed(2) + "s, current RSS " + MiB(live()) + " MiB, peakRSS " + MiB(rss()) + " MiB, fds " + fds());
    }
}

for (const s of servers) s.close();
half.close();
echo.close();
