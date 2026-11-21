// covers: dyna:smtp.SmtpClient
// test_smtp_smtp_client_fake.js -- dyna:smtp against an in-process scripted SMTP server (dyna:net TCPServer)
// that records every byte the client writes. S9 S1b, 2026-11-27.
// Oracles: RFC 5321 (sequencing, 4.5.2 transparency: the server unstuffs and must get the exact
// CRLF-normalised message back; 421 ends the session), RFC 2920 (MAIL+RCPT+DATA in ONE write; lone "."
// after 354 when every RCPT failed), RFC 3207 (bytes after the STARTTLS 220 are an injection: no TLS
// attempt, SMTP_TLS), RFC 4954 (cleartext PLAIN refused without TLS unless insecureAuth), RFC 1870 SIZE.
// Pool: maxConnections caps concurrent sessions, maxMessagesPerConnection forces QUIT + reconnect.
import { suite } from "../lib/kit.js";
import { SmtpClient } from "dyna:smtp";
import { TCPServer } from "dyna:net";

const t = suite("test_smtp_smtp_client_fake");
const enc = new TextEncoder(), dec = new TextDecoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function fake(opts = {}) {
    const st = { conns: 0, maxLive: 0, live: 0, writes: [], cmds: [], bodies: [], quits: 0 };
    const caps = opts.caps || ["PIPELINING", "8BITMIME", "SIZE 100000", "ENHANCEDSTATUSCODES"];
    const srv = new TCPServer({ port: 0 });
    srv.start({
        connect(c) {
            st.conns++; st.live++; st.maxLive = Math.max(st.maxLive, st.live);
            c.s = { buf: "", data: false, body: "" };
            if (!opts.silent) c.write(enc.encode(opts.greeting || "220 fake.test ESMTP\r\n"));
        },
        close() { st.live--; },
        data(c, bytes) {
            const s = c.s;
            const chunk = dec.decode(bytes);
            st.writes.push(chunk);
            s.buf += chunk;
            let i;
            while ((i = s.buf.indexOf("\r\n")) >= 0) {
                const line = s.buf.slice(0, i);
                s.buf = s.buf.slice(i + 2);
                if (s.data) {
                    if (line === ".") {
                        s.data = false;
                        st.bodies.push(s.body);
                        c.write(enc.encode(opts.final || "250 2.0.0 queued\r\n"));
                    } else s.body += (line.startsWith(".") ? line.slice(1) : line) + "\r\n";
                    continue;
                }
                st.cmds.push(line);
                const verb = line.split(" ")[0].toUpperCase();
                const hook = opts.on && opts.on(verb, line, c, st);
                if (hook !== undefined) { if (hook) c.write(enc.encode(hook)); continue; }
                if (verb === "EHLO") c.write(enc.encode(caps.map((k, j) => "250" + (j === caps.length - 1 ? " " : "-") + k).join("\r\n").replace(/^/, "250-fake.test\r\n") + "\r\n"));
                else if (verb === "MAIL") c.write(enc.encode("250 2.1.0 ok\r\n"));
                else if (verb === "RCPT") c.write(enc.encode(/bad@/.test(line) ? "550 5.1.1 no such user\r\n" : "250 2.1.5 ok\r\n"));
                else if (verb === "DATA") { s.data = true; s.body = ""; c.write(enc.encode("354 go\r\n")); }
                else if (verb === "RSET") c.write(enc.encode("250 2.0.0 reset\r\n"));
                else if (verb === "QUIT") { st.quits++; c.write(enc.encode("221 2.0.0 bye\r\n")); c.close(); }
                else c.write(enc.encode("502 5.5.1 unknown\r\n"));
            }
        },
    });
    return { srv, st, port: srv.port };
}

const MSG = "From: a@b.test\r\nTo: x@y.test\r\nSubject: t\r\n\r\nhello\r\n.leading dot\r\n..two\r\nlast";

