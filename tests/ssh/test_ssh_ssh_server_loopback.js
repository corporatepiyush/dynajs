// flags: --std
// covers: dyna:ssh.SSHServer -- this module's client against this module's server on 127.0.0.1: auth (authorizedKeys, authorize hook sync/async, certificates and every refusal), sessions, flow control, re-key, ext-info, caps
// timeout: 180
// Keys and certificates are made by ssh-keygen at run time under TMPDIR. Covered: exec sessions with stdout/stderr/exit status/exit signal,
// stdin through session.stdin, async onSession auto-exit (0 on fulfil, 1 + stderr on reject or throw), the "signal" request (exec.signal ->
// session.onsignal), byte-exact 8 MiB both ways under small windows (window grants), server- and client-initiated re-key by bytes (also on the output path alone: a one-way 8 MiB burst with no peer input and rekeyBytes
// 65536 re-keys at least 80 times, which allows the at-most-one-packet overshoot of about 32 KiB per epoch and fails at two) with
// byte-exact content, EXT_INFO server-sig-algs, authorizedKeys refusal, the authorize hook (sync true/false/throw, Promise true/false,
// what it receives; it never sees, and cannot override, a certificate that failed a check), maxAuthTries, user certificates (accepted, wrong principal, expired, not yet valid, untrusted CA, host cert used as a
// user cert, force-command, source-address match and mismatch, verify-required refused), host certificates through @cert-authority
// (accepted, wrong principal, CA not trusted, @revoked CA), maxConnections, maxChannels, idleTimeoutMs, dyna.shrinkBuffers (stats.bufferBytes stays above 1 MiB after a 4 MiB burst by default and
// drops to at most 768 KiB when opted in), close() with live sessions, stats,
// and option validation. The suite SKIPs without ssh-keygen.
import { SSHClient, SSHServer } from "dyna:ssh";
import * as sys from "dyna:sys";
import { suite } from "../lib/kit.js";
import { HAVE_KEYGEN, tmpDir, keygen, write, pubBlob, run, text, readAll, sleep, certify } from "./ssh_util.js";

const t = suite("ssh_server_loopback");
const servers = [];
t.after(() => { for (const s of servers) { try { s.close(); } catch {} } });

function bytes(n, seed) { const u = new Uint8Array(n); let x = seed >>> 0 || 1; for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; u[i] = x >>> 24; } return u; }
function same(a, b) { if (a.byteLength !== b.byteLength) return false; for (let i = 0; i < a.byteLength; i++) if (a[i] !== b[i]) return false; return true; }

async function duplex(c, cmd, input) {
    const x = await c.exec(cmd);
    const outs = Promise.all([readAll(x.stdout), readAll(x.stderr)]);
    await x.write(input);
    x.end();
    const [stdout, stderr] = await outs;
    return { stdout, stderr, exit: await x.exit };
}

