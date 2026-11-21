// flags: --std
// covers: dyna:smtp.SmtpClient
// test_smtp_smtp_client_session_binary.js -- S1c client fixes, against the dyna:smtp SmtpServer, a scripted
// dyna:net fake, exim and mailpit (tests/lib/services.js, stopped in teardown). S9 S1c, 2026-11-27.
// (1) RFC 5321 4.2.2/3.8: a 421 after a delivered message (server per-session limit) closes that session;
// the in-flight message is requeued at the queue head and the queue continues; a 421
// on a connection that delivered nothing still rejects (no reconnect loop). (2) RFC 3030 2/3: under BDAT
// with BODY=BINARYMIME the octets go exactly as given (NUL, bare CR, bare LF, a line over 998 octets, no
// trailing CRLF); without BINARYMIME the body is still CRLF-normalised (RFC 5321 2.3.8).
import { suite } from "../lib/kit.js";
import { SmtpClient, SmtpServer } from "dyna:smtp";
import { TCPServer } from "dyna:net";
import * as os from "os";
import { ensureExim, ensureMailpit, stopStarted } from "../lib/services.js";

const t = suite("test_smtp_smtp_client_session_binary");
const enc = new TextEncoder(), dec = new TextDecoder();
const EXIM = 25291, MP_SMTP = 25293, MP_HTTP = 25294;
const servers = [];
let ex = null, mp = null;
t.before(async () => { ex = await ensureExim(EXIM, ["smtp_accept_max_per_connection = 3"]); mp = await ensureMailpit(MP_SMTP, MP_HTTP); });
t.after(async () => { for (const s of servers) if (!s.closed) s.close(); await stopStarted(); });

function server(o = {}) {
    const s = new SmtpServer({ port: 0, tls: "none", name: "mx.test", ...o });
    s.listen();
    servers.push(s);
    return s;
}
const msg = (i) => "Subject: m" + i + "\r\n\r\nbody " + i + "\r\n";
const same = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);

t.test("421 session limit: every queued message is delivered over new connections (SmtpServer)", async (a) => {
    const got = [];
    const s = server({ maxMessagesPerSession: 5, onData(sess, m) { got.push(dec.decode(m.data)); } });
    const c = new SmtpClient({ host: "127.0.0.1", port: s.port, tls: "none", maxConnections: 2, maxMessagesPerConnection: 0 });
    const N = 37;
    const rs = await Promise.all(Array.from({ length: N }, (_, i) => c.send({ from: "a@b.test", to: "x@y.test", data: msg(i) })));
    a.ok(rs.every((r) => r.code === 250), "all accepted");
    a.eq(got.length, N, "each delivered once");
    a.deep(got.map((b) => +/body (\d+)/.exec(b)[1]).sort((x, y) => x - y), Array.from({ length: N }, (_, i) => i));
    a.ok(s.stats().sessions >= Math.ceil(N / 5), "reconnected: " + s.stats().sessions + " sessions");
    await c.quit();
    c.close();
});

t.test("421 on a connection that delivered nothing rejects; requeue is bounded", async (a) => {
    let conns = 0;
    const f = new TCPServer({ port: 0 });
    f.start({
        connect(k) { conns++; k.n = conns; k.mails = 0; k.buf = ""; k.data = false; k.write(enc.encode("220 fake ESMTP\r\n")); },
        data(k, b) {
            k.buf += dec.decode(b);
            let i;
            while ((i = k.buf.indexOf("\r\n")) >= 0) {
                const line = k.buf.slice(0, i);
                k.buf = k.buf.slice(i + 2);
                if (k.data) { if (line === ".") { k.data = false; k.write(enc.encode("250 2.0.0 ok\r\n")); } continue; }
                const v = line.split(" ")[0].toUpperCase();
                if (v === "EHLO") k.write(enc.encode("250-fake\r\n250 8BITMIME\r\n"));
                else if (v === "MAIL") { if (k.n > 1 || k.mails++ >= 1) { k.write(enc.encode("421 4.7.0 go away\r\n")); k.close(); return; } k.write(enc.encode("250 ok\r\n")); }
                else if (v === "RCPT") k.write(enc.encode("250 ok\r\n"));
                else if (v === "DATA") { k.data = true; k.write(enc.encode("354 go\r\n")); }
                else if (v === "QUIT") { k.write(enc.encode("221 bye\r\n")); k.close(); return; }
                else k.write(enc.encode("250 ok\r\n"));
            }
        },
    });
    const c = new SmtpClient({ host: "127.0.0.1", port: f.port, tls: "none", maxMessagesPerConnection: 0 });
    const p1 = c.send({ from: "a@b.test", to: "x@y.test", data: msg(1) }), p2 = c.send({ from: "a@b.test", to: "x@y.test", data: msg(2) });
    a.eq((await p1).code, 250, "first delivered");
    const e = await a.rejects(p2, { code: "SMTP_CONNECTION" });
    a.match(String(e && e.message), /closing/);
    a.eq(conns, 2, "one reconnect, then the 421 on a fresh connection rejects");
    c.close();
    f.close();
}, { timeoutMs: 15000 });

