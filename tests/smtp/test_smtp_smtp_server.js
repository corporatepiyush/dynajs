// flags: --std
// covers: dyna:smtp.SmtpServer
// test_smtp_smtp_server.js -- dyna:smtp SmtpServer driven by our SmtpClient, raw TCP peers, a TCP proxy
// and swaks. S9 S2b, 2026-11-27.
// Oracles: RFC 5321 (sequencing, 4.5.2 transparency, bare LF refused 554 5.6.0, 421 on timeout/shutdown,
// 4.5.3.1.10 too many connections), RFC 2920 PIPELINING, RFC 3030 CHUNKING/BDAT + BINARYMIME (NUL bytes
// survive), RFC 3207 (plaintext pipelined after STARTTLS is discarded, proven with an injecting proxy),
// RFC 4954 + 5802/7677 (PLAIN, SCRAM-SHA-1/256/512 with creds from onAuth; wrong password fails), RFC 3461
// DSN parameters reach the hooks, RFC 1870 SIZE. onChunk: chunks <= chunkSize, one in flight
// (backpressure), concatenation is the exact body. Hook throw/reject -> 451 4.3.0 (454 4.7.0 for AUTH).
import { suite } from "../lib/kit.js";
import { SmtpClient, SmtpServer } from "dyna:smtp";
import { TCPServer } from "dyna:net";
import { PBKDF2, HMAC, SHA1, SHA256, SHA512 } from "dyna:crypto";
import { Spawn, Which } from "dyna:sys";
import * as std from "std";
import * as os from "os";
import { testCert, stopStarted } from "../lib/services.js";

const t = suite("test_smtp_smtp_server");
const enc = new TextEncoder(), dec = new TextDecoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (f, ms = 3000) => { for (let i = 0; i < ms / 10 && !f(); i++) await sleep(10); return f(); };
const MSG = "From: a@b.test\r\nTo: x@y.test\r\nSubject: t\r\n\r\nhello\r\n.leading dot\r\n..two\r\nlast\r\n";
const cert = testCert();
const servers = [];

function server(o = {}) {
    const s = new SmtpServer({ port: 0, tls: "none", name: "mx.test", ...o });
    s.listen();
    servers.push(s);
    return s;
}
const client = (s, o = {}) => new SmtpClient({ host: "127.0.0.1", port: s.port, tls: "none", name: "client.test", ...o });

function raw(port) {
    return new Promise((res) => {
        const st = { buf: "", closed: false };
        st.cl = TCPServer.connect({ host: "127.0.0.1", port }, {
            connect(c) { st.c = c; res(st); },
            data(c, b) { st.buf += dec.decode(b); },
            close() { st.closed = true; },
        });
        st.send = (s) => st.c.write(enc.encode(s));
    });
}

async function scramCreds(mech, pass, iterations = 4096) {
    const h = { "SCRAM-SHA-1": "sha1", "SCRAM-SHA-256": "sha256", "SCRAM-SHA-512": "sha512" }[mech];
    const len = { sha1: 20, sha256: 32, sha512: 64 }[h];
    const salt = enc.encode("salt-" + mech);
    const sp = await PBKDF2({ hash: h, password: pass, salt, iterations, length: len });
    const storedKey = { sha1: SHA1, sha256: SHA256, sha512: SHA512 }[h](HMAC(h, sp, "Client Key"));
    return { salt, iterations, storedKey, serverKey: HMAC(h, sp, "Server Key") };
}

t.test("pipelined DATA: hook arguments, per-recipient verdicts, exact transparency, stats", async (a) => {
    const seen = {};
    const s = server({
        onHelo(sess, h) { seen.helo = h; seen.sess = { ...sess }; },
        onMail(sess, m) { seen.mail = m; },
        onRcpt(sess, r) { return r.address !== "bad@y.test"; },
        onData(sess, m) { seen.data = m; },
    });
    const c = client(s);
    const r = await c.send({ from: "a@b.test", to: ["x@y.test", "bad@y.test"], data: MSG });
    a.deep(r.accepted, ["x@y.test"]);
    a.eq(r.rejected[0].code, 550);
    a.eq(r.rejected[0].enhanced, "5.1.1");
    a.eq(r.code, 250);
    a.deep(seen.helo, { name: "client.test", ehlo: true });
    a.eq(seen.sess.remoteAddress, "127.0.0.1");
    a.eq(seen.sess.tls, false);
    a.eq(seen.mail.address, "a@b.test");
    a.eq(seen.data.from, "a@b.test");
    a.deep(seen.data.to.map((x) => x.address), ["x@y.test"]);
    a.eq(dec.decode(seen.data.data), MSG, "unstuffed body is the message");
    a.eq(seen.data.size, MSG.length);
    await c.quit();
    c.close();
    const st = s.stats();
    a.eq(st.messages, 1);
    a.ok(st.jobs > 0 && st.mainP50Us > 0 && st.mainP99Us >= st.mainP50Us && (std.getenv("DYNA_SMTP_INLINE") ? st.handoffP50Us >= 0 : st.handoffP50Us > 0), JSON.stringify(st));
});

