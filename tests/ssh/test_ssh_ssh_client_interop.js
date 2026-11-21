// flags: --std
// covers: dyna:ssh.SSHClient -- interop against the system OpenSSH sshd (private instance, temp config, ed25519 only, KEX/ciphers restricted to the allowed set)
// timeout: 300
// Every KEX x cipher pair, exec streams and exit status/signal, stdin, a 64 MiB output, flow control (an unread stream stops the
// transfer at the window), 64 parallel channels and the maxChannels cap, client- and server-initiated re-key mid-transfer with
// byte-exact content, encrypted keys (bcrypt-pbkdf + aes256-ctr / aes256-gcm@openssh.com), refused authentication, close() with
// channels open, an rsa-sha2-512 host key (refused by default, accepted under dyna.allowRsaSha2_512), a real-sshd chacha20-poly1305
// stream with one byte flipped by a TCP proxy (fatal MAC error), and option validation. The suite SKIPs when /usr/sbin/sshd or ssh-keygen is missing or sshd is older than 9.9. sntrup761x25519-sha512 (and its @openssh.com alias) against sshd proves this module's own sntrup761 key generation and decapsulation against the OpenSSH reference.
import { SSHClient } from "dyna:ssh";
import * as sys from "dyna:sys";
import { suite } from "../lib/kit.js";
import { HAVE_SSHD, SKIP_WHY, startSshd, keygen, run, text, readAll, sleep, write, tamperProxy } from "./ssh_util.js";

const t = suite("ssh_interop");
const servers = [];
async function sshd(opts) { const d = await startSshd(opts); servers.push(d); return d; }
t.after(() => { for (const d of servers) d.close(); });

const KEX = ["mlkem768x25519-sha256", "sntrup761x25519-sha512", "curve25519-sha256"];
const CIPHERS = ["chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com"];

function seqText(n) { let s = ""; for (let i = 1; i <= n; i++) s += i + "\n"; return s; }