t.test("421 session limit against exim (smtp_accept_max_per_connection = 3)", async (a) => {
    if (!ex || !ex.ok) return a.skip("exim: " + (ex ? ex.why : "not started"));
    const dir = ex.dir + "/spool/input";
    const count = () => { const [l] = os.readdir(dir); return (l || []).filter((n) => n.endsWith("-D")).length; };
    const before = count();
    const c = new SmtpClient({ host: "127.0.0.1", port: EXIM, tls: "none", maxConnections: 1, maxMessagesPerConnection: 0 });
    const rs = await Promise.all(Array.from({ length: 10 }, (_, i) => c.send({ from: "a@b.test", to: "u@test.example", data: msg(i) })));
    a.ok(rs.every((r) => r.code === 250), "all accepted by exim");
    a.eq(count() - before, 10, "ten spool files");
    await c.quit();
    c.close();
});

t.test("BINARYMIME: BDAT body arrives byte-exact (NUL, bare CR/LF, 2000-octet line, no final CRLF)", async (a) => {
    const seen = [];
    const s = server({ onMail(sess, m) { seen.push(m.body); }, onData(sess, m) { seen.push(m.data); } });
    const c = new SmtpClient({ host: "127.0.0.1", port: s.port, tls: "none" });
    const long = "z".repeat(2000);
    const cases = [
        new Uint8Array([...enc.encode("Subject: b\r\n\r\n"), 0, 1, 2, 255, 10, 13, 46, 13, 10, 46]),
        enc.encode("Subject: lf\n\nline1\nline2\r.\nend"),
        enc.encode("Subject: long\r\n\r\n" + long + "\r\nx"),
    ];
    for (const bin of cases) {
        seen.length = 0;
        await c.send({ from: "a@b.test", to: "x@y.test", data: bin, bdat: true });
        a.eq(seen[0], "BINARYMIME", "BODY=BINARYMIME");
        a.ok(same(seen[1], bin), "byte-exact: got " + seen[1].length + " want " + bin.length);
    }
    seen.length = 0;
    await c.send({ from: "a@b.test", to: "x@y.test", data: "Subject: d\n\nbare\nlf", bdat: false });
    a.eq(dec.decode(seen[1]), "Subject: d\r\n\r\nbare\r\nlf\r\n", "DATA still normalises");
    seen.length = 0;
    await c.send({ from: "a@b.test", to: "x@y.test", data: "Subject: t\r\n\r\nclean\r\n", bdat: true });
    a.eq(seen[0], undefined, "clean 7-bit text stays 7BIT under BDAT");
    c.close();
    const s2 = server({ disable: ["BINARYMIME"], onMail(sess, m) { seen.push(m.body); }, onData(sess, m) { seen.push(m.data); } });
    const c2 = new SmtpClient({ host: "127.0.0.1", port: s2.port, tls: "none" });
    seen.length = 0;
    await c2.send({ from: "a@b.test", to: "x@y.test", data: "Subject: n\n\nbare\nlf", bdat: true });
    a.eq(dec.decode(seen[1]), "Subject: n\r\n\r\nbare\r\nlf\r\n", "CHUNKING without BINARYMIME normalises");
    await a.rejects(c2.send({ from: "a@b.test", to: "x@y.test", data: cases[0], bdat: true }), { code: "SMTP_LOCAL", message: /NUL/ });
    c2.close();
});

t.test("mailpit peer: a bare-LF BDAT body is stored as sent when BINARYMIME is offered, else CRLF-normalised", async (a) => {
    if (!mp || !mp.ok) return a.skip("mailpit: " + (mp ? mp.why : "not started"));
    const ehlo = await new Promise((res) => {
        let buf = "";
        TCPServer.connect({ host: "127.0.0.1", port: MP_SMTP }, {
            connect(k) { k.write(enc.encode("EHLO t\r\n")); },
            data(k, b) { buf += dec.decode(b); if (/^250 /m.test(buf)) { k.write(enc.encode("QUIT\r\n")); k.close(); res(buf); } },
            close() { res(buf); },
        });
    });
    const binary = /BINARYMIME/.test(ehlo) && /CHUNKING/.test(ehlo);
    const subj = "s1c-" + Math.random().toString(36).slice(2);
    const body = enc.encode("Subject: " + subj + "\n\nbare\nlf\rcr\r\nend\r\n");
    const c = new SmtpClient({ host: "127.0.0.1", port: MP_SMTP, tls: "none" });
    await c.send({ from: "a@b.test", to: "x@y.test", data: body, bdat: true });
    c.close();
    const api = (p) => fetch("http://127.0.0.1:" + MP_HTTP + p);
    let id = null;
    for (let i = 0; i < 50 && !id; i++) {
        const j = await (await api("/api/v1/search?query=" + encodeURIComponent('subject:"' + subj + '"'))).json();
        if (j.messages && j.messages.length) id = j.messages[0].ID; else await new Promise((r) => setTimeout(r, 20));
    }
    a.ok(id, "stored");
    const raw = new Uint8Array(await (await api("/api/v1/message/" + id + "/raw")).arrayBuffer());
    const want = binary ? body : enc.encode(dec.decode(body).replace(/\r\n|\r|\n/g, "\r\n"));
    const tail = raw.slice(raw.length - want.length);
    a.ok(same(tail, want), (binary ? "byte-exact" : "normalised") + " (mailpit EHLO " + (binary ? "has" : "lacks") + " BINARYMIME): " + JSON.stringify(dec.decode(raw).slice(-60)));
});

await t.run();
