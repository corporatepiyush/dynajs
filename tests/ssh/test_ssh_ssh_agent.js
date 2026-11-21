// flags: --std
// covers: integration dyna:ssh.SSHAgent dyna:ssh.SSHClient -- this module's agent and agent-backed client against this module's server and against raw and scripted agent peers
// timeout: 180
// Keys are made by ssh-keygen and dyna:crypto at run time under TMPDIR; every socket lives in a private temp dir. SSHAgent: option validation,
// the socket (mode 0600, an existing path refused instead of replaced, a path too long for sun_path refused, unlinked on close), add/list/
// remove/removeAll/stats, certificates, lifetimes, maxKeys and maxConnections. The wire protocol through a raw client: identities, signatures that
// verify, FAILURE for unknown messages, EXTENSION, the confirm constraint, unknown constraints, trailing bytes and a private half that does not
// match its public key; pipelined requests answered in order while signatures and the lock KDF run on the worker pool (owner rule: crypto
// off the event loop; measured 34 us per ed25519 SIGN and 13 ms per lock KDF against a 3-5 us pool handoff); lock/unlock (a locked agent lists and signs nothing and refuses add/remove); sk-ssh-ed25519 keys are listed but never
// signed for (owner decision: no FIDO device access in the agent); framing (zero and oversized lengths close the connection, split and coalesced
// frames, the connection cap) and 400 random frames that never crash it. SSHClient with `agent`: several keys tried in order (query first, sign
// only after PK_OK), privateKey first and the agent after it (the duplicate skipped), a certificate held by the agent, the "refused for every key"
// error, option validation, an unreachable socket, and a scripted agent that signs sk keys, sends a malformed answer, closes mid-sign, hangs
// (handshakeTimeoutMs ends it), sends two replies, signs with the wrong algorithm, or refuses one key (the next is tried). No agent forwarding:
// against a scripted server the client refuses an auth-agent@openssh.com open and sends no auth-agent-req. Teardown runs in child
// processes under LeakSanitizer when BIN is the sanitizer build. The suite SKIPs without ssh-keygen.
import { SSHClient, SSHServer, SSHAgent } from "dyna:ssh";
import { Ed25519Generate, Ed25519Verify, RandomBytes } from "dyna:crypto";
import * as sys from "dyna:sys";
import * as os from "os";
import { suite } from "../lib/kit.js";
import { HAVE_KEYGEN, tmpDir, keygen, write, pubBlob, run, text, sleep, certify } from "./ssh_util.js";
import { authLine } from "./fake_client.js";
import { W, R, FakeSSH } from "./fake_server.js";
import { AG, frame, cat, edBlob, skBlob, addEd, addSk, rawAgent, skSign, edSign, scriptedAgent, idsAnswer, sockDir } from "./agent_util.js";

const t = suite("ssh_agent");
const owned = [];
const keep = (x) => { owned.push(x); return x; };
t.after(() => { for (const x of owned) { try { x.close(); } catch {} } });

const exists = (p) => { const [, err] = os.stat(p); return err === 0; };