t.test("async hooks, custom verdicts, throw and reject -> 451 + hookErrors", async (a) => {
    const s = server({
        async onMail(sess, m) { await sleep(5); if (m.address === "no@b.test") return { code: 550, message: "no thanks", enhanced: "5.7.1" }; },
        onRcpt(sess, r) { if (r.address === "boom@y.test") throw new Error("boom"); if (r.address === "rej@y.test") return Promise.reject(new Error("r")); },
        async onData() { await sleep(5); return false; },
    });
    const c = client(s);
    const e = await a.rejects(c.send({ from: "no@b.test", to: "x@y.test", data: MSG }), { code: "SMTP_REJECTED" });
    a.eq(e.smtpCode, 550);
    a.match(e.response, /no thanks/);
    const e2 = await a.rejects(c.send({ from: "a@b.test", to: ["boom@y.test", "rej@y.test"], data: MSG }), { code: "SMTP_REJECTED" });
    a.deep(e2.rejected.map((x) => [x.code, x.enhanced]), [[451, "4.3.0"], [451, "4.3.0"]]);
    const e3 = await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: MSG }), { code: "SMTP_REJECTED" });
    a.ok(e3.smtpCode >= 500, "onData false denies: " + e3.smtpCode);
    a.eq(s.stats().hookErrors, 2);
    c.close();
});

t.test("BDAT + BINARYMIME: NUL bytes arrive exactly; DSN parameters reach the hooks", async (a) => {
    const seen = {};
    const s = server({ onMail(sess, m) { seen.mail = m; }, onRcpt(sess, r) { seen.rcpt = r; }, onData(sess, m) { seen.data = m; } });
    const c = client(s);
    const bin = new Uint8Array([...enc.encode("Subject: b\r\n\r\n"), 0, 1, 2, 255, 13, 10, 46, 13, 10]);
    await c.send({ from: "a@b.test", to: "x@y.test", data: bin, bdat: true, dsn: { ret: "HDRS", envid: "e1", notify: ["FAILURE", "DELAY"], orcpt: true } });
    a.eq(seen.mail.body, "BINARYMIME");
    a.eq(seen.mail.ret, "HDRS");
    a.eq(seen.mail.envid, "e1");
    a.match(seen.rcpt.notify, /FAILURE/);
    a.match(seen.rcpt.orcpt, /rfc822;x@y\.test/i);
    const got = seen.data.data;
    a.eq(got.length, bin.length);
    a.ok(got.every((v, i) => v === bin[i]), "byte-exact");
    c.close();
});

t.test("onChunk streaming: chunk <= chunkSize, one in flight, exact concatenation (DATA and BDAT)", async (a) => {
    let live = 0, maxLive = 0, parts = [], max = 0, dataArg;
    const s = server({
        chunkSize: 1024,
        async onChunk(sess, u8) { live++; maxLive = Math.max(maxLive, live); max = Math.max(max, u8.length); parts.push(u8.slice()); await sleep(1); live--; },
        onData(sess, m) { dataArg = m; },
    });
    const c = client(s);
    const body = "Subject: big\r\n\r\n" + Array.from({ length: 1500 }, (_, i) => (i % 7 ? "line " : ".dot ") + i).join("\r\n") + "\r\n";
    for (const bdat of [false, true]) {
        parts = [];
        await c.send({ from: "a@b.test", to: "x@y.test", data: body, bdat });
        const all = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
        parts.reduce((o, p) => (all.set(p, o), o + p.length), 0);
        a.eq(dec.decode(all), body, "concatenation, bdat=" + bdat);
        a.ok(parts.length > 10, "streamed in " + parts.length + " chunks");
        a.eq(dataArg.data, undefined, "no data with onChunk");
        a.eq(dataArg.size, body.length);
    }
    a.ok(max <= 1024, "max chunk " + max);
    a.eq(maxLive, 1, "backpressure: one onChunk in flight");
    c.close();
});