t.test("pipelined send: one write, per-recipient results, exact transparency", async (a) => {
    const f = fake();
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none", name: "client.test" });
    const r = await c.send({ from: "a@b.test", to: ["x@y.test", "bad@y.test"], data: MSG });
    a.deep(r.accepted, ["x@y.test"], "accepted");
    a.eq(r.rejected.length, 1, "one rejected");
    a.eq(r.rejected[0].recipient, "bad@y.test");
    a.eq(r.rejected[0].code, 550);
    a.eq(r.rejected[0].enhanced, "5.1.1");
    a.eq(r.code, 250);
    a.eq(r.enhanced, "2.0.0");
    a.ok(f.st.writes.some((w) => /^MAIL FROM:<a@b\.test> SIZE=\d+\r\nRCPT TO:<x@y\.test>\r\nRCPT TO:<bad@y\.test>\r\nDATA\r\n$/.test(w)), "MAIL+RCPT+RCPT+DATA in one write: " + JSON.stringify(f.st.writes.slice(1, 3)));
    a.eq(f.st.bodies[0], MSG + "\r\n", "server unstuffed body is the CRLF-terminated message");
    await c.quit();
    a.eq(f.st.quits, 1, "QUIT sent");
    f.srv.close();
});

t.test("every recipient rejected: lone dot, SMTP_REJECTED with the list", async (a) => {
    const f = fake();
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none" });
    const e = await a.rejects(c.send({ from: "a@b.test", to: ["bad@y.test"], data: "x" }), { code: "SMTP_REJECTED" });
    a.ok(e && e.rejected && e.rejected[0].code === 550, "rejected list on the error");
    a.ok(f.st.writes.includes(".\r\n"), "lone dot after 354");
    a.eq(f.st.bodies[0], "", "no body sent");
    c.close();
    f.srv.close();
});

t.test("bare LF and lone CR normalised; 8-bit refused without 8BITMIME", async (a) => {
    const f = fake({ caps: ["PIPELINING"] });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none" });
    await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: "café" }), { code: "SMTP_LOCAL", message: /8BITMIME/ });
    const r = await c.send({ from: "a@b.test", to: "x@y.test", data: enc.encode("a\nb\rc\r\n.\n") });
    a.eq(r.code, 250);
    a.eq(f.st.bodies[0], "a\r\nb\r\nc\r\n.\r\n", "normalised and transparent");
    c.close();
    f.srv.close();
});

t.test("SIZE limit refused locally", async (a) => {
    const f = fake({ caps: ["SIZE 100"] });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none" });
    await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: "x".repeat(200) }), { code: "SMTP_LOCAL", message: /SIZE/ });
    a.ok(!f.st.cmds.some((l) => l.startsWith("MAIL")), "no MAIL sent");
    c.close();
    f.srv.close();
});

t.test("421 mid-transaction rejects with SMTP_CONNECTION", async (a) => {
    const f = fake({ on: (v) => (v === "MAIL" ? "421 4.3.2 going down\r\n" : undefined) });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none" });
    const e = await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: "x" }), { code: "SMTP_CONNECTION" });
    a.match(e && e.message, /421/);
    c.close();
    f.srv.close();
});

t.test("STARTTLS: bytes after the 220 are refused before any TLS", async (a) => {
    const f = fake({ caps: ["STARTTLS"], on: (v) => (v === "STARTTLS" ? "220 2.0.0 go\r\n250 injected\r\n" : undefined) });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port });
    await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: "x" }), { code: "SMTP_TLS", message: /injection/ });
    c.close();
    f.srv.close();
});

t.test("STARTTLS required but not offered", async (a) => {
    const f = fake();
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port });
    await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: "x" }), { code: "SMTP_TLS", message: /STARTTLS/ });
    c.close();
    f.srv.close();
});

