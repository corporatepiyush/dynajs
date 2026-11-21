// flags: --std
// covers: dyna:ssh.SSHClient -- refusal paths driven by a scriptable JS server (fake_server.js): weak-only and non-strict peers, a forged host signature, wrong first packets, timeouts, auth and channel refusals, DISCONNECT reason codes
// timeout: 120
// Each refusal must reject connect() with a message naming the cause AND send SSH_MSG_DISCONNECT with the RFC 4253 reason code
// (the fake server records it). The forged-signature case proves the host signature over H is verified before NEWKEYS: the key blob
// is the trusted one from known_hosts, only one signature bit is flipped. Also: a tampered AES-GCM tag (MAC error), sk-ssh-ed25519 host
// signatures in the FIDO format (good and forged), and process teardown with a live exec or a pending connect (ssh_teardown_child.js):
// the exit code is the script's and the runtime frees cleanly.
import { SSHClient } from "dyna:ssh";
import { suite } from "../lib/kit.js";
import { FakeSSH, W, R, DEFAULT_KEX } from "./fake_server.js";
import * as std from "std";
import * as sys from "dyna:sys";
import { tmpDir, keygen, write, readAll, text } from "./ssh_util.js";

const t = suite("ssh_refusals");
const dir = tmpDir("refuse");
const user = keygen(dir + "/user");
const fakes = [];
t.after(() => { for (const f of fakes) f.close(); });

function fake(opts) {
    const disc = [];
    const f = new FakeSSH(Object.assign({
        onPacket: (s, ty, p) => { if (ty === 1) { const r = new R(p, 1); disc.push({ code: r.u32(), msg: r.cstr() }); } return false; },
    }, opts));
    f.disc = disc;
    fakes.push(f);
    write(dir + "/kh" + f.port, f.knownHostsLine());
    f.opts = { host: "127.0.0.1", port: f.port, username: "u", privateKey: user.priv, knownHosts: dir + "/kh" + f.port, dyna: { handshakeTimeoutMs: 3000 } };
    return f;
}
const settle = () => new Promise((r) => setTimeout(r, 50));

t.test("peer offering only weak algorithms", async (a) => {
    const f = fake({ kex: "diffie-hellman-group14-sha1,ecdh-sha2-nistp256,kex-strict-s-v00@openssh.com", hostKeyAlgs: "ssh-rsa,ecdsa-sha2-nistp256", ciphers: "aes128-cbc,aes256-ctr" });
    await a.rejects(SSHClient.connect(f.opts), /no common key exchange algorithm/);
    await settle();
    a.eq(f.disc[0] && f.disc[0].code, 3, "DISCONNECT key exchange failed");
    const g = fake({ ciphers: "aes128-ctr,aes256-cbc,3des-cbc" });
    await a.rejects(SSHClient.connect(g.opts), /no common cipher/);
    const h = fake({ hostKeyAlgs: "ssh-rsa,rsa-sha2-256,ssh-dss" });
    await a.rejects(SSHClient.connect(h.opts), /no common host key algorithm/);
    const z = fake({ comp: "zlib@openssh.com" });
    await a.rejects(SSHClient.connect(z.opts), /requires compression/);
});

t.test("non-strict-KEX peer refused (Terrapin)", async (a) => {
    const f = fake({ kex: "curve25519-sha256,ext-info-s" });
    await a.rejects(SSHClient.connect(f.opts), /does not offer strict key exchange/);
    await settle();
    a.eq(f.disc[0] && f.disc[0].code, 3);
});

t.test("forged host signature refused before NEWKEYS", async (a) => {
    const f = fake({ badSig: true });
    await a.rejects(SSHClient.connect(f.opts), /host key signature does not verify/);
    await settle();
    a.eq(f.disc[0] && f.disc[0].code, 9, "DISCONNECT host key not verifiable");
    a.ok(!f.sessions[0].log.includes(21), "the client never sent NEWKEYS");
});

