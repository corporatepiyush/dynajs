// flags: --std
// covers: integration dyna:ssh.SSHAgent dyna:ssh.SSHClient -- agent interop with OpenSSH (>= 9.9): ssh-add and ssh against this module's agent, this module's client against OpenSSH's ssh-agent
// timeout: 180
// OpenSSH's tools run as children with SSH_AUTH_SOCK pointing at the socket under test (-F /dev/null, BatchMode, a private known_hosts).
// ssh-add against SSHAgent: -l/-L on an empty agent, add (a key with its certificate next to it adds both), -l/-L listing, -T (OpenSSH signs
// through this agent and verifies the signature), -t lifetime (visible as expiresAt), -c refused (no confirm constraint), -d, -D, and -x/-X
// lock and unlock through SSH_ASKPASS (a wrong passphrase keeps it locked; a locked agent lists nothing). ssh with this agent logs into a
// private sshd and into SSHServer (also by certificate), and ssh -A into SSHServer still runs its command with agent forwarding refused.
// SSHClient with `agent` against OpenSSH's own ssh-agent (-D -a, killed in teardown): into sshd and into SSHServer, and a locked OpenSSH agent
// gives the "refused for every key" error with 0 identities. The suite SKIPs without OpenSSH >= 9.9 (ssh, sshd, ssh-agent, ssh-add).
import { SSHClient, SSHServer, SSHAgent } from "dyna:ssh";
import * as sys from "dyna:sys";
import * as os from "os";
import { suite } from "../lib/kit.js";
import { sockDir } from "./agent_util.js";
import { HAVE_SSH, HAVE_SSHD, SKIP_WHY, SKIP_WHY_SSH, startSshd, tmpDir, keygen, write, pubBlob, run, text, sleep, collect, certify } from "./ssh_util.js";

const t = suite("ssh_agent_openssh");
const owned = [];
const keep = (x) => { owned.push(x); return x; };
t.after(() => { for (const x of owned) { try { x.close(); } catch {} } });

async function tool(cmd, args, env, timeoutMs = 20000) {
    const p = new sys.Spawn(cmd, args, { env: { PATH: std.getenv("PATH"), HOME: std.getenv("HOME"), ...env }, timeoutMs });
    try {
        const [o, e] = await Promise.all([collect(p.stdout), collect(p.stderr)]);
        const r = await p.wait();
        return { code: r.code, out: text(o), err: text(e) };
    } finally { p.close(); }
}