if (!HAVE_SSHD) t.skip("sshd", SKIP_WHY);
else {
    let main;
    t.before(async () => { main = await sshd({ tag: "interop" }); });

    for (const kex of KEX) for (const cip of CIPHERS) {
        t.test("handshake " + kex + " " + cip, async (a) => {
            const d = await sshd({ tag: "m", kex, ciphers: cip });
            const c = await SSHClient.connect(d.base);
            try {
                a.deep(c.algorithms, { kex, hostKey: "ssh-ed25519", cipherClientToServer: cip, cipherServerToClient: cip, strictKex: true });
                const r = await run(c, "echo hello; echo oops >&2; exit 3");
                a.eq(text(r.stdout), "hello\n");
                a.eq(text(r.stderr), "oops\n");
                a.eq(r.exit.exitCode, 3);
                a.eq(r.exit.error, null);
            } finally { c.close(); d.close(); }
        });
    }

    t.test("sntrup761x25519-sha512@openssh.com alias, repeated handshakes and re-keys", async (a) => {
        const d = await sshd({ tag: "ntru", kex: "sntrup761x25519-sha512@openssh.com" });
        let total = 0;
        for (let i = 0; i < 8; i++) {
            const c = await SSHClient.connect({ ...d.base, dyna: { rekeyBytes: 65536, windowBytes: 65536 } });
            a.eq(c.algorithms.kex, "sntrup761x25519-sha512");
            const r = await run(c, "seq 1 60000");
            a.ok(text(r.stdout) === seqText(60000), "byte-exact across sntrup re-keys");
            a.ok(c.stats.rekeys >= 1, "rekeys " + c.stats.rekeys);
            total += c.stats.rekeys;
            c.close();
        }
        a.ok(total >= 8, "sntrup re-keys in total: " + total);
        d.close();
    });

    t.test("host key fingerprint and server version", async (a) => {
        const c = await SSHClient.connect(main.base);
        const fp = sys.Exec("ssh-keygen", ["-l", "-f", main.dir + "/host.pub"]).stdout.split(" ")[1];
        a.eq(c.hostKey.fingerprint, fp);
        a.eq(c.hostKey.type, "ssh-ed25519");
        a.match(c.serverVersion, /^SSH-2\.0-OpenSSH_/);
        a.eq(c.connected, true);
        c.close();
        a.eq(c.closed, true);
    });

    t.test("exit-signal", async (a) => {
        const c = await SSHClient.connect(main.base);
        const r = await run(c, "kill -TERM $$");
        a.eq(r.exit.exitCode, null);
        a.eq(r.exit.signal, "TERM");
        c.close();
    });

    t.test("stdin through write and end", async (a) => {
        const c = await SSHClient.connect(main.base);
        const payload = seqText(150000);
        const r = await run(c, "cat", payload);
        a.eq(r.stdout.byteLength, payload.length);
        a.ok(text(r.stdout) === payload, "cat echoed stdin byte-exact");
        const w = await run(c, "wc -c", new Uint8Array(3 << 20).fill(7));
        a.eq(text(w.stdout).trim(), String(3 << 20));
        c.close();
    });

    t.test("64 MiB output and content integrity", async (a) => {
        const c = await SSHClient.connect(main.base);
        const x = await c.exec("head -c 67108864 /dev/zero");
        const r = x.stdout.getReader();
        let n = 0, nonzero = 0;
        for (;;) {
            const { value, done } = await r.read();
            if (done) break;
            n += value.byteLength;
            if (value[0] | value[value.length - 1]) nonzero++;
        }
        a.eq(n, 67108864);
        a.eq(nonzero, 0);
        a.eq((await x.exit).exitCode, 0);
        const s = await run(c, "seq 1 400000");
        a.ok(text(s.stdout) === seqText(400000), "seq output byte-exact");
        c.close();
    });

    t.test("flow control holds an unread stream at the window", async (a) => {
        const c = await SSHClient.connect({ ...main.base, dyna: { windowBytes: 262144 } });
        const x = await c.exec("head -c 33554432 /dev/zero");
        await sleep(400);
        const held = c.stats.bytesIn;
        a.ok(held < 1 << 20, "bytes received before reading stay near the window: " + held);
        const out = await readAll(x.stdout);
        a.eq(out.byteLength, 33554432);
        a.ok(c.stats.bytesIn > 33554432, "the rest arrived once read");
        c.close();
    });

    t.test("64 parallel channels", async (a) => {
        const c = await SSHClient.connect(main.base);
        const res = await Promise.all(Array.from({ length: 64 }, (_, i) => run(c, "echo ch" + i + "; head -c " + (1000 * i) + " /dev/zero | wc -c")));
        for (let i = 0; i < 64; i++) a.eq(text(res[i].stdout), "ch" + i + "\n" + String(1000 * i).padStart(8) + "\n", "channel " + i);
        a.eq(c.stats.openChannels, 0);
        c.close();
    });

    t.test("maxChannels cap", async (a) => {
        const c = await SSHClient.connect({ ...main.base, dyna: { maxChannels: 3 } });
        const xs = [];
        for (let i = 0; i < 3; i++) xs.push(await c.exec("sleep 0.3"));
        await a.rejects(c.exec("true"), /too many open channels \(dyna.maxChannels is 3\)/);
        for (const x of xs) await x.exit;
        const r = await run(c, "echo again");
        a.eq(text(r.stdout), "again\n");
        c.close();
    });

    t.test("client re-key by bytes mid-transfer", async (a) => {
        const c = await SSHClient.connect({ ...main.base, dyna: { rekeyBytes: 262144 } });
        const r = await run(c, "seq 1 999999");
        a.ok(text(r.stdout) === seqText(999999), "6.9 MB byte-exact across re-keys");
        a.ok(c.stats.rekeys >= 10, "rekeys " + c.stats.rekeys);
        const before = c.stats.rekeys;
        c.rekey();
        const s = await run(c, "echo after");
        a.eq(text(s.stdout), "after\n");
        a.ok(c.stats.rekeys > before, "rekey() completed");
        c.close();
    });

    t.test("client re-key by time (dyna.rekeySeconds) on an idle-then-busy connection", async (a) => {
        const c = await SSHClient.connect({ ...main.base, dyna: { rekeySeconds: 1 } });
        a.eq(c.stats.rekeys, 0);
        const r = await run(c, "sleep 1.6; seq 1 20000");
        a.ok(text(r.stdout) === seqText(20000), "byte-exact output after the timed re-key");
        a.ok(c.stats.rekeys >= 1, "rekeys " + c.stats.rekeys);
        c.close();
    }, { timeoutMs: 20000 });

    t.test("server re-key (sshd RekeyLimit 1M) mid-transfer", async (a) => {
        const d = await sshd({ tag: "rk", extra: ["RekeyLimit 1M"] });
        const c = await SSHClient.connect(d.base);
        const payload = seqText(300000);
        const r = await run(c, "cat; seq 1 999999", payload);
        a.ok(text(r.stdout) === payload + seqText(999999), "both directions byte-exact");
        a.ok(c.stats.rekeys >= 4, "rekeys " + c.stats.rekeys);
        c.close();
        d.close();
    });

    t.test("encrypted keys", async (a) => {
        for (const cipher of [undefined, "aes256-gcm@openssh.com"]) {
            const k = keygen(main.dir + "/enc" + (cipher ? "gcm" : "ctr"), { pass: "s3cret", cipher, rounds: 8 });
            const d = await sshd({ tag: "enc", extraAuth: [k.pub] });
            a.match(k.priv, /OPENSSH PRIVATE KEY/);
            const c = await SSHClient.connect({ ...d.base, privateKey: k.priv, passphrase: "s3cret" });
            a.eq(text((await run(c, "echo enc")).stdout), "enc\n", "key " + (cipher || "aes256-ctr"));
            c.close();
            a.throws(() => SSHClient.connect({ ...d.base, privateKey: k.priv, passphrase: "wrong" }), /wrong passphrase/);
            a.throws(() => SSHClient.connect({ ...d.base, privateKey: k.priv }), /encrypted: pass `passphrase`/);
            d.close();
        }
    });

    t.test("refused authentication", async (a) => {
        const k = keygen(main.dir + "/stranger");
        await a.rejects(SSHClient.connect({ ...main.base, privateKey: k.priv }), /publickey authentication of .* was refused/);
    });

    t.test("close with channels open", async (a) => {
        const c = await SSHClient.connect(main.base);
        const x = await c.exec("sleep 5");
        c.close();
        const ex = await x.exit;
        a.eq(ex.exitCode, null);
        a.match(ex.error, /the client was closed/);
        await a.rejects(readAll(x.stdout), /the client was closed/);
        a.throws(() => c.exec("true"), /closed native resource/);
    });

    t.test("rsa-sha2-512 host key: refused by default, accepted with dyna.allowRsaSha2_512", async (a) => {
        const d = await sshd({ tag: "rsa", hostType: "rsa", hostAlgs: "rsa-sha2-512" });
        await a.rejects(SSHClient.connect(d.base), /no common host key algorithm/);
        const c = await SSHClient.connect({ ...d.base, dyna: { allowRsaSha2_512: true } });
        a.eq(c.algorithms.hostKey, "rsa-sha2-512");
        a.eq(c.hostKey.type, "ssh-rsa");
        a.eq(text((await run(c, "echo rsa")).stdout), "rsa\n");
        c.close();
    });

    t.test("tampered chacha20-poly1305 packet from a real sshd is fatal (MAC error)", async (a) => {
        const d = await sshd({ tag: "tamper", ciphers: "chacha20-poly1305@openssh.com" });
        const px = tamperProxy(d.port, 300000);
        try {
            write(d.dir + "/kh_proxy", "[127.0.0.1]:" + px.port + " " + d.host.pub.split(/\s+/).slice(0, 2).join(" ") + "\n");
            const c = await SSHClient.connect({ ...d.base, port: px.port, knownHosts: d.dir + "/kh_proxy" });
            const x = await c.exec("head -c 4000000 /dev/zero");
            const [o] = await Promise.all([readAll(x.stdout).catch(() => null), readAll(x.stderr).catch(() => null)]);
            a.match((await x.exit).error, /packet authentication failed/);
            a.ok(px.flipped(), "the proxy flipped one byte at offset 300000");
            a.ok(o === null || o.byteLength < 4000000, "the transfer stopped at the forged packet");
            a.eq(c.connected, false);
            c.close();
        } finally { px.close(); }
    }, { timeoutMs: 30000 });

    t.test("option validation", async (a) => {
        a.throws(() => new SSHClient(), /use SSHClient.connect/);
        a.throws(() => SSHClient.connect({ ...main.base, bogus: 1 }), TypeError);
        a.throws(() => SSHClient.connect({ ...main.base, dyna: { nope: 1 } }), TypeError);
        a.throws(() => SSHClient.connect({ ...main.base, dyna: { windowBytes: 10 } }), RangeError);
        a.throws(() => SSHClient.connect({ ...main.base, dyna: { maxChannels: 1.5 } }), RangeError);
        a.throws(() => SSHClient.connect({ ...main.base, dyna: { hostKeyPolicy: "yes" } }), TypeError);
        a.throws(() => SSHClient.connect({ ...main.base, port: 70000 }), RangeError);
        a.throws(() => SSHClient.connect({ ...main.base, privateKey: "garbage" }), /not an OpenSSH private key/);
        a.throws(() => SSHClient.connect({ ...main.base, username: undefined }), /username is required/);
        write(main.dir + "/rsa.txt", "");
        const rsa = sys.Exec("ssh-keygen", ["-q", "-t", "rsa", "-b", "2048", "-N", "", "-f", main.dir + "/rsakey"]);
        a.eq(rsa.code, 0);
        a.throws(() => SSHClient.connect({ ...main.base, privateKey: sys.Exec("cat", [main.dir + "/rsakey"]).stdout }), /only ssh-ed25519/);
    });
}

await t.run();