if (!HAVE_KEYGEN) t.skip("ssh-keygen", "no ssh-keygen on this host");
else {
    const dir = tmpDir("agent");
    const host = keygen(dir + "/host");
    const u1 = keygen(dir + "/u1");
    const u2 = keygen(dir + "/u2");
    const u3 = keygen(dir + "/u3");
    const ca = keygen(dir + "/ca");
    const ucert = certify(dir, dir + "/ca", dir + "/u3.pub", { principals: "carol" });
    const hcert = certify(dir, dir + "/ca", dir + "/u3.pub", { principals: "h", host: true });
    const sdir = sockDir(dir);
    let seq = 0;
    const sock = () => sdir + "/a" + (seq++) + ".sock";
    const agent = (o = {}) => keep(SSHAgent.listen({ path: sock(), ...o }));
    const server = (o = {}) => keep(SSHServer.listen({ hostKey: host.priv, onSession: async (x) => { await x.write(x.user + ":" + x.keyType + "\n"); x.exit(0); }, ...o }));
    const kh = (s) => { const p = dir + "/kh" + s.port; write(p, "[127.0.0.1]:" + s.port + " " + pubBlob(host.pub) + "\n"); return p; };
    const opts = (s, more) => ({ host: "127.0.0.1", port: s.port, username: "carol", knownHosts: kh(s), ...more });
    const who = async (c) => text((await run(c, "x")).stdout).trim();

    t.test("listen validates its options and never leaves a socket behind on a refusal", (a) => {
        a.throws(() => new SSHAgent(), TypeError);
        a.throws(() => SSHAgent.listen(), TypeError);
        a.throws(() => SSHAgent.listen({}), /options.path/);
        a.throws(() => SSHAgent.listen({ path: sock(), bogus: 1 }), TypeError);
        a.throws(() => SSHAgent.listen({ path: sock(), keys: {} }), /array/);
        a.throws(() => SSHAgent.listen({ path: sock(), dyna: { maxKeys: 0 } }), RangeError);
        a.throws(() => SSHAgent.listen({ path: sock(), dyna: { nope: 1 } }), TypeError);
        const p = sock();
        a.throws(() => SSHAgent.listen({ path: p, keys: [{ privateKey: "garbage" }] }), /SSHAgent.add/);
        a.ok(!exists(p), "a refused key leaves no socket");
        a.throws(() => SSHAgent.listen({ path: p, keys: [{ privateKey: u1.priv, lifetimeSeconds: 0 }] }), RangeError);
    });

    t.test("socket: mode 0600, an existing path refused, a too-long path refused, unlinked on close", (a) => {
        const g = agent();
        const [st] = os.stat(g.path);
        a.eq(st.mode & 0o777, 0o600, "socket mode");
        a.throws(() => SSHAgent.listen({ path: g.path }), /already exists/);
        write(dir + "/plain", "x");
        a.throws(() => SSHAgent.listen({ path: dir + "/plain" }), /already exists/);
        a.eq(std.loadFile(dir + "/plain"), "x", "an existing file is never replaced");
        const long = dir + "/" + "l".repeat(120) + ".sock";
        a.throws(() => SSHAgent.listen({ path: long }), /at most \d+ fit/);
        a.throws(() => SSHClient.connect({ host: "127.0.0.1", username: "x", agent: long }), /agent/);
        const p = g.path;
        g.close();
        a.ok(g.closed, "closed");
        a.ok(!exists(p), "the socket is unlinked on close");
        g.close();
    });

    t.test("add, list, remove, removeAll, stats, certificates, lifetimes and maxKeys", (a) => {
        const g = agent({ keys: [{ privateKey: u1.priv, comment: "one" }], dyna: { maxKeys: 3 } });
        const l = g.list();
        a.eq(l.length, 1);
        a.deep([l[0].type, l[0].publicKey, l[0].comment, l[0].expiresAt, l[0].canSign], ["ssh-ed25519", pubBlob(u1.pub), "one", null, true]);
        a.match(l[0].fingerprint, /^SHA256:[A-Za-z0-9+/]{43}$/);
        const id = g.add({ privateKey: u1.priv, comment: "renamed", lifetimeSeconds: 60 });
        a.eq(id.comment, "renamed", "a second add of the same key replaces its comment");
        a.ok(id.expiresAt > Date.now() + 50000 && id.expiresAt <= Date.now() + 60000, "expiresAt");
        a.eq(g.list().length, 1);
        const c = g.add({ privateKey: u3.priv, certificate: ucert });
        a.eq(c.type, "ssh-ed25519-cert-v01@openssh.com");
        a.throws(() => g.add({ privateKey: u2.priv, certificate: ucert }), /certificate/);
        a.throws(() => g.add({ privateKey: u3.priv, certificate: hcert }), /user certificate/, "a host certificate");
        a.throws(() => g.add({ privateKey: u2.priv, bogus: 1 }), TypeError);
        a.throws(() => g.add(), TypeError);
        g.add({ privateKey: u2.priv });
        a.throws(() => g.add({ privateKey: keygen(dir + "/u9").priv }), RangeError, "maxKeys");
        a.eq(g.stats.keys, 3);
        a.eq(g.remove(pubBlob(u2.pub)), true);
        a.eq(g.remove(pubBlob(u2.pub)), false);
        a.throws(() => g.remove("nonsense"), TypeError);
        g.removeAll();
        a.eq(g.list().length, 0);
        a.deep(Object.keys(g.stats).sort(), ["accepted", "connections", "keys", "offloaded", "refused", "refusedConnections", "requests", "signatures"]);
    });

    t.test("wire: identities, signatures that verify, and FAILURE for everything it does not do", async (a) => {
        const kp = Ed25519Generate();
        const g = agent({ keys: [{ privateKey: u1.priv }] });
        const r = await rawAgent(g.path);
        a.eq((await r.req(addEd(kp, "raw")))[0], AG.SUCCESS);
        const ids = await r.req(Uint8Array.of(AG.REQ_IDS));
        const rd = new R(ids, 1);
        a.eq(ids[0], AG.IDS);
        a.eq(rd.u32(), 2);
        const data = RandomBytes(100);
        const sr = await r.req(new W().u8(AG.SIGN).str(edBlob(kp.publicKey)).str(data).u32(0).done());
        a.eq(sr[0], AG.SIGN_RESP);
        const sig = new R(new R(sr, 1).str());
        a.eq(sig.cstr(), "ssh-ed25519");
        a.ok(Ed25519Verify(kp.publicKey, data, sig.str()), "the signature verifies");
        a.eq((await r.req(new W().u8(AG.SIGN).str(edBlob(Ed25519Generate().publicKey)).str(data).u32(0).done()))[0], AG.FAILURE, "unknown key");
        const sreq = frame(new W().u8(AG.SIGN).str(edBlob(kp.publicKey)).str(data).u32(0).done());
        r.send(cat(sreq, frame(Uint8Array.of(AG.REQ_IDS)), sreq, frame(new W().u8(AG.LOCK).str("p").done()), frame(Uint8Array.of(AG.REQ_IDS)), frame(new W().u8(AG.UNLOCK).str("p").done())));
        const order = [];
        for (let i = 0; i < 6; i++) { const m = await r.next(); order.push(m[0] === AG.IDS ? "ids" + new R(m, 1).u32() : m[0]); }
        a.deep(order, [AG.SIGN_RESP, "ids2", AG.SIGN_RESP, AG.SUCCESS, "ids0", AG.SUCCESS], "pipelined replies keep request order while signatures and the lock KDF run on the worker pool");
        a.eq((await r.req(Uint8Array.of(99)))[0], AG.FAILURE, "unknown message");
        a.eq((await r.req(new W().u8(AG.EXT).str("session-bind@openssh.com").done()))[0], AG.FAILURE, "EXTENSION");
        a.eq((await r.req(cat(addEd(Ed25519Generate(), "c", AG.ADD_C), Uint8Array.of(2))))[0], AG.FAILURE, "confirm constraint refused");
        a.eq((await r.req(cat(addEd(Ed25519Generate(), "c", AG.ADD_C), Uint8Array.of(9))))[0], AG.FAILURE, "unknown constraint");
        a.eq((await r.req(cat(addEd(Ed25519Generate(), "c", AG.ADD_C), new W().u8(255).str("sk-provider@openssh.com").str("x").done())))[0], AG.FAILURE, "sk-provider on an ed25519 key");
        a.eq((await r.req(cat(addEd(Ed25519Generate()), Uint8Array.of(0))))[0], AG.FAILURE, "trailing bytes");
        const bad = Ed25519Generate();
        a.eq((await r.req(new W().u8(AG.ADD).str("ssh-ed25519").str(bad.publicKey).str(cat(Ed25519Generate().privateKey, bad.publicKey)).str("x").done()))[0], AG.FAILURE, "mismatched private half");
        a.eq((await r.req(new W().u8(AG.ADD).str("ssh-rsa").str("x").done()))[0], AG.FAILURE, "rsa");
        const life = Ed25519Generate();
        a.eq((await r.req(cat(addEd(life, "life", AG.ADD_C), new W().u8(1).u32(1).done())))[0], AG.SUCCESS, "lifetime 1 s");
        a.ok(g.list().some((k) => k.comment === "life" && k.expiresAt !== null));
        await sleep(1100);
        a.ok(!g.list().some((k) => k.comment === "life"), "expired after its lifetime");
        a.eq((await r.req(new W().u8(AG.REMOVE).str(edBlob(kp.publicKey)).done()))[0], AG.SUCCESS);
        a.eq((await r.req(new W().u8(AG.REMOVE).str(edBlob(kp.publicKey)).done()))[0], AG.FAILURE, "already removed");
        a.eq((await r.req(Uint8Array.of(AG.REMOVE_ALL)))[0], AG.SUCCESS);
        a.eq(g.list().length, 0);
        a.ok(g.stats.signatures >= 1 && g.stats.refused >= 1 && g.stats.requests >= 14, JSON.stringify(g.stats));
        a.eq(g.stats.offloaded, 5, "every signature and the lock KDF ran on the worker pool, not the event loop");
        r.close();
    });

    t.test("lock: a locked agent lists and signs nothing and refuses changes until the right passphrase", async (a) => {
        const kp = Ed25519Generate();
        const g = agent({ keys: [{ privateKey: u1.priv }] });
        const r = await rawAgent(g.path);
        a.eq((await r.req(new W().u8(AG.LOCK).str("pw").done()))[0], AG.SUCCESS);
        a.eq(g.locked, true);
        a.eq((await r.req(new W().u8(AG.LOCK).str("pw").done()))[0], AG.FAILURE, "lock twice");
        a.deep(Array.from(await r.req(Uint8Array.of(AG.REQ_IDS))), [AG.IDS, 0, 0, 0, 0]);
        a.eq(g.list().length, 0, "list() is empty while locked");
        a.eq((await r.req(new W().u8(AG.SIGN).str(new R(pubWire(u1.pub)).str()).str("d").u32(0).done()))[0], AG.FAILURE, "sign while locked");
        a.eq((await r.req(addEd(kp)))[0], AG.FAILURE, "add while locked");
        a.eq((await r.req(Uint8Array.of(AG.REMOVE_ALL)))[0], AG.FAILURE, "remove-all while locked");
        a.throws(() => g.add({ privateKey: u2.priv }), /locked/);
        a.throws(() => g.removeAll(), /locked/);
        a.eq((await r.req(new W().u8(AG.UNLOCK).str("wrong").done()))[0], AG.FAILURE);
        a.eq((await r.req(new W().u8(AG.UNLOCK).str("pw").done()))[0], AG.SUCCESS);
        a.eq((await r.req(new W().u8(AG.UNLOCK).str("pw").done()))[0], AG.FAILURE, "unlock when not locked");
        a.eq(g.locked, false);
        a.eq(g.list().length, 1, "the keys are back");
        r.close();
    });

    t.test("sk-ssh-ed25519 keys are held and listed but never signed for", async (a) => {
        const sk = Ed25519Generate();
        const g = agent();
        const r = await rawAgent(g.path);
        a.eq((await r.req(addSk(sk.publicKey)))[0], AG.SUCCESS);
        a.eq((await r.req(cat(addSk(Ed25519Generate().publicKey, "c", AG.ADD_C), new W().u8(255).str("sk-provider@openssh.com").str("internal").done())))[0], AG.SUCCESS, "sk-provider accepted on an sk key");
        a.eq((await r.req(new W().u8(AG.ADD).str("sk-ssh-ed25519@openssh.com").str(sk.publicKey).str("web:").u8(1).str("h").str("").str("x").done()))[0], AG.FAILURE, "application not ssh:");
        const l = g.list();
        a.eq(l.length, 2);
        a.ok(l.every((k) => k.type === "sk-ssh-ed25519@openssh.com" && k.canSign === false), JSON.stringify(l));
        a.eq((await r.req(new W().u8(AG.SIGN).str(skBlob(sk.publicKey)).str("d").u32(0).done()))[0], AG.FAILURE);
        r.close();
    });

    t.test("framing: zero and oversized lengths close, split and coalesced frames work, maxConnections caps", async (a) => {
        const g = agent({ keys: [{ privateKey: u1.priv }], dyna: { maxConnections: 2 } });
        const r1 = await rawAgent(g.path);
        const req = frame(Uint8Array.of(AG.REQ_IDS));
        for (const b of req) r1.send(Uint8Array.of(b));
        a.eq((await r1.next())[0], AG.IDS, "byte-by-byte");
        r1.send(cat(req, req, req));
        a.eq((await r1.next())[0], AG.IDS);
        a.eq((await r1.next())[0], AG.IDS);
        a.eq((await r1.next())[0], AG.IDS, "three coalesced");
        const r2 = await rawAgent(g.path);
        const r3 = await rawAgent(g.path);
        a.eq(await Promise.race([r3.next(), sleep(2000).then(() => "still open")]), null, "the third connection is closed");
        a.eq(g.stats.refusedConnections, 1);
        a.eq(g.stats.connections, 2);
        r2.send(Uint8Array.of(0, 0, 0, 0));
        a.eq(await r2.next(), null, "zero length closes");
        r1.send(new W().u32((256 << 10) + 1).done());
        a.eq(await r1.next(), null, "an oversized length closes");
        await sleep(30);
        a.eq(g.stats.connections, 0);
        const r4 = await rawAgent(g.path);
        a.eq((await r4.req(Uint8Array.of(AG.REQ_IDS)))[0], AG.IDS, "still serving");
        r4.close();
    }, { timeoutMs: 15000 });

    t.test("400 random frames never crash it and every reply is FAILURE, SUCCESS or a well-formed answer", async (a) => {
        const g = agent({ keys: [{ privateKey: u1.priv }] });
        const types = [AG.SIGN, AG.ADD, AG.ADD_C, AG.REMOVE, AG.UNLOCK, AG.EXT, AG.REQ_IDS, 0, 255];
        let r = await rawAgent(g.path);
        let odd = 0, reconnects = 0;
        for (let i = 0; i < 400; i++) {
            const body = cat(Uint8Array.of(types[i % types.length]), RandomBytes(i % 37 === 0 ? 3000 : (i * 13) % 90));
            const rep = await r.req(body);
            if (rep === null) { reconnects++; r = await rawAgent(g.path); continue; }
            if (![AG.FAILURE, AG.SUCCESS, AG.IDS, AG.SIGN_RESP].includes(rep[0])) odd++;
        }
        a.eq(odd, 0, "unexpected reply types");
        a.eq(reconnects, 0, "a well-framed message never closes the connection");
        a.eq((await r.req(Uint8Array.of(AG.REQ_IDS)))[0], AG.IDS);
        r.close();
    }, { timeoutMs: 60000 });

    t.test("client: the agent's keys are tried in order, querying before signing", async (a) => {
        const g = agent({ keys: [{ privateKey: u1.priv }, { privateKey: u2.priv }] });
        const s = server({ authorizedKeys: u1.pub + "\n" });
        a.deep(g.list().map((k) => k.publicKey), [pubBlob(u1.pub), pubBlob(u2.pub)], "listed in the order added");
        const s2 = server({ authorizedKeys: u2.pub + "\n" });
        const c = await SSHClient.connect(opts(s2, { agent: g.path }));
        a.eq(await who(c), "carol:ssh-ed25519");
        c.close();
        a.eq(g.stats.signatures, 1, "one signature: the refused first key was only queried");
        const c1 = await SSHClient.connect(opts(s, { agent: g.path }));
        c1.close();
        a.eq(g.stats.signatures, 2);
    });

    t.test("client: privateKey first, then the agent without the duplicate; the refused-everything error", async (a) => {
        const g = agent({ keys: [{ privateKey: u1.priv }, { privateKey: u2.priv }] });
        const s = server({ authorizedKeys: u2.pub + "\n" });
        const c = await SSHClient.connect(opts(s, { privateKey: u1.priv, agent: g.path }));
        a.eq(await who(c), "carol:ssh-ed25519");
        c.close();
        const s2 = server({ authorizedKeys: u3.pub + "\n" });
        await a.rejects(SSHClient.connect(opts(s2, { privateKey: u1.priv, agent: g.path })), /refused for every key tried \(2 tried: the privateKey and 1 agent identities\)/);
        await a.rejects(SSHClient.connect(opts(s2, { agent: g.path })), /refused for every key tried \(2 tried: 2 agent identities\)/);
        const empty = agent();
        await a.rejects(SSHClient.connect(opts(s2, { agent: empty.path })), /0 tried/);
        await a.rejects(SSHClient.connect(opts(s2, { privateKey: u2.priv })), /with the ssh-ed25519 key was refused/, "no agent: the old single-key error");
    });

    t.test("client: a certificate held by the agent", async (a) => {
        const g = agent({ keys: [{ privateKey: u3.priv, certificate: ucert }] });
        const s = server({ trustedUserCAKeys: ca.pub + "\n" });
        const c = await SSHClient.connect(opts(s, { agent: g.path }));
        a.eq(await who(c), "carol:ssh-ed25519-cert-v01@openssh.com");
        c.close();
    });

    t.test("client: option validation and an unreachable agent", async (a) => {
        const s = server({ authorizedKeys: u1.pub + "\n" });
        a.throws(() => SSHClient.connect(opts(s, {})), /privateKey .* or options.agent is required/);
        a.throws(() => SSHClient.connect(opts(s, { agent: 5 })), TypeError);
        a.throws(() => SSHClient.connect(opts(s, { agent: "" })), TypeError);
        a.throws(() => SSHClient.connect(opts(s, { agent: sock(), passphrase: "x" })), TypeError);
        a.throws(() => SSHClient.connect(opts(s, { agent: sock(), certificate: ucert })), TypeError);
        const saved = std.getenv("SSH_AUTH_SOCK");
        std.unsetenv("SSH_AUTH_SOCK");
        a.throws(() => SSHClient.connect(opts(s, { agent: true })), /SSH_AUTH_SOCK/);
        const g = agent({ keys: [{ privateKey: u1.priv }] });
        std.setenv("SSH_AUTH_SOCK", g.path);
        const c = await SSHClient.connect(opts(s, { agent: true }));
        a.eq(await who(c), "carol:ssh-ed25519", "agent: true uses SSH_AUTH_SOCK");
        c.close();
        if (saved === undefined) std.unsetenv("SSH_AUTH_SOCK"); else std.setenv("SSH_AUTH_SOCK", saved);
        await a.rejects(SSHClient.connect(opts(s, { agent: sdir + "/missing.sock" })), /agent at .* cannot be reached/);
        const c2 = await SSHClient.connect(opts(s, { privateKey: u1.priv, agent: sdir + "/missing.sock" }));
        a.eq(await who(c2), "carol:ssh-ed25519", "the agent is only reached when privateKey was refused");
        c2.close();
    });

    t.test("client: an sk-ssh-ed25519 key signed by a scripted agent", async (a) => {
        const sk = Ed25519Generate();
        for (const [flags, ok] of [[1, true], [0, false]]) {
            const p = sock();
            const sa = keep(scriptedAgent(p, (m, r) => {
                if (m[0] === AG.REQ_IDS) return idsAnswer([[skBlob(sk.publicKey), "yubi"]]);
                if (m[0] === AG.SIGN) { r.str(); return cat(Uint8Array.of(AG.SIGN_RESP), new W().str(skSign(sk.privateKey, "ssh:", r.str(), flags)).done()); }
                return Uint8Array.of(AG.FAILURE);
            }));
            const s = server({ authorizedKeys: authLine(sk.publicKey, true) + "\n" });
            if (ok) {
                const c = await SSHClient.connect(opts(s, { agent: p }));
                a.eq(await who(c), "carol:sk-ssh-ed25519@openssh.com");
                c.close();
            } else await a.rejects(SSHClient.connect(opts(s, { agent: p })), /refused for every key tried/, "no user presence");
            a.deep(sa.seen, [AG.REQ_IDS, AG.SIGN]);
        }
    });

    t.test("client: misbehaving agents fail the connection with a named reason, a refused signature moves on", async (a) => {
        const kp = Ed25519Generate();
        const s = server({ authorizedKeys: authLine(kp.publicKey) + "\n" });
        const ids = idsAnswer([[edBlob(Ed25519Generate().publicKey), "other"], [edBlob(kp.publicKey), "good"]]);
        const good = (m, r) => { r.str(); return cat(Uint8Array.of(AG.SIGN_RESP), new W().str(edSign(kp.privateKey, r.str())).done()); };
        const cases = [
            ["malformed identities", () => cat(ids, Uint8Array.of(0)), null, /malformed identities answer/],
            ["closes mid-sign", () => ids, () => "close", /agent at .* closed the connection/],
            ["two replies", () => ids, () => ({ raw: cat(frame(Uint8Array.of(AG.FAILURE)), frame(Uint8Array.of(AG.FAILURE))) }), /more than one reply|unsolicited reply/],
            ["wrong algorithm", () => ids, (m, r) => { r.str(); return cat(Uint8Array.of(AG.SIGN_RESP), new W().str(new W().str("ssh-rsa").str(new Uint8Array(64)).done()).done()); }, /unexpected algorithm/],
            ["oversized reply", () => ({ raw: new W().u32(1 << 20).done() }), null, /oversized reply/],
            ["a bad signature", () => ids, (m, r) => { r.str(); return cat(Uint8Array.of(AG.SIGN_RESP), new W().str(edSign(Ed25519Generate().privateKey, r.str())).done()); }, /refused for every key tried/],
        ];
        for (const [name, onIds, onSign, re] of cases) {
            const p = sock();
            keep(scriptedAgent(p, (m, r) => (m[0] === AG.REQ_IDS ? onIds() : onSign(m, r))));
            await a.rejects(SSHClient.connect(opts(s, { agent: p, dyna: { handshakeTimeoutMs: 5000 } })), re, name);
        }
        const p = sock();
        let first = true;
        keep(scriptedAgent(p, (m, r) => {
            if (m[0] === AG.REQ_IDS) return idsAnswer([[edBlob(kp.publicKey), "good"]]);
            if (first) { first = false; return Uint8Array.of(AG.FAILURE); }
            return good(m, r);
        }));
        await a.rejects(SSHClient.connect(opts(s, { agent: p })), /refused for every key tried \(1 tried/, "a refused signature is not retried");
        const p2 = sock();
        keep(scriptedAgent(p2, (m) => (m[0] === AG.REQ_IDS ? idsAnswer([[edBlob(kp.publicKey), "x"]]) : null)));
        const t0 = Date.now();
        await a.rejects(SSHClient.connect(opts(s, { agent: p2, dyna: { handshakeTimeoutMs: 700 } })), /time|handshake/i, "a hung agent ends at handshakeTimeoutMs");
        a.ok(Date.now() - t0 >= 600, "waited for the deadline, not an instant failure: " + (Date.now() - t0));
    }, { timeoutMs: 60000 });

    t.test("no agent forwarding: the client refuses an auth-agent@openssh.com open and never asks for forwarding", async (a) => {
        const reqs = [];
        let refusal = null;
        const g = agent({ keys: [{ privateKey: u1.priv }] });
        const f = keep(new FakeSSH({ onPacket: (s, ty, p) => {
            const r = new R(p, 1);
            if (ty === 50) { s.send(new W().u8(52).done()); s.send(new W().u8(90).str("auth-agent@openssh.com").u32(77).u32(65536).u32(32768).done()); return true; }
            if (ty === 92) { refusal = { rid: r.u32(), code: r.u32() }; return true; }
            if (ty === 98) { r.u32(); reqs.push(r.cstr()); }
            return false;
        }, onExec: (s, ch) => { s.send(new W().u8(96).u32(ch.rid).done()); s.send(new W().u8(98).u32(ch.rid).str("exit-status").bool(false).u32(0).done()); s.send(new W().u8(97).u32(ch.rid).done()); } }));
        write(dir + "/khf", f.knownHostsLine());
        const c = await SSHClient.connect({ host: "127.0.0.1", port: f.port, username: "carol", agent: g.path, knownHosts: dir + "/khf" });
        await run(c, "x");
        c.close();
        a.deep(refusal, { rid: 77, code: 1 }, "CHANNEL_OPEN_FAILURE administratively prohibited");
        a.deep(reqs, ["exec"], "no auth-agent-req@openssh.com");
    });

    t.test("teardown: closing with requests in flight and exiting with live agents (LeakSanitizer when BIN is the sanitizer build)", (a) => {
        const bin = std.getenv("BIN") || "./dynajs";
        for (const mode of ["close-busy", "exit-live"]) {
            const p = sdir + "/c-" + mode + ".sock";
            const r = sys.Exec("/bin/sh", ["-c", "LSAN_OPTIONS=suppressions=tests/asan_suppressions.txt ASAN_OPTIONS=detect_leaks=1 " + bin + " --std tests/ssh/ssh_agent_child.js " + mode + " " + p + " </dev/null 2>&1"], { timeoutMs: 60000 });
            a.eq(r.code, 0, mode + " exit code: " + r.stdout.slice(-600));
            a.ok(/agent-ran/.test(r.stdout), mode + " ran: " + r.stdout.slice(-300));
            a.ok(!/LeakSanitizer|AddressSanitizer|runtime error/.test(r.stdout), mode + ": no sanitizer report: " + r.stdout.slice(0, 600));
            a.ok(!exists(p), mode + ": the socket is gone");
        }
    }, { timeoutMs: 120000 });
}

function pubWire(line) {
    const b = atob(line.trim().split(/\s+/)[1]);
    const u = new Uint8Array(b.length);
    for (let i = 0; i < b.length; i++) u[i] = b.charCodeAt(i);
    return new W().str(u).done();
}

await t.run();