t.test("AUTH PLAIN and SCRAM-SHA-1/256/512 via onAuth; wrong password fails; session.user", async (a) => {
    const users = { tim: "pw" };
    let user;
    const s = server({
        insecureAuth: true,
        async onAuth(sess, x) {
            if (x.mechanism === "PLAIN") return users[x.username] === x.password;
            return users[x.username] ? scramCreds(x.mechanism, users[x.username]) : false;
        },
        onMail(sess) { user = sess.user; },
    });
    for (const mech of ["PLAIN", "SCRAM-SHA-1", "SCRAM-SHA-256", "SCRAM-SHA-512"]) {
        user = undefined;
        const c = client(s, { user: "tim", pass: "pw", mechanisms: [mech], insecureAuth: true });
        const r = await c.send({ from: "a@b.test", to: "x@y.test", data: MSG });
        a.eq(r.code, 250, mech);
        a.eq(user, "tim", mech + " session.user");
        c.close();
        const bad = client(s, { user: "tim", pass: "nope", mechanisms: [mech], insecureAuth: true });
        await a.rejects(bad.send({ from: "a@b.test", to: "x@y.test", data: MSG }), { code: "SMTP_AUTH" });
        bad.close();
    }
});

t.test("STARTTLS and implicit TLS: session.tls, AUTH PLAIN over TLS", async (a) => {
    if (!cert) return a.skip("openssl missing");
    for (const mode of ["starttls", "implicit"]) {
        let tls;
        const s = server({ tls: mode, cert: cert.cert, key: cert.key, onAuth: (sess, x) => x.password === "pw", onData(sess) { tls = sess.tls; } });
        const c = client(s, { tls: mode, ca: cert.ca, servername: "localhost", user: "tim", pass: "pw", mechanisms: ["PLAIN"] });
        const r = await c.send({ from: "a@b.test", to: "x@y.test", data: MSG });
        a.eq(r.code, 250, mode);
        a.eq(tls, true, mode + " session.tls");
        c.close();
    }
});

t.test("RFC 3207: plaintext injected after STARTTLS is discarded (injecting proxy)", async (a) => {
    if (!cert) return a.skip("openssl missing");
    const froms = [];
    const s = server({ tls: "starttls", cert: cert.cert, key: cert.key, onMail(sess, m) { froms.push(m.address); } });
    let injected = 0;
    const px = new TCPServer({ port: 0 });
    px.start({
        connect(cc) {
            cc.q = [];
            cc.up = TCPServer.connect({ host: "127.0.0.1", port: s.port }, {
                connect(u) { cc.u = u; for (const b of cc.q) u.write(b); cc.q = null; },
                data(u, b) { cc.write(b); },
                close() { cc.close(); },
            });
        },
        data(cc, b) {
            if (!injected && dec.decode(b) === "STARTTLS\r\n") { injected = 1; b = enc.encode("STARTTLS\r\nMAIL FROM:<evil@x.test>\r\n"); }
            if (cc.q) cc.q.push(b); else cc.u.write(b);
        },
        close(cc) { if (cc.u) cc.u.close(); },
    });
    const c = new SmtpClient({ host: "127.0.0.1", port: px.port, ca: cert.ca, servername: "localhost", name: "client.test", commandTimeoutMs: 5000 });
    const r = await c.send({ from: "a@b.test", to: "x@y.test", data: MSG });
    a.eq(injected, 1, "proxy injected");
    a.eq(r.code, 250);
    a.deep(froms, ["a@b.test"], "only the post-TLS MAIL reached onMail");
    c.close();
    px.close();
}, { timeoutMs: 20000 });

t.test("raw peers: bare LF 554 5.6.0, command timeout 421 4.4.2, maxConnections 421 4.7.0, close() 421 4.3.2", async (a) => {
    const s = server({ commandTimeoutMs: 300, maxConnections: 2 });
    const p = await raw(s.port);
    await until(() => /^220 /m.test(p.buf));
    p.send("EHLO r.test\r\nMAIL FROM:<a@b.test>\r\nRCPT TO:<x@y.test>\r\nDATA\r\n");
    await until(() => /^354 /m.test(p.buf));
    p.send("a\nb\r\n.\r\n");
    a.ok(await until(() => /^554 5\.6\.0/m.test(p.buf) && p.closed), "bare LF: " + JSON.stringify(p.buf.slice(-80)));
    const q = await raw(s.port);
    a.ok(await until(() => /^421 Timeout/m.test(q.buf) && q.closed, 3000), "timeout: " + JSON.stringify(q.buf));
    const s2 = server({ maxConnections: 1 });
    const r1 = await raw(s2.port);
    await until(() => /^220 /m.test(r1.buf));
    const r2 = await raw(s2.port);
    a.ok(await until(() => /^421 4\.7\.0/m.test(r2.buf) && r2.closed), "busy: " + JSON.stringify(r2.buf));
    a.eq(s2.stats().refused, 1);
    s2.close();
    a.ok(await until(() => /^421 4\.3\.2/m.test(r1.buf) && r1.closed), "shutdown: " + JSON.stringify(r1.buf));
    a.ok(s2.closed, "closed");
});