t.test("host key blob of another type refused", async (a) => {
    const f = fake({ replyHostBlob: new W().str("ssh-rsa").str(Uint8Array.of(1, 0, 1)).str(new Uint8Array(256).fill(9)).done() });
    await a.rejects(SSHClient.connect(f.opts), /malformed ssh-ed25519 host key/);
});

t.test("strict KEX: first packet must be KEXINIT", async (a) => {
    const f = fake({ onGreeted: (s) => { s.send(new W().u8(2).str("x").done()); s.sendKexinit(); } });
    await a.rejects(SSHClient.connect(f.opts), /strict KEX: unexpected message 2 during the initial key exchange/);
    await settle();
    a.eq(f.disc[0] && f.disc[0].code, 2, "DISCONNECT protocol error");
    const g = fake({ onPacket: (s, ty) => { if (ty === 20) s.send(new W().u8(4).bool(false).str("dbg").str("").done()); return false; } });
    await a.rejects(SSHClient.connect(g.opts), /strict KEX: unexpected message 4/);
});

t.test("version line checks", async (a) => {
    const f = fake({ greeting: "SSH-1.5-Old\r\n" });
    await a.rejects(SSHClient.connect(f.opts), /only SSH-2\.0 is supported/);
    const g = fake({ greeting: "hello there\r\nbanner line\r\nSSH-2.0-Fake\r\n", version: "SSH-2.0-Fake" });
    const c = await SSHClient.connect(g.opts);
    a.eq(c.serverVersion, "SSH-2.0-Fake");
    c.close();
    const h = fake({ greeting: "x".repeat(70000), onGreeted: () => {} });
    await a.rejects(SSHClient.connect(h.opts), /no SSH identification line/);
});

t.test("handshake timeout", async (a) => {
    const f = fake({ greeting: "", onGreeted: () => {} });
    const t0 = Date.now();
    await a.rejects(SSHClient.connect({ ...f.opts, dyna: { handshakeTimeoutMs: 400 } }), /handshake timed out after 400 ms/);
    // wall-clock: a deadline against a hang; the 400 ms timer must fire long before the 3 s default, with 6x headroom for ASan
    a.ok(Date.now() - t0 < 2500, "timed out promptly");
});

t.test("authentication refused and banner", async (a) => {
    const f = fake({ authFail: true });
    await a.rejects(SSHClient.connect(f.opts), /was refused \(the server accepts: publickey\)/);
    await settle();
    a.eq(f.disc[0] && f.disc[0].code, 14, "DISCONNECT no more auth methods");
    const g = fake({ banner: "Welcome\n" });
    const c = await SSHClient.connect(g.opts);
    a.eq(c.banner, "Welcome\n");
    c.close();
    await settle();
    a.eq(g.disc[0] && g.disc[0].code, 11, "close() sends DISCONNECT by application");
});

t.test("server DISCONNECT is reported", async (a) => {
    const f = fake({ onPacket: (s, ty) => { if (ty === 20) { s.send(new W().u8(1).u32(7).str("go away").str("").done()); } return false; } });
    await a.rejects(SSHClient.connect(f.opts), /the server disconnected \(reason 7: go away\)/);
});

t.test("channel open and exec refused", async (a) => {
    const f = fake({ refuseChannel: true });
    const c = await SSHClient.connect(f.opts);
    await a.rejects(c.exec("x"), /refused the session channel \(code 1: nope\)/);
    a.eq(c.connected, true, "a refused channel leaves the connection usable");
    c.close();
    const g = fake({ onPacket: (s, ty, p) => {
        if (ty !== 98) return false;
        const r = new R(p, 1), id = r.u32();
        s.send(new W().u8(100).u32(s.chans[id].rid).done());
        s.send(new W().u8(97).u32(s.chans[id].rid).done());
        return true;
    } });
    const c2 = await SSHClient.connect(g.opts);
    await a.rejects(c2.exec("x"), /refused the exec request/);
    c2.close();
});

