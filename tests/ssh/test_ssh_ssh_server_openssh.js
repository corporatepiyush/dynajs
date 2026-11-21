// flags: --std
// covers: dyna:ssh.SSHServer -- interop with the system OpenSSH ssh client (>= 9.9): every KEX x cipher, user and host certificates, pty/env, stdin, exit codes, refusals
// timeout: 240
// The OpenSSH client runs as a child (-F /dev/null, BatchMode, IdentitiesOnly, a private known_hosts) against SSHServer on 127.0.0.1.
// Every KEX (mlkem768x25519-sha256, sntrup761x25519-sha512 and its @openssh.com alias, curve25519-sha256) x cipher pair runs a command and
// checks stdout, stderr and the exit code; sntrup761 here exercises this module's encapsulation against OpenSSH's decapsulation. Also:
// 2 MiB of stdin echoed byte-exact, exit-signal (ssh exits 255), pty-req with -tt (term and size) and SetEnv (only LANG/LC_* accepted),
// a user certificate (TrustedUserCAKeys-style trustedUserCAKeys, force-command, no-pty refusing the pty, an untrusted CA refused), a host
// certificate verified by ssh through @cert-authority, maxAuthTries reached by a client offering a wrong key, and the banner. The suite SKIPs without an OpenSSH >= 9.9 ssh client or ssh-keygen.
import { SSHServer } from "dyna:ssh";
import * as sys from "dyna:sys";
import { suite } from "../lib/kit.js";
import { HAVE_SSH, SKIP_WHY_SSH, tmpDir, keygen, write, pubBlob, text, sshCli, certify } from "./ssh_util.js";

const t = suite("ssh_server_openssh");
const servers = [];
t.after(() => { for (const s of servers) { try { s.close(); } catch {} } });

const KEX = ["mlkem768x25519-sha256", "sntrup761x25519-sha512", "sntrup761x25519-sha512@openssh.com", "curve25519-sha256"];
const CIPHERS = ["chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com"];

async function session(s) {
    const cmd = s.command || "";
    if (cmd === "cat") {
        const r = s.stdin.getReader();
        for (;;) { const { value, done } = await r.read(); if (done) break; await s.write(value); }
        return;
    }
    if (cmd === "info") { await s.write(JSON.stringify({ type: s.type, command: s.command, requested: s.requestedCommand, env: s.env, pty: s.pty, cert: s.certificate && s.certificate.keyId, keyType: s.keyType }) + "\n"); return; }
    if (cmd === "kill") { s.kill("TERM"); return; }
    await s.write("hello " + cmd + "\n");
    await s.writeStderr("warn\n");
    s.exit(cmd === "seven" ? 7 : 0);
}

