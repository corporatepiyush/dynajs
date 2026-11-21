// flags: --std
// dyna:ssh ssh-agent bench (rule 11: CPU and memory), SSHAgent against OpenSSH's ssh-agent (-D -a, killed at the end), both driven by the
// same in-process raw protocol client (tests/ssh/agent_util.js), so the client's cost is equal on both sides.
// Sections (AGENT_BENCH_SECTIONS, default all):
//   sign:    SIGNS ed25519 SIGN_REQUESTs (180-byte userauth-sized data), on 1 connection (request/response) and on 8 connections at once:
//            signatures/s, and CPU per signature: OpenSSH's agent process from ps cputime; ours is this process's CPU (agent and
//            client together; the signature itself runs on the worker pool) minus the client-only cost measured against a FAILURE reply.
//   keys:    a size sweep N = 16..16384 keys added over the wire: current RSS growth per key (ours: this process; OpenSSH: the agent's RSS).
//   plateau: ROUNDS rounds of add 1000 keys / sign 200 / remove all: current RSS per round on both (leak or plateau).
// Env: AGENT_BENCH_SIGNS (4000), AGENT_BENCH_ROUNDS (12), AGENT_BENCH_MAXN (16384).
import * as std from "std";
import * as os from "os";
import * as sys from "dyna:sys";
import { SSHAgent } from "dyna:ssh";
import { Ed25519Generate, RandomBytes } from "dyna:crypto";
import { tmpDir, sleep } from "./ssh_util.js";
import { W } from "./fake_server.js";
import { AG, addEd, edBlob, rawAgent, sockDir } from "./agent_util.js";

if (!sys.Which("ssh-agent")) { console.log("bench_ssh_agent: no ssh-agent; skipped"); std.exit(0); }
const SIGNS = Number(std.getenv("AGENT_BENCH_SIGNS") || 4000);
const ROUNDS = Number(std.getenv("AGENT_BENCH_ROUNDS") || 12);
const MAXN = Number(std.getenv("AGENT_BENCH_MAXN") || 16384);
const SECTIONS = new Set((std.getenv("AGENT_BENCH_SECTIONS") || "sign,keys,plateau").split(","));

const dir = sockDir(tmpDir("agbench"));
const rssKiB = (pid) => Number(sys.Exec("ps", ["-o", "rss=", "-p", String(pid)]).stdout.trim());
const cpuSec = (pid) => { const t = sys.Exec("ps", ["-o", "time=", "-p", String(pid)]).stdout.trim().split(":").map(Number); return t.reduce((a, x) => a * 60 + x, 0); };
const selfCpu = () => { const r = sys.Exec("ps", ["-o", "time=", "-p", String(sys.pid())]).stdout.trim().split(":").map(Number); return r.reduce((a, x) => a * 60 + x, 0); };
const now = () => Date.now();

const ours = SSHAgent.listen({ path: dir + "/ours.sock", dyna: { maxKeys: 65536, maxConnections: 64 } });
const osPath = dir + "/openssh.sock";
const osAgent = new sys.Spawn("ssh-agent", ["-D", "-a", osPath], { timeoutMs: 600000 });
for (let i = 0; i < 200 && os.stat(osPath)[1] !== 0; i++) await sleep(10);
const osPid = Number(sys.Exec("/bin/sh", ["-c", "pgrep -f 'ssh-agent -D -a " + osPath + "'"]).stdout.trim().split("\n")[0]);
const targets = [["ours", ours.path, sys.pid()], ["openssh", osPath, osPid]];
const out = { host: sys.Exec("uname", ["-sm"]).stdout.trim(), signs: SIGNS };

async function signRun(path, kp, conns, n, bad) {
    const rs = await Promise.all(Array.from({ length: conns }, () => rawAgent(path)));
    const data = RandomBytes(180);
    const req = new W().u8(AG.SIGN).str(edBlob(bad ? Ed25519Generate().publicKey : kp.publicKey)).str(data).u32(0).done();
    const t0 = now();
    await Promise.all(rs.map(async (r) => { for (let i = 0; i < n / conns; i++) { const m = await r.req(req); if (m[0] !== (bad ? AG.FAILURE : AG.SIGN_RESP)) throw new Error("reply " + m[0]); } }));
    const ms = now() - t0;
    for (const r of rs) r.close();
    return ms;
}

try {
    if (SECTIONS.has("sign")) {
        out.sign = {};
        for (const [name, path, pid] of targets) {
            const kp = Ed25519Generate();
            const r = await rawAgent(path);
            await r.req(addEd(kp, "bench"));
            r.close();
            for (const conns of [1, 8]) {
                await signRun(path, kp, conns, 200, false);
                const c0 = name === "ours" ? selfCpu() : cpuSec(pid);
                const ms = await signRun(path, kp, conns, SIGNS, false);
                const c1 = name === "ours" ? selfCpu() : cpuSec(pid);
                let clientOnly = 0;
                if (name === "ours") { const b0 = selfCpu(); await signRun(path, kp, conns, SIGNS, true); clientOnly = selfCpu() - b0; }
                out.sign[name + "_c" + conns] = { opsPerSec: Math.round(SIGNS / (ms / 1000)), cpuUsPerSign: +(((c1 - c0 - clientOnly) / SIGNS) * 1e6).toFixed(1), cpuClockResolutionS: 0.01 };
            }
        }
        out.sign.oursOffloaded = ours.stats.offloaded;
    }
    if (SECTIONS.has("keys")) {
        out.keys = {};
        for (const [name, path, pid] of targets) {
            const rows = [];
            const r = await rawAgent(path);
            await r.req(Uint8Array.of(AG.REMOVE_ALL));
            await sleep(50);
            const base = rssKiB(pid);
            let have = 0;
            for (let n = 16; n <= MAXN; n *= 4) {
                for (; have < n; have++) { const m = await r.req(addEd(Ed25519Generate(), "k" + have)); if (m[0] !== AG.SUCCESS) throw new Error(name + " add failed at " + have); }
                const kib = rssKiB(pid) - base;
                rows.push({ n, rssGrowthKiB: kib, bytesPerKey: Math.round((kib * 1024) / n) });
            }
            await r.req(Uint8Array.of(AG.REMOVE_ALL));
            r.close();
            out.keys[name] = rows;
        }
    }
    if (SECTIONS.has("plateau")) {
        out.plateau = {};
        for (const [name, path, pid] of targets) {
            const rows = [];
            const r = await rawAgent(path);
            for (let round = 0; round < ROUNDS; round++) {
                const kps = Array.from({ length: 1000 }, () => Ed25519Generate());
                for (const kp of kps) await r.req(addEd(kp, "p"));
                const sreq = new W().u8(AG.SIGN).str(edBlob(kps[0].publicKey)).str(RandomBytes(180)).u32(0).done();
                for (let i = 0; i < 200; i++) await r.req(sreq);
                await r.req(Uint8Array.of(AG.REMOVE_ALL));
                rows.push(rssKiB(pid));
            }
            r.close();
            out.plateau[name] = { rssKiBPerRound: rows, firstToLastKiB: rows[rows.length - 1] - rows[0], lastHalfKiB: rows[rows.length - 1] - rows[Math.floor(rows.length / 2)] };
        }
    }
} finally {
    ours.close();
    try { osAgent.kill("SIGTERM"); } catch {}
    try { await osAgent.wait(); } catch {}
    osAgent.close();
}
console.log(JSON.stringify(out, null, 1));
