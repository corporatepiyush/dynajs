// flags: --std
// covers: dyna:ssh.SSHClient -- malformed and truncated packets, fuzzed in the cleartext phase and after NEWKEYS (encrypted with the negotiated AES-GCM keys so the post-KEX parsers are reached); run it under ASan+UBSan
// timeout: 240
// Seeded (SSH_FUZZ_SEED, default 1) so a failure replays. Cleartext phase: mutated KEXINIT payloads (byte flips, truncation,
// growth), random bytes after the identification line, random packet_length/padding fields, and random KEX replies; each
// connect must reject with an "SSH: " error (or, rarely, connect when a KEXINIT mutation hit only the cookie or an unused list), never hang or crash. Encrypted phase: after auth and an exec, the server sends random
// message types with random bodies, channel messages with random ids and lengths, and packets with a flipped ciphertext/tag byte;
// the exec's exit must settle and the client stay memory-safe. Iterations: SSH_FUZZ_N (default 150 per phase).
import { SSHClient } from "dyna:ssh";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { FakeSSH, W } from "./fake_server.js";
import { tmpDir, keygen, write, readAll } from "./ssh_util.js";

const t = suite("ssh_fuzz");
const N = Number(std.getenv("SSH_FUZZ_N") || 150);
let seed = Number(std.getenv("SSH_FUZZ_SEED") || 1) >>> 0;
const rnd = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296; };
const ri = (n) => Math.floor(rnd() * n);
const bytes = (n) => { const b = new Uint8Array(n); for (let i = 0; i < n; i++) b[i] = ri(256); return b; };

const dir = tmpDir("fuzz");
const user = keygen(dir + "/user");
const f = new FakeSSH({});
write(dir + "/kh", f.knownHostsLine());
const opts = { host: "127.0.0.1", port: f.port, username: "u", privateKey: user.priv, knownHosts: dir + "/kh", dyna: { handshakeTimeoutMs: 2000 } };
t.after(() => f.close());
const base = Object.assign({}, f.o);
const reset = () => { for (const k of Object.keys(f.o)) delete f.o[k]; Object.assign(f.o, base); f.o.onError = () => {}; };

function mutate(u8) {
    const b = Array.from(u8);
    const k = 1 + ri(6);
    for (let i = 0; i < k; i++) {
        const op = ri(4);
        if (op === 0 && b.length) b[ri(b.length)] ^= 1 << ri(8);
        else if (op === 1 && b.length) b.length = ri(b.length);
        else if (op === 2) b.splice(ri(b.length + 1), 0, ...bytes(ri(16)));
        else if (b.length) b[ri(b.length)] = [0, 255, 0x7f, 0x80][ri(4)];
    }
    return Uint8Array.from(b);
}

function cleartextCase(i) {
    reset();
    const kind = i % 4;
    if (kind === 0) f.o.onGreeted = (s) => { s.is = mutate(s.kexinitPayload()); s.send(s.is.length ? s.is : Uint8Array.of(20)); };
    else if (kind === 1) f.o.onGreeted = (s) => { s.sendRaw(bytes(ri(300))); s.end(); };
    else if (kind === 2) f.o.onGreeted = (s) => {
        const p = s.packet(s.kexinitPayload());
        const v = mutate(p.subarray(0, 5 + ri(3)));
        p.set(v.subarray(0, Math.min(v.length, p.length)), 0);
        s.sendRaw(p.subarray(0, ri(p.length + 1)));
        s.end();
    };
    else f.o.onPacket = (s, ty) => {
        if (ty !== 30) return false;
        const w = new W().u8(31);
        w.str(ri(2) ? s.srv.hostBlob : bytes(ri(80))).str(bytes([0, 31, 32, 33, 1120][ri(5)])).str(mutate(new W().str("ssh-ed25519").str(bytes(64)).done()));
        s.send(ri(4) ? w.done() : mutate(w.done()));
        s.end();
        return true;
    };
}

t.test("cleartext phase", async (a) => {
    let rejected = 0, benign = 0;
    for (let i = 0; i < N; i++) {
        cleartextCase(i);
        try {
            const c = await SSHClient.connect(opts);
            c.close();
            if (i % 4 === 0) benign++;
            else a.ok(false, "case " + i + " connected");
        } catch (e) {
            if (/^SSH: /.test(e.message)) rejected++;
            else a.ok(false, "case " + i + ": " + e.message);
        }
    }
    a.eq(rejected + benign, N, "every mutated handshake rejected with an SSH: error, or (a KEXINIT mutation that hit only the cookie or an unused name-list) connected");
    a.ok(benign < N / 10, "benign KEXINIT mutations stay rare: " + benign);
});

function encryptedScript(s, ch) {
    const k = 1 + ri(12);
    for (let j = 0; j < k && !s.closed; j++) {
        const mode = ri(5);
        if (mode === 0) {
            const types = [1, 2, 3, 4, 5, 6, 7, 20, 21, 30, 31, 50, 51, 52, 53, 60, 80, 81, 82, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 192, 255];
            s.send(new W().u8(types[ri(types.length)]).raw(bytes(ri(64))).done());
        } else if (mode === 1) {
            const ty = 91 + ri(10);
            const w = new W().u8(ty).u32(ri(3) ? ch.rid : ri(5) ? ri(70) : 0xffffffff);
            if (ty === 94 || ty === 95) { if (ty === 95) w.u32(ri(3)); w.u32(ri(3) ? ri(2000) : 0xfffffff0).raw(bytes(ri(200))); }
            else if (ty === 98) w.str(["exit-status", "exit-signal", "x"][ri(3)]).bool(ri(2)).raw(bytes(ri(40)));
            else w.raw(bytes(ri(24)));
            s.send(w.done());
        } else if (mode === 2) {
            const p = s.packet(new W().u8(94).u32(ch.rid).str("data").done());
            p[ri(p.length)] ^= 1 << ri(8);
            s.sendRaw(p);
        } else if (mode === 3) {
            s.send(new W().u8(94).u32(ch.rid).str(bytes(ri(3000))).done());
        } else {
            const p = s.packet(new W().u8(2).str(bytes(ri(40))).done());
            s.sendRaw(p.subarray(0, ri(p.length)));
            s.end();
        }
    }
    if (!s.closed && ri(2)) { s.send(new W().u8(96).u32(ch.rid).done()); s.send(new W().u8(97).u32(ch.rid).done()); }
    setTimeout(() => s.end(), 5);
}

t.test("encrypted phase", async (a) => {
    let settled = 0, errors = 0;
    for (let i = 0; i < N; i++) {
        reset();
        f.o.onExec = encryptedScript;
        const c = await SSHClient.connect(opts);
        try {
            const x = await c.exec("fuzz" + i);
            const drain = (rs) => readAll(rs).catch(() => null);
            await Promise.all([drain(x.stdout), drain(x.stderr)]);
            const ex = await x.exit;
            settled++;
            if (ex.error) errors++;
        } catch (e) {
            a.match(e.message, /^SSH: /, "case " + i);
            settled++;
        }
        c.close();
    }
    a.eq(settled, N, "every fuzzed channel settled");
    a.ok(errors > N / 4, "most cases ended in a protocol error: " + errors);
});

await t.run();