t.test("options: strict keys, ranges, CRAM-MD5, tls needs cert, requireAuth needs onAuth", (a) => {
    a.throws(() => new SmtpServer({ bogus: 1 }), TypeError);
    a.throws(() => new SmtpServer({ tls: "maybe" }), RangeError);
    a.throws(() => new SmtpServer({ tls: "starttls" }), TypeError);
    a.throws(() => new SmtpServer({ mechanisms: ["CRAM-MD5"] }), RangeError);
    a.throws(() => new SmtpServer({ requireAuth: true }), TypeError);
    a.throws(() => new SmtpServer({ chunkSize: 10 }), RangeError);
    a.throws(() => new SmtpServer({ disable: ["NOPE"] }), RangeError);
    a.throws(() => new SmtpServer({ onData: 1 }), TypeError);
    const s = new SmtpServer({ port: 0, tls: "none" });
    a.ok(s.listen() > 0);
    s.dispose();
    a.ok(s.closed);
});

t.test("differential: swaks delivers the same body as SmtpClient (swaks ends the data with CRLF.CRLF itself)", async (a) => {
    if (!Which("swaks")) return a.skip("swaks missing");
    const bodies = [];
    const s = server({ insecureAuth: true, onAuth: (sess, x) => x.password === "pw", onData(sess, m) { bodies.push(dec.decode(m.data)); } });
    const c = client(s, { user: "tim", pass: "pw", insecureAuth: true, mechanisms: ["PLAIN"] });
    await c.send({ from: "a@b.test", to: "x@y.test", data: MSG });
    c.close();
    const file = std.getenv("TMPDIR").replace(/\/$/, "") + "/swaks-s2b-" + Math.random().toString(36).slice(2) + ".eml";
    const f = std.open(file, "w"); f.puts(MSG.slice(0, -2)); f.close();
    const p = new Spawn("swaks", ["--server", "127.0.0.1:" + s.port, "--helo", "client.test", "--to", "x@y.test", "--from", "a@b.test", "-a", "PLAIN", "-au", "tim", "-ap", "pw", "--data", "@" + file], { timeoutMs: 20000 });
    let out = "";
    for await (const ch of p.stdout) out += dec.decode(ch);
    const w = await p.wait();
    os.remove(file);
    a.eq(w.code, 0, "swaks exit: " + out.slice(-300));
    a.eq(bodies.length, 2);
    a.eq(bodies[1], bodies[0], "identical bodies");
});

const waitRe = (p, re, ms = 3000) => until(() => re.test(p.buf), ms);
async function ehlo(s) {
    const p = await raw(s.port);
    await waitRe(p, /^220 /m);
    p.send("EHLO r.test\r\n");
    await waitRe(p, /^250 /m);
    return p;
}

t.test("large bodies: 4 MiB DATA and BDAT through a slow onChunk (receive pause/resume), slow onData", async (a) => {
    let live = 0, maxLive = 0, n = 0, total = 0, h = 0;
    const s = server({
        maxMessageSize: 64 << 20,
        async onChunk(sess, u8) { live++; maxLive = Math.max(maxLive, live); n++; total += u8.length; for (const v of u8) h = (h * 31 + v) >>> 0; await sleep(2); live--; },
    });
    const line = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\r\n";
    const body = "Subject: large\r\n\r\n" + line.repeat(Math.ceil((4 << 20) / line.length));
    let want = 0;
    for (const v of enc.encode(body)) want = (want * 31 + v) >>> 0;
    const c = client(s);
    for (const bdat of [false, true]) {
        n = total = h = 0;
        const r = await c.send({ from: "a@b.test", to: "x@y.test", data: body, bdat });
        a.eq(r.code, 250, "bdat=" + bdat);
        a.eq(total, body.length, "all octets, bdat=" + bdat);
        a.eq(h, want, "byte-exact hash, bdat=" + bdat);
        a.ok(n >= 64, n + " chunks");
    }
    a.eq(maxLive, 1, "one onChunk in flight");
    c.close();
    let got = null;
    const s2 = server({ maxMessageSize: 64 << 20, async onData(sess, m) { await sleep(50); got = m.data.length; } });
    const c2 = client(s2);
    a.eq((await c2.send({ from: "a@b.test", to: "x@y.test", data: body })).code, 250);
    a.eq(got, body.length, "slow onData whole body");
    c2.close();
}, { timeoutMs: 20000 });