if (!HAVE_SSH) t.skip("ssh", SKIP_WHY_SSH);
else {
    const dir = tmpDir("srvossh");
    const host = keygen(dir + "/host");
    const user = keygen(dir + "/user");
    const other = keygen(dir + "/other");
    const ca = keygen(dir + "/ca");
    const ca2 = keygen(dir + "/ca2");
    const uname = "carol";

    function listen(opts = {}) {
        const s = SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: session, ...opts });
        servers.push(s);
        return s;
    }
    function kh(s, line) { const p = dir + "/kh" + s.port + "_" + Math.floor(Math.random() * 1e6); write(p, (line || "[127.0.0.1]:" + s.port + " " + pubBlob(host.pub)) + "\n"); return p; }
    function args(s, opts = {}) {
        const a = ["-F", "/dev/null", "-p", String(s.port), "-i", opts.key || dir + "/user", "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
            "-o", "UserKnownHostsFile=" + (opts.kh || kh(s)), "-o", "GlobalKnownHostsFile=/dev/null", "-o", "StrictHostKeyChecking=yes",
            "-o", "LogLevel=" + (opts.log || "ERROR"), "-o", "ConnectTimeout=10"];
        if (opts.kex) a.push("-o", "KexAlgorithms=" + opts.kex);
        if (opts.cipher) a.push("-o", "Ciphers=" + opts.cipher);
        for (const o of opts.extra || []) a.push("-o", o);
        if (opts.tt) a.push("-tt");
        a.push(uname + "@127.0.0.1");
        if (opts.cmd !== undefined) a.push(opts.cmd);
        return a;
    }

    for (const kex of KEX) for (const cip of CIPHERS) {
        t.test("OpenSSH client: " + kex + " " + cip, async (a) => {
            const s = listen();
            const r = await sshCli(args(s, { kex, cipher: cip, cmd: "seven" }));
            a.eq(r.stderr, "warn\n");
            a.eq(text(r.stdout), "hello seven\n");
            a.eq(r.code, 7);
        });
    }

    t.test("stdin echoed byte-exact, exit-signal", async (a) => {
        const s = listen();
        const data = new Uint8Array(2 << 20);
        for (let i = 0; i < data.length; i++) data[i] = (i * 31 + (i >> 9)) & 255;
        const r = await sshCli(args(s, { cmd: "cat" }), { input: data });
        a.eq(r.code, 0, r.stderr);
        a.eq(r.stdout.byteLength, data.length);
        let ok = true;
        for (let i = 0; i < data.length && ok; i++) ok = r.stdout[i] === data[i];
        a.ok(ok, "byte-exact");
        const k = await sshCli(args(s, { cmd: "kill" }));
        a.eq(k.code, 255, "ssh exits 255 on exit-signal");
    });

    t.test("pty-req and env (LANG/LC_* only)", async (a) => {
        const s = listen();
        const r = await sshCli(args(s, { tt: true, cmd: "info", extra: ["SetEnv=LANG=C.UTF-8 LC_CTYPE=C FOO=bar"] }));
        const j = JSON.parse(text(r.stdout).trim());
        a.eq(j.pty.term.length > 0, true);
        a.deep(j.env, { LANG: "C.UTF-8", LC_CTYPE: "C" });
        const n = await sshCli(args(s, { cmd: "info" }));
        a.eq(JSON.parse(text(n.stdout)).pty, null);
    });

    t.test("user certificates", async (a) => {
        const s = listen({ authorizedKeys: undefined, trustedUserCAKeys: ca.pub });
        const mk = (name, opts) => { sys.Exec("cp", [dir + "/user", dir + "/" + name]); sys.Exec("cp", [dir + "/user.pub", dir + "/" + name + ".pub"]); sys.Exec("chmod", ["600", dir + "/" + name]); certify(dir, dir + "/" + (opts.ca || "ca"), dir + "/" + name + ".pub", opts); return dir + "/" + name; };
        const good = mk("cu_good", { principals: uname, id: "cert-1" });
        const r = await sshCli(args(s, { key: good, cmd: "info", extra: ["CertificateFile=" + good + "-cert.pub"] }));
        a.eq(r.code, 0, r.stderr);
        const j = JSON.parse(text(r.stdout));
        a.eq(j.cert, "cert-1");
        a.eq(j.keyType, "ssh-ed25519-cert-v01@openssh.com");
        const fc = mk("cu_force", { principals: uname, options: ["force-command=info"], ...{ id: "forced" } });
        const fr = await sshCli(args(s, { key: fc, cmd: "whatever", extra: ["CertificateFile=" + fc + "-cert.pub"] }));
        a.eq(fr.code, 0, fr.stderr);
        const f = JSON.parse(text(fr.stdout));
        a.eq(f.command, "info");
        a.eq(f.requested, "whatever");
        const np = mk("cu_nopty", { principals: uname, options: ["no-pty"] });
        const p = await sshCli(args(s, { key: np, tt: true, cmd: "info", extra: ["CertificateFile=" + np + "-cert.pub"] }));
        a.match(p.stderr, /PTY allocation request failed/, "no-pty certificate: pty-req refused (ssh -tt then closes the session)");
        const p2 = await sshCli(args(s, { key: np, cmd: "info", extra: ["CertificateFile=" + np + "-cert.pub"] }));
        a.eq(JSON.parse(text(p2.stdout)).pty, null, "the same certificate works without a pty");
        const bad = mk("cu_ca2", { principals: uname, ca: "ca2" });
        const b = await sshCli(args(s, { key: bad, cmd: "info", extra: ["CertificateFile=" + bad + "-cert.pub"] }));
        a.eq(b.code, 255);
        a.match(b.stderr, /Permission denied \(publickey\)/);
    });

    t.test("host certificate verified by ssh via @cert-authority", async (a) => {
        sys.Exec("cp", [dir + "/host.pub", dir + "/hc.pub"]);
        const hc = certify(dir, dir + "/ca", dir + "/hc.pub", { principals: "127.0.0.1", host: true });
        const s = listen({ hostCertificate: hc });
        const line = "@cert-authority [127.0.0.1]:" + s.port + " " + pubBlob(ca.pub);
        const r = await sshCli(args(s, { kh: kh(s, line), cmd: "x", extra: ["HostKeyAlgorithms=ssh-ed25519-cert-v01@openssh.com"] }));
        a.eq(r.code, 0, r.stderr);
        a.eq(text(r.stdout), "hello x\n");
        const w = await sshCli(args(s, { kh: kh(s, "@cert-authority [127.0.0.1]:" + s.port + " " + pubBlob(ca2.pub)), cmd: "x", extra: ["HostKeyAlgorithms=ssh-ed25519-cert-v01@openssh.com"] }));
        a.eq(w.code, 255, "a CA ssh does not trust is refused by ssh");
    });

    t.test("refusals: wrong key, maxAuthTries, banner", async (a) => {
        const s = listen({ banner: "welcome to dyna\n", dyna: { maxAuthTries: 1 } });
        const r = await sshCli(args(s, { key: dir + "/other", cmd: "x", log: "INFO" }));
        a.eq(r.code, 255);
        a.match(r.stderr, /too many authentication failures|Permission denied/i);
        a.match(r.stderr, /welcome to dyna/);
        const ok = await sshCli(args(s, { cmd: "x" }));
        a.eq(ok.code, 0, ok.stderr);
        a.eq(text(ok.stdout), "hello x\n");
    });
}

await t.run();