if (!HAVE_SSH || !HAVE_SSHD || !sys.Which("ssh-agent") || !sys.Which("ssh-add")) t.skip("openssh", !HAVE_SSH ? SKIP_WHY_SSH : !HAVE_SSHD ? SKIP_WHY : "no ssh-agent/ssh-add on this host");
else {
    const dir = tmpDir("agos");
    const host = keygen(dir + "/host");
    const u1 = keygen(dir + "/u1");
    const u2 = keygen(dir + "/u2");
    keygen(dir + "/ca");
    const ca = { pub: std.loadFile(dir + "/ca.pub") };
    certify(dir, dir + "/ca", dir + "/u2.pub", { principals: "carol" });
    write(dir + "/askpass", "#!/bin/sh\ncat " + dir + "/pass\n");
    os.exec(["chmod", "700", dir + "/askpass"]);
    const sdir = sockDir(dir);
    let seq = 0;
    const agent = (o = {}) => keep(SSHAgent.listen({ path: sdir + "/a" + (seq++) + ".sock", ...o }));
    const server = (o = {}) => keep(SSHServer.listen({ hostKey: host.priv, onSession: async (x) => { await x.write(x.user + ":" + x.keyType + "\n"); x.exit(0); }, ...o }));
    const kh = (port, hostPub) => { const p = dir + "/kh" + port; write(p, "[127.0.0.1]:" + port + " " + pubBlob(hostPub) + "\n"); return p; };
    const sshArgs = (port, known, extra = []) => ["-F", "/dev/null", "-o", "BatchMode=yes", "-o", "UserKnownHostsFile=" + known, "-o", "StrictHostKeyChecking=yes",
        "-o", "KexAlgorithms=mlkem768x25519-sha256", "-o", "PubkeyAcceptedAlgorithms=ssh-ed25519,ssh-ed25519-cert-v01@openssh.com", "-p", String(port), ...extra, "carol@127.0.0.1", "hello"];

    t.test("ssh-add: add, list, sign-and-verify, lifetime, confirm refused, delete, delete all", async (a) => {
        const g = agent();
        const env = { SSH_AUTH_SOCK: g.path };
        const e0 = await tool("ssh-add", ["-l"], env);
        a.eq(e0.code, 1);
        a.match(e0.out + e0.err, /no identities/);
        const add = await tool("ssh-add", [dir + "/u2"], env);
        a.eq(add.code, 0, add.err);
        a.match(add.err, /Certificate added/, "the certificate next to the key is added too");
        const l = await tool("ssh-add", ["-l"], env);
        a.eq(l.out.trim().split("\n").length, 2, l.out);
        a.match(l.out, /ED25519-CERT/);
        const L = await tool("ssh-add", ["-L"], env);
        a.ok(L.out.includes(pubBlob(u2.pub)), "-L lists the key line");
        a.deep(g.list().map((k) => k.type).sort(), ["ssh-ed25519", "ssh-ed25519-cert-v01@openssh.com"]);
        const T = await tool("ssh-add", ["-T", dir + "/u2.pub"], env);
        a.eq(T.code, 0, "ssh-add -T verified a signature made by this agent: " + T.err);
        a.ok(g.stats.signatures >= 1);
        const tl = await tool("ssh-add", ["-t", "30", dir + "/u1"], env);
        a.eq(tl.code, 0, tl.err);
        const e1 = g.list().find((k) => k.publicKey === pubBlob(u1.pub));
        a.ok(e1 && e1.expiresAt > Date.now() + 20000 && e1.expiresAt <= Date.now() + 30000, "expiresAt from ssh-add -t");
        const c = await tool("ssh-add", ["-c", dir + "/u1"], { ...env, SSH_ASKPASS: dir + "/askpass", SSH_ASKPASS_REQUIRE: "force" });
        a.ne(c.code, 0, "ssh-add -c is refused: " + c.err);
        const d = await tool("ssh-add", ["-d", dir + "/u1.pub"], env);
        a.eq(d.code, 0, d.err);
        a.eq(g.list().length, 2);
        const D = await tool("ssh-add", ["-D"], env);
        a.eq(D.code, 0, D.err);
        a.eq(g.list().length, 0);
    });

    t.test("ssh-add -x / -X: lock and unlock through SSH_ASKPASS", async (a) => {
        const g = agent({ keys: [{ privateKey: u1.priv }] });
        const env = { SSH_AUTH_SOCK: g.path, SSH_ASKPASS: dir + "/askpass", SSH_ASKPASS_REQUIRE: "force" };
        write(dir + "/pass", "s3cret\n");
        const x = await tool("ssh-add", ["-x"], env);
        a.eq(x.code, 0, x.err);
        a.eq(g.locked, true);
        const l = await tool("ssh-add", ["-l"], env);
        a.match(l.out + l.err, /no identities/, "a locked agent lists nothing");
        write(dir + "/pass", "wrong\n");
        a.ne((await tool("ssh-add", ["-X"], env)).code, 0, "a wrong passphrase");
        a.eq(g.locked, true);
        write(dir + "/pass", "s3cret\n");
        const X = await tool("ssh-add", ["-X"], env);
        a.eq(X.code, 0, X.err);
        a.eq(g.locked, false);
        a.eq((await tool("ssh-add", ["-l"], env)).code, 0);
    });

    t.test("ssh through this agent into sshd and into SSHServer (key and certificate); ssh -A gets no forwarding", async (a) => {
        const g = agent({ keys: [{ privateKey: u2.priv }] });
        const env = { SSH_AUTH_SOCK: g.path };
        const d = keep(await startSshd({ dir: tmpDir("agos-sshd"), extraAuth: [u2.pub] }));
        const user = sys.getEnv("USER") || "root";
        const r = await tool("ssh", sshArgs(d.port, d.known).map((x) => (x === "carol@127.0.0.1" ? user + "@127.0.0.1" : x === "hello" ? "echo sshd-ok" : x)), env);
        a.eq(r.code, 0, r.err.slice(-400));
        a.eq(r.out.trim(), "sshd-ok");
        const s = server({ authorizedKeys: u2.pub + "\n" });
        const r2 = await tool("ssh", sshArgs(s.port, kh(s.port, host.pub)), env);
        a.eq(r2.code, 0, r2.err.slice(-400));
        a.eq(r2.out.trim(), "carol:ssh-ed25519");
        const r3 = await tool("ssh", sshArgs(s.port, kh(s.port, host.pub), ["-A"]), env);
        a.eq(r3.code, 0, "ssh -A still runs: " + r3.err.slice(-400));
        a.eq(r3.out.trim(), "carol:ssh-ed25519");
        g.add({ privateKey: u2.priv, certificate: std.loadFile(dir + "/u2-cert.pub") });
        const sc = server({ trustedUserCAKeys: ca.pub });
        const r4 = await tool("ssh", sshArgs(sc.port, kh(sc.port, host.pub)), env);
        a.eq(r4.code, 0, r4.err.slice(-400));
        a.eq(r4.out.trim(), "carol:ssh-ed25519-cert-v01@openssh.com");
    }, { timeoutMs: 60000 });

    t.test("SSHClient through OpenSSH's ssh-agent into sshd and SSHServer; a locked OpenSSH agent", async (a) => {
        const p = sdir + "/os.sock";
        const ag = new sys.Spawn("ssh-agent", ["-D", "-a", p], { timeoutMs: 120000 });
        keep({ close() { try { ag.kill("SIGTERM"); } catch {} try { ag.close(); } catch {} } });
        for (let i = 0; i < 100 && os.stat(p)[1] !== 0; i++) await sleep(20);
        const env = { SSH_AUTH_SOCK: p };
        a.eq((await tool("ssh-add", [dir + "/u1"], env)).code, 0);
        const d = keep(await startSshd({ dir: tmpDir("agos-sshd2"), extraAuth: [u1.pub] }));
        const { privateKey: _pk, ...b } = d.base;
        const c = await SSHClient.connect({ ...b, agent: p });
        a.eq(text((await run(c, "echo via-openssh-agent")).stdout).trim(), "via-openssh-agent");
        c.close();
        const s = server({ authorizedKeys: u1.pub + "\n" });
        const c2 = await SSHClient.connect({ host: "127.0.0.1", port: s.port, username: "carol", knownHosts: kh(s.port, host.pub), agent: p });
        a.eq(text((await run(c2, "x")).stdout).trim(), "carol:ssh-ed25519");
        c2.close();
        write(dir + "/pass", "pw\n");
        a.eq((await tool("ssh-add", ["-x"], { ...env, SSH_ASKPASS: dir + "/askpass", SSH_ASKPASS_REQUIRE: "force" })).code, 0);
        await a.rejects(SSHClient.connect({ host: "127.0.0.1", port: s.port, username: "carol", knownHosts: kh(s.port, host.pub), agent: p }), /0 tried/);
    }, { timeoutMs: 60000 });
}

await t.run();