t.test("option plumbing on the wire: requireTls/requireAuth 530, insecureAuth 538, disable, SIZE, maxRecipients 452", async (a) => {
    if (cert) {
        const p = await ehlo(server({ tls: "starttls", cert: cert.cert, key: cert.key, requireTls: true }));
        a.match(p.buf, /^250-STARTTLS/m);
        p.send("MAIL FROM:<a@b.test>\r\n");
        a.ok(await waitRe(p, /^530 5\.7\.0 Must issue a STARTTLS/m), "requireTls: " + p.buf.slice(-80));
        p.c.close();
    }
    let p = await ehlo(server({ requireAuth: true, insecureAuth: true, onAuth: () => true }));
    a.match(p.buf, /^250-AUTH /m);
    p.send("MAIL FROM:<a@b.test>\r\n");
    a.ok(await waitRe(p, /^530 5\.7\.0 Authentication required/m), "requireAuth: " + p.buf.slice(-80));
    p.c.close();
    p = await ehlo(server({ onAuth: () => true }));
    a.ok(!/AUTH/.test(p.buf), "AUTH not advertised in cleartext: " + p.buf);
    p.send("AUTH PLAIN AHRpbQBwdw==\r\n");
    a.ok(await waitRe(p, /^538 5\.7\.11/m), "538: " + p.buf.slice(-80));
    p.c.close();
    p = await ehlo(server({ disable: ["CHUNKING", "DSN"], maxMessageSize: 5000, maxRecipients: 2 }));
    a.ok(!/CHUNKING|BINARYMIME|DSN/.test(p.buf), "disabled: " + p.buf);
    a.match(p.buf, /^250-SIZE 5000\r$/m);
    p.send("MAIL FROM:<a@b.test>\r\nRCPT TO:<x1@y.test>\r\nRCPT TO:<x2@y.test>\r\nRCPT TO:<x3@y.test>\r\n");
    a.ok(await waitRe(p, /^452 4\.5\.3/m), "maxRecipients: " + p.buf.slice(-120));
    p.c.close();
});

t.test("verdict edge cases: invalid object 451, bad SCRAM key length 454, OAuth token reaches onAuth", async (a) => {
    let tok = null;
    const s = server({
        insecureAuth: true,
        onMail: () => ({ code: "nope" }),
        onAuth(sess, x) {
            if (x.mechanism === "OAUTHBEARER" || x.mechanism === "XOAUTH2") { tok = x.token; return true; }
            return { salt: "s", iterations: 4096, storedKey: new Uint8Array(3), serverKey: new Uint8Array(3) };
        },
    });
    const c = client(s, { user: "tim", token: "tok-123", insecureAuth: true });
    const e = await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: MSG }), { code: "SMTP_REJECTED" });
    a.eq(e.smtpCode, 451);
    a.eq(tok, "tok-123", "token argument");
    a.eq(s.stats().hookErrors, 1);
    c.close();
    const c2 = client(s, { user: "tim", pass: "pw", mechanisms: ["SCRAM-SHA-256"] });
    const e2 = await a.rejects(c2.send({ from: "a@b.test", to: "x@y.test", data: MSG }), { code: "SMTP_AUTH" });
    a.match(String(e2.response || e2.message), /454/);
    a.eq(s.stats().hookErrors, 2);
    c2.close();
});

t.test("lifetime: peer leaves during an async hook; close() from inside a hook", async (a) => {
    let settled = 0;
    const s = server({ async onMail() { await sleep(80); settled++; } });
    const p = await ehlo(s);
    p.send("MAIL FROM:<a@b.test>\r\n");
    await sleep(10);
    p.c.close();
    await sleep(150);
    a.eq(settled, 1, "hook settled after the peer left");
    const c = client(s);
    a.eq((await c.send({ from: "a@b.test", to: "x@y.test", data: MSG })).code, 250, "server still serves");
    c.close();
    const s2 = server({ onData() { s2.close(); } });
    const c2 = client(s2);
    await a.rejects(c2.send({ from: "a@b.test", to: "x@y.test", data: MSG }));
    a.ok(s2.closed, "closed from a hook");
    c2.close();
    await sleep(50);
});

t.after(async () => {
    for (const s of servers) if (!s.closed) s.close();
    await stopStarted();
});

await t.run();