t.test("scripted exec over AES-GCM", async (a) => {
    const f = fake({ onExec: (s, ch) => {
        s.send(new W().u8(94).u32(ch.rid).str("out:" + ch.cmd).done());
        s.send(new W().u8(95).u32(ch.rid).u32(1).str("err").done());
        s.send(new W().u8(95).u32(ch.rid).u32(9).str("dropped").done());
        s.send(new W().u8(98).u32(ch.rid).str("keepalive@openssh.com").bool(true).done());
        s.send(new W().u8(98).u32(ch.rid).str("exit-signal").bool(false).str("KILL").bool(true).str("boom").str("").done());
        s.send(new W().u8(96).u32(ch.rid).done());
        s.send(new W().u8(97).u32(ch.rid).done());
    } });
    const c = await SSHClient.connect(f.opts);
    a.eq(c.algorithms.cipherServerToClient, "aes256-gcm@openssh.com");
    const x = await c.exec("cmd");
    const [o, e] = await Promise.all([readAll(x.stdout), readAll(x.stderr)]);
    a.eq(text(o), "out:cmd");
    a.eq(text(e), "err");
    a.deep(await x.exit, { exitCode: null, signal: "KILL", coreDumped: true, message: "boom", error: null });
    await settle();
    a.ok(f.sessions[0].log.includes(100), "keepalive answered with CHANNEL_FAILURE");
    c.close();
});

t.test("channel data beyond the window is fatal", async (a) => {
    const f = fake({ onExec: (s, ch) => { s.send(new W().u8(94).u32(ch.rid).str(new Uint8Array(40000)).done()); } });
    const c = await SSHClient.connect(f.opts);
    const x = await c.exec("big");
    const ex = await x.exit;
    a.match(ex.error, /exceeds the window .* or packet size \(32768\)/);
    a.eq(c.connected, false);
    c.close();
}, { timeoutMs: 20000 });

t.test("tampered AEAD tag is fatal (MAC error)", async (a) => {
    const f = fake({ onExec: (s, ch) => { const p = s.packet(new W().u8(94).u32(ch.rid).str("tampered").done()); p[p.length - 1] ^= 1; s.sendRaw(p); } });
    const c = await SSHClient.connect(f.opts);
    const x = await c.exec("t");
    const [o] = await Promise.all([readAll(x.stdout).catch(() => new Uint8Array(0)), readAll(x.stderr).catch(() => null)]);
    a.match((await x.exit).error, /packet authentication failed/);
    a.eq(text(o), "", "no tampered byte reached stdout");
    await settle();
    a.eq(f.disc[0] && f.disc[0].code, 5, "DISCONNECT MAC error");
    c.close();
}, { timeoutMs: 20000 });

t.test("sk-ssh-ed25519 host key: FIDO signature format verified", async (a) => {
    const f = fake({ hostKind: "sk", hostKeyAlgs: "sk-ssh-ed25519@openssh.com" });
    const c = await SSHClient.connect(f.opts);
    a.eq(c.algorithms.hostKey, "sk-ssh-ed25519@openssh.com");
    a.eq(c.hostKey.type, "sk-ssh-ed25519@openssh.com");
    c.close();
    const g = fake({ hostKind: "sk", hostKeyAlgs: "sk-ssh-ed25519@openssh.com", badSig: true });
    await a.rejects(SSHClient.connect(g.opts), /host key signature does not verify/);
});

t.test("process teardown with a live exec or a pending connect does not abort", async (a) => {
    const bin = std.getenv("BIN") || "./dynajs";
    for (const [mode, rc] of [["exec-pending-throw", 1], ["exec-pending-exit", 3], ["connect-pending-throw", 1]]) {
        const r = sys.Exec("/bin/sh", ["-c", bin + " --std tests/ssh/ssh_teardown_child.js " + mode + " </dev/null 2>&1"]);
        a.eq(r.code, rc, mode + " exit code: " + r.stdout.slice(-300));
        a.ok(!/Assertion failed|AddressSanitizer|runtime error/.test(r.stdout), mode + " teardown clean");
    }
}, { timeoutMs: 60000 });

await t.run();