t.test("cleartext AUTH refused without insecureAuth; LOGIN with it", async (a) => {
    let auth = [];
    const f = fake({
        caps: ["AUTH PLAIN LOGIN"],
        on: (v, line) => {
            if (v === "AUTH") { auth.push(line); return "334 VXNlcm5hbWU6\r\n"; }
            if (auth.length === 1 && !/^(MAIL|RCPT|DATA|QUIT|EHLO)/.test(line)) { auth.push(line); return "334 UGFzc3dvcmQ6\r\n"; }
            if (auth.length === 2 && !/^(MAIL|RCPT|DATA|QUIT|EHLO)/.test(line)) { auth.push(line); return "235 2.7.0 ok\r\n"; }
            return undefined;
        },
    });
    const c1 = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none", user: "tim", pass: "pw" });
    await a.rejects(c1.send({ from: "a@b.test", to: "x@y.test", data: "x" }), { code: "SMTP_AUTH", message: /cleartext/ });
    c1.close();
    a.eq(auth.length, 0, "nothing sent");
    const c2 = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none", user: "tim", pass: "pw", insecureAuth: true, mechanisms: ["LOGIN"] });
    const r = await c2.send({ from: "a@b.test", to: "x@y.test", data: "x" });
    a.eq(r.code, 250);
    a.deep(auth, ["AUTH LOGIN", "dGlt", "cHc="], "LOGIN exchange");
    c2.close();
    f.srv.close();
});

t.test("pool: maxConnections caps sessions; maxMessagesPerConnection reconnects", async (a) => {
    const f = fake({ on: (v, line, c) => (v === "DATA" ? undefined : undefined) });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none", maxConnections: 3, maxMessagesPerConnection: 2 });
    const ps = [];
    for (let i = 0; i < 12; i++) ps.push(c.send({ from: "a@b.test", to: "x" + i + "@y.test", data: "m" + i }));
    const rs = await Promise.all(ps);
    a.ok(rs.every((r) => r.code === 250), "all 12 sent");
    a.ok(f.st.maxLive <= 3, "at most 3 live sessions, saw " + f.st.maxLive);
    a.ok(f.st.conns >= 6, "reconnects after 2 messages each: " + f.st.conns + " connections");
    a.eq(new Set(f.st.bodies).size, 12, "12 distinct bodies");
    const s = c.stats();
    a.eq(s.messages, 12);
    a.ok(s.jobs >= 12 && s.handoffP99Us >= s.handoffP50Us, "stats " + JSON.stringify(s));
    await c.quit();
    await sleep(50);
    a.eq(f.st.live, 0, "every session closed after quit");
    f.srv.close();
});

t.test("timeouts: silent server", async (a) => {
    const f = fake({ silent: true });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none", connectTimeoutMs: 300 });
    await a.rejects(c.send({ from: "a@b.test", to: "x@y.test", data: "x" }), { code: "SMTP_TIMEOUT", message: /waiting for the server/ });
    c.close();
    f.srv.close();
});

t.test("connect refused rejects every queued message", async (a) => {
    const f = fake();
    const port = f.port;
    f.srv.close();
    await sleep(20);
    const c = new SmtpClient({ host: "127.0.0.1", port, tls: "none" });
    const ps = [1, 2, 3].map((i) => c.send({ from: "a@b.test", to: "x@y.test", data: "x" + i }));
    for (const p of ps) await a.rejects(p, { code: "SMTP_CONNECTION" });
    c.close();
});

t.test("close() rejects pending messages with SMTP_CLOSED; send after quit throws", async (a) => {
    const f = fake({ silent: true });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none" });
    const p = c.send({ from: "a@b.test", to: "x@y.test", data: "x" });
    await sleep(30);
    c.close();
    await a.rejects(p, { code: "SMTP_CLOSED" });
    a.ok(c.closed, "closed");
    const c2 = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none" });
    await c2.quit();
    a.throws(() => c2.send({ from: "a@b.test", to: "x@y.test", data: "x" }), /quitting/);
    c2.close();
    f.srv.close();
});

await t.run();