async function echoSession(s) {
    const cmd = s.command || "";
    if (cmd === "cat") {
        const r = s.stdin.getReader();
        for (;;) { const { value, done } = await r.read(); if (done) break; await s.write(value); }
        return;
    }
    if (cmd.startsWith("gen ")) { const n = +cmd.slice(4); await s.write(bytes(n, 7)); return; }
    if (cmd === "who") { await s.write(JSON.stringify({ user: s.user, type: s.type, command: s.command, requested: s.requestedCommand, keyType: s.keyType, cert: s.certificate && s.certificate.keyId, env: s.env, pty: s.pty, addr: s.remoteAddress }) + "\n"); return; }
    if (cmd === "sig") { s.onsignal = (n) => { s.write("got " + n + "\n").then(() => s.exit(42)); }; await s.write("ready\n"); return new Promise(() => {}); }
    if (cmd === "kill") { s.kill("SIGUSR1", "bye"); return; }
    if (cmd === "reject") throw new Error("handler failed");
    if (cmd === "sync-throw") { s.exit(9); return; }
    if (cmd === "hang") return new Promise(() => {});
    await s.write("out:" + cmd + "\n");
    await s.writeStderr("err:" + cmd + "\n");
    s.exit(cmd === "fail" ? 3 : 0);
}

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const dir = tmpDir("srvloop");
    const host = keygen(dir + "/host");
    const user = keygen(dir + "/user");
    const other = keygen(dir + "/other");
    const ca = keygen(dir + "/ca");
    const ca2 = keygen(dir + "/ca2");
    const uname = "alice";

    function listen(opts = {}) {
        const s = SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub + "\n", onSession: echoSession, ...opts });
        servers.push(s);
        return s;
    }
    function khFor(s, extra) { const p = dir + "/kh" + s.port + Math.floor(Math.random() * 1e6); write(p, extra !== undefined ? extra : "[127.0.0.1]:" + s.port + " " + pubBlob(host.pub) + "\n"); return p; }
    const base = (s, more = {}) => ({ host: "127.0.0.1", port: s.port, username: uname, privateKey: user.priv, knownHosts: khFor(s), ...more });

    t.test("exec: stdout, stderr, exit status, algorithms, ext-info", async (a) => {
        const s = listen();
        const c = await SSHClient.connect(base(s));
        a.deep(c.algorithms, { kex: "mlkem768x25519-sha256", hostKey: "ssh-ed25519", cipherClientToServer: "chacha20-poly1305@openssh.com", cipherServerToClient: "chacha20-poly1305@openssh.com", strictKex: true });
        a.deep(c.serverSignatureAlgorithms, ["ssh-ed25519", "sk-ssh-ed25519@openssh.com"]);
        a.match(c.serverVersion, /^SSH-2\.0-dynajs_/);
        const r = await run(c, "fail");
        a.eq(text(r.stdout), "out:fail\n");
        a.eq(text(r.stderr), "err:fail\n");
        a.eq(r.exit.exitCode, 3);
        const w = JSON.parse(text((await run(c, "who")).stdout));
        a.deep(w, { user: uname, type: "exec", command: "who", requested: "who", keyType: "ssh-ed25519", cert: null, env: {}, pty: null, addr: "127.0.0.1" });
        const st = s.stats;
        a.eq(st.connections, 1);
        a.eq(st.authenticated, 1);
        c.close();
        await sleep(50);
        a.eq(s.stats.connections, 0);
        a.eq(s.stats.accepted, 1);
    });

    t.test("exit-signal, async auto-exit, reject and throw paths", async (a) => {
        const s = listen();
        const c = await SSHClient.connect(base(s));
        const k = await run(c, "kill");
        a.eq(k.exit.exitCode, null);
        a.eq(k.exit.signal, "USR1");
        a.eq(k.exit.message, "bye");
        const g = await run(c, "gen 10");
        a.eq(g.exit.exitCode, 0, "fulfilled handler exits 0");
        const rj = await run(c, "reject");
        a.eq(rj.exit.exitCode, 1);
        a.match(text(rj.stderr), /handler failed/);
        const sy = await run(c, "sync-throw");
        a.eq(sy.exit.exitCode, 9, "an explicit exit wins over the auto-exit");
        c.close();
    });

    t.test("signal request reaches onsignal", async (a) => {
        const s = listen();
        const c = await SSHClient.connect(base(s));
        const x = await c.exec("sig");
        const rd = x.stdout.getReader();
        a.eq(text((await rd.read()).value), "ready\n");
        x.signal("SIGTERM");
        a.eq(text((await rd.read()).value), "got TERM\n");
        a.eq((await x.exit).exitCode, 42);
        a.throws(() => x.signal("bad name"), TypeError);
        c.close();
    });

    t.test("8 MiB both ways byte-exact under 64 KiB windows (window grants)", async (a) => {
        const s = listen({ dyna: { windowBytes: 65536 } });
        const c = await SSHClient.connect(base(s, { dyna: { windowBytes: 65536 } }));
        const data = bytes(8 << 20, 3);
        const r = await duplex(c, "cat", data);
        a.ok(same(r.stdout, data), "cat echoed 8 MiB byte-exact");
        const big = await run(c, "gen " + (8 << 20));
        a.ok(same(big.stdout, bytes(8 << 20, 7)), "server output byte-exact");
        c.close();
    }, { timeoutMs: 60000 });

    t.test("an unread session output stops at the client window", async (a) => {
        const s = listen();
        const c = await SSHClient.connect(base(s, { dyna: { windowBytes: 131072 } }));
        const x = await c.exec("gen " + (16 << 20));
        await sleep(300);
        a.ok(c.stats.bytesIn < 1 << 20, "held near the window: " + c.stats.bytesIn);
        const out = await readAll(x.stdout);
        a.eq(out.byteLength, 16 << 20);
        c.close();
    }, { timeoutMs: 60000 });

    t.test("re-key by bytes: server-initiated and client-initiated, byte-exact", async (a) => {
        const s = listen({ dyna: { rekeyBytes: 65536, windowBytes: 65536 } });
        const c = await SSHClient.connect(base(s, { dyna: { windowBytes: 65536 } }));
        const data = bytes(2 << 20, 11);
        const r = await duplex(c, "cat", data);
        a.ok(same(r.stdout, data), "byte-exact across server re-keys");
        a.ok(c.stats.rekeys >= 20, "server re-keys seen by the client: " + c.stats.rekeys);
        c.close();
        const s2 = listen({ dyna: { windowBytes: 65536 } });
        const c2 = await SSHClient.connect(base(s2, { dyna: { rekeyBytes: 65536, windowBytes: 65536 } }));
        const r2 = await duplex(c2, "cat", data);
        a.ok(same(r2.stdout, data), "byte-exact across client re-keys");
        a.ok(c2.stats.rekeys >= 20, "client re-keys: " + c2.stats.rekeys);
        c2.close();
    }, { timeoutMs: 60000 });

    t.test("re-key by bytes on the output path: a one-way 8 MiB burst re-keys about every rekeyBytes", async (a) => {
        const s = listen({ dyna: { rekeyBytes: 65536 } });
        const c = await SSHClient.connect(base(s, { dyna: { windowBytes: 16 << 20, rekeyBytes: 4294967296 } }));
        const r = await run(c, "gen " + (8 << 20));
        a.ok(same(r.stdout, bytes(8 << 20, 7)), "byte-exact");
        a.ok(c.stats.rekeys >= 80, "server re-keys during one write burst with no peer input: " + c.stats.rekeys);
        c.close();
    }, { timeoutMs: 60000 });

    t.test("dyna.shrinkBuffers: idle buffers above 256 KiB are freed only when opted in", async (a) => {
        const held = {};
        for (const shrink of [false, true]) {
            const s = listen({ dyna: { windowBytes: 16 << 20, shrinkBuffers: shrink } });
            const c = await SSHClient.connect(base(s, { dyna: { windowBytes: 16 << 20, shrinkBuffers: shrink } }));
            const data = bytes(4 << 20, 5);
            const r = await duplex(c, "cat", data);
            a.ok(same(r.stdout, data), "byte-exact, shrinkBuffers " + shrink);
            await sleep(50);
            held[shrink] = c.stats.bufferBytes;
            c.close();
        }
        a.ok(held[false] >= 1 << 20, "default keeps the grown capacity: " + held[false]);
        a.ok(held[true] <= 3 * 262144, "shrinkBuffers frees idle buffers above 256 KiB: " + held[true]);
    }, { timeoutMs: 60000 });

    t.test("authorizedKeys refusal and maxAuthTries", async (a) => {
        const s = listen();
        await a.rejects(SSHClient.connect(base(s, { privateKey: other.priv })), /publickey authentication of alice with the ssh-ed25519 key was refused/);
        const s1 = listen({ dyna: { maxAuthTries: 1 } });
        await a.rejects(SSHClient.connect(base(s1, { privateKey: other.priv })), /too many authentication failures for alice \(dyna\.maxAuthTries is 1\)/);
    });

    t.test("authorize hook: sync, async, throw, what it sees", async (a) => {
        const seen = [];
        let mode = "sync-true";
        const s = listen({
            authorizedKeys: undefined,
            authorize: (req) => {
                seen.push(req);
                if (mode === "sync-true") return true;
                if (mode === "sync-false") return false;
                if (mode === "throw") throw new Error("nope");
                if (mode === "truthy") return 1;
                return new Promise((res) => setTimeout(() => res(mode === "async-true"), 30));
            },
        });
        for (const m of ["sync-true", "async-true"]) {
            mode = m;
            const c = await SSHClient.connect(base(s));
            a.eq(text((await run(c, "x")).stdout), "out:x\n", m);
            c.close();
        }
        for (const m of ["sync-false", "async-false", "throw", "truthy"]) {
            mode = m;
            await a.rejects(SSHClient.connect(base(s)), /was refused/, m);
        }
        const q = seen[0];
        a.eq(q.username, uname);
        a.eq(q.keyType, "ssh-ed25519");
        a.eq(q.publicKey, pubBlob(user.pub));
        a.match(q.fingerprint, /^SHA256:/);
        a.eq(q.listed, false);
        a.eq(q.certificate, null);
        a.eq(q.remoteAddress, "127.0.0.1");
        const fp = sys.Exec("ssh-keygen", ["-l", "-f", dir + "/user.pub"]).stdout.split(" ")[1];
        a.eq(q.fingerprint, fp);
        const s2 = listen({ authorize: (req) => req.listed });
        const c2 = await SSHClient.connect(base(s2));
        c2.close();
        await a.rejects(SSHClient.connect(base(s2, { privateKey: other.priv })), /was refused/);
    });

    t.test("user certificates: accepted and every refusal", async (a) => {
        const s = listen({ authorizedKeys: undefined, trustedUserCAKeys: ca.pub + "\n" });
        const cert = (name, opts) => { const d = dir + "/" + name; sys.Exec("cp", [dir + "/user", d]); sys.Exec("cp", [dir + "/user.pub", d + ".pub"]); return certify(dir, dir + "/" + (opts.ca || "ca"), d + ".pub", opts); };
        const good = cert("u_good", { principals: uname + ",bob", id: "good-id" });
        const c = await SSHClient.connect(base(s, { certificate: good }));
        const w = JSON.parse(text((await run(c, "who")).stdout));
        a.eq(w.keyType, "ssh-ed25519-cert-v01@openssh.com");
        a.eq(w.cert, "good-id");
        c.close();
        await a.rejects(SSHClient.connect(base(s)), /was refused/, "the bare key is not enough without authorizedKeys");
        const refuse = async (name, opts, why) => {
            const cl = cert(name, opts);
            await a.rejects(SSHClient.connect(base(s, { certificate: cl })), /publickey authentication of alice with the ssh-ed25519-cert-v01@openssh.com key was refused/, why);
        };
        await refuse("u_princ", { principals: "mallory" }, "wrong principal");
        await refuse("u_expired", { principals: uname, validity: "20200101:20200102" }, "expired");
        await refuse("u_future", { principals: uname, validity: "+1d:+2d" }, "not yet valid");
        await refuse("u_ca2", { principals: uname, ca: "ca2" }, "untrusted CA");
        await refuse("u_src", { principals: uname, options: ["source-address=10.0.0.0/8"] }, "source-address mismatch");
        await refuse("u_vr", { principals: uname, options: ["verify-required"] }, "verify-required is not supported");
        const hostish = cert("u_host", { principals: uname, host: true });
        a.throws(() => SSHClient.connect(base(s, { certificate: hostish })), /not a user certificate for privateKey/);
        a.throws(() => SSHClient.connect(base(s, { certificate: pubBlob(user.pub) })), /must be an ssh-ed25519-cert-v01@openssh.com public key line/);
        const src = cert("u_src_ok", { principals: uname, options: ["source-address=10.1.0.0/16,127.0.0.0/8"] });
        const c2 = await SSHClient.connect(base(s, { certificate: src }));
        c2.close();
        const fc = cert("u_force", { principals: uname, options: ["force-command=who"] });
        const c3 = await SSHClient.connect(base(s, { certificate: fc }));
        const w3 = JSON.parse(text((await run(c3, "something else")).stdout));
        a.eq(w3.command, "who");
        a.eq(w3.requested, "something else");
        c3.close();
        const seen = [];
        const sh = listen({ authorizedKeys: undefined, trustedUserCAKeys: ca.pub, authorize: (r) => { seen.push(r); return r.listed; } });
        const c4 = await SSHClient.connect(base(sh, { certificate: good }));
        c4.close();
        a.eq(seen[0].certificate.keyId, "good-id");
        a.deep(seen[0].certificate.principals, [uname, "bob"]);
        a.eq(seen[0].certificate.type, "user");
        a.eq(seen[0].listed, true);
    });

    t.test("the authorize hook cannot override certificate checks", async (a) => {
        let calls = 0;
        const s = listen({ authorizedKeys: undefined, trustedUserCAKeys: ca.pub, authorize: () => { calls++; return true; } });
        const mk = (name, opts) => { sys.Exec("cp", [dir + "/user.pub", dir + "/" + name + ".pub"]); return certify(dir, dir + "/" + (opts.ca || "ca"), dir + "/" + name + ".pub", opts); };
        await a.rejects(SSHClient.connect(base(s, { certificate: mk("h_exp", { principals: uname, validity: "20200101:20200102" }) })), /was refused/, "expired");
        await a.rejects(SSHClient.connect(base(s, { certificate: mk("h_ca2", { principals: uname, ca: "ca2" }) })), /was refused/, "untrusted CA");
        await a.rejects(SSHClient.connect(base(s, { certificate: mk("h_pr", { principals: "zed" }) })), /was refused/, "wrong principal");
        a.eq(calls, 0, "the hook never saw a failing certificate");
        const c = await SSHClient.connect(base(s, { certificate: mk("h_ok", { principals: uname }) }));
        c.close();
        a.eq(calls, 1);
        const p = await SSHClient.connect(base(s));
        p.close();
        a.eq(calls, 2, "a plain key still goes to the hook");
    });

    t.test("host certificates through @cert-authority", async (a) => {
        sys.Exec("cp", [dir + "/host.pub", dir + "/hc.pub"]);
        const hc = certify(dir, dir + "/ca", dir + "/hc.pub", { principals: "127.0.0.1", host: true, id: "host-id" });
        const s = listen({ hostCertificate: hc });
        const kh = khFor(s, "@cert-authority [127.0.0.1]:* " + pubBlob(ca.pub) + "\n");
        const c = await SSHClient.connect(base(s, { knownHosts: kh }));
        a.eq(c.algorithms.hostKey, "ssh-ed25519-cert-v01@openssh.com");
        a.eq(c.hostKey.type, "ssh-ed25519-cert-v01@openssh.com");
        a.eq(text((await run(c, "x")).stdout), "out:x\n");
        c.close();
        const plain = await SSHClient.connect(base(s));
        a.eq(plain.algorithms.hostKey, "ssh-ed25519", "without a CA line the plain key is negotiated");
        plain.close();
        await a.rejects(SSHClient.connect(base(s, { knownHosts: khFor(s, "@cert-authority [127.0.0.1]:* " + pubBlob(ca2.pub) + "\n@cert-authority [127.0.0.1]:" + s.port + " " + pubBlob(ca2.pub) + "\n") })), /which no @cert-authority line .* trusts/);
        await a.rejects(SSHClient.connect(base(s, { knownHosts: khFor(s, "@cert-authority [127.0.0.1]:* " + pubBlob(ca.pub) + "\n@revoked * " + pubBlob(ca.pub) + "\n") })), /@revoked/);
        sys.Exec("cp", [dir + "/host.pub", dir + "/hc2.pub"]);
        const wrong = certify(dir, dir + "/ca", dir + "/hc2.pub", { principals: "elsewhere.example", host: true });
        const s2 = listen({ hostCertificate: wrong });
        await a.rejects(SSHClient.connect(base(s2, { knownHosts: khFor(s2, "@cert-authority * " + pubBlob(ca.pub) + "\n") })), /host certificate refused: the certificate does not list the principal "127\.0\.0\.1"/);
        a.throws(() => listen({ hostCertificate: certify(dir, dir + "/ca", (sys.Exec("cp", [dir + "/user.pub", dir + "/uc.pub"]), dir + "/uc.pub"), { principals: "x", host: true }) }), /not a host certificate for hostKey/);
    });

    t.test("caps: maxConnections, maxChannels, idleTimeoutMs", async (a) => {
        const s = listen({ dyna: { maxConnections: 1, maxChannels: 2, idleTimeoutMs: 400 } });
        const c = await SSHClient.connect(base(s));
        await a.rejects(SSHClient.connect(base(s)), /closed the connection|disconnected|reset by peer/);
        a.eq(s.stats.refused, 1);
        const x1 = await c.exec("hang");
        const x2 = await c.exec("hang");
        await a.rejects(c.exec("x"), /refused the session channel \(code 4: too many channels/);
        x1.close();
        x2.close();
        await x1.exit;
        await x2.exit;
        await sleep(900);
        a.eq(c.connected, false, "idle connection dropped");
        c.close();
    });

    t.test("close() ends live sessions", async (a) => {
        const s = listen();
        let sess;
        const s2 = listen({ onSession: (x) => { sess = x; return new Promise(() => {}); } });
        const c = await SSHClient.connect(base(s2));
        const x = await c.exec("hang");
        await sleep(30);
        s2.close();
        const d = await sess.done;
        a.match(d.error, /the server was closed/);
        const ex = await x.exit;
        a.match(ex.error, /disconnected|closed/);
        a.eq(s2.closed, true);
        c.close();
        s.close();
    });

    t.test("option validation", async (a) => {
        a.throws(() => new SSHServer(), /use SSHServer.listen/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, onSession: () => {} }), /nobody can log in/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub }), /onSession/);
        a.throws(() => SSHServer.listen({ hostKey: "garbage", authorizedKeys: user.pub, onSession: () => {} }), /hostKey/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: "from=\"x\" " + user.pub, onSession: () => {} }), /authorizedKeys: line 1/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, bogus: 1 }), TypeError);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, dyna: { maxAuthTries: 0 } }), RangeError);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, dyna: { nope: 1 } }), TypeError);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, dyna: { shrinkBuffers: 1 } }), /shrinkBuffers must be a boolean/);
        a.throws(() => SSHClient.connect({ host: "127.0.0.1", username: "u", privateKey: user.priv, dyna: { shrinkBuffers: "yes" } }), /shrinkBuffers must be a boolean/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, port: 70000 }), RangeError);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: 5 }), /must be a function/);
        a.throws(() => SSHServer.listen({ hostKey: host.priv, authorizedKeys: user.pub, onSession: () => {}, hostCertificate: "ssh-ed25519 AAAA" }), /hostCertificate/);
    });
}

await t.run();
