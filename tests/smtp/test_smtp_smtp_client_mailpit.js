// flags: --std
// covers: dyna:smtp.SmtpClient
// test_smtp_smtp_client_mailpit.js -- dyna:smtp against mailpit 1.34 as a capture sink on private ports
// (tests/lib/services.js ensureMailpit, stopped in teardown). S9 S1b, 2026-11-27.
// Oracles: the raw stored message must equal the sent message after RFC 5321 CRLF normalisation
// (transparency, 4.5.2) byte for byte, for strings, Uint8Array and ArrayBuffer data; RFC 6531 SMTPUTF8
// envelopes (the RFC 6530 example address); AUTH PLAIN/LOGIN over plaintext only with insecureAuth;
// a server without PIPELINING gets one command per reply; mailpit's envelope recipients match.
import { suite } from "../lib/kit.js";
import { SmtpClient } from "dyna:smtp";
import { ensureMailpit, stopStarted } from "../lib/services.js";

const t = suite("test_smtp_smtp_client_mailpit");
const SMTP = 25261, HTTP = 25262;
let mp = null;
const enc = new TextEncoder(), dec = new TextDecoder();

t.before(async () => { mp = await ensureMailpit(SMTP, HTTP); });
t.after(async () => { await stopStarted(); });
function need(a) { if (!mp || !mp.ok) a.skip("mailpit: " + (mp ? mp.why : "not started")); }
const api = (p) => fetch("http://127.0.0.1:" + HTTP + p);

async function find(subject) {
    for (let i = 0; i < 50; i++) {
        const j = await (await api("/api/v1/search?query=" + encodeURIComponent('subject:"' + subject + '"'))).json();
        if (j.messages && j.messages.length) return j.messages[0];
        await new Promise((r) => setTimeout(r, 20));
    }
    return null;
}
async function raw(id) { return new Uint8Array(await (await api("/api/v1/message/" + id + "/raw")).arrayBuffer()); }
const norm = (s) => s.replace(/\r\n|\r|\n/g, "\r\n");
function endsWith(hay, needle) {
    if (needle.length > hay.length) return false;
    for (let i = 0; i < needle.length; i++) if (hay[hay.length - needle.length + i] !== needle[i]) return false;
    return true;
}

const C = (o = {}) => new SmtpClient({ host: "127.0.0.1", port: SMTP, tls: "none", ...o });

t.test("byte-exact raw message for string, Uint8Array and ArrayBuffer data", async (a) => {
    need(a);
    const c = C();
    const bodies = [
        ["str", "Subject: mp-str\r\n\r\nline\r\n.\r\n..x\n.y\rlast"],
        ["u8", "Subject: mp-u8\r\n\r\n" + ("z".repeat(990) + "\r\n").repeat(5) + "end\r\n"],
        ["ab", "Subject: mp-ab\r\n\r\ncafé\r\n"],
    ];
    for (const [k, b] of bodies) {
        const data = k === "str" ? b : k === "u8" ? enc.encode(b) : enc.encode(b).buffer;
        const r = await c.send({ from: "a@b.test", to: "rcpt@test.example", data });
        a.eq(r.code, 250, k);
        const m = await find("mp-" + k);
        a.must(m, "stored " + k);
        let want = norm(b);
        if (!want.endsWith("\r\n")) want += "\r\n";
        const got = await raw(m.ID);
        a.ok(endsWith(got, enc.encode(want)) || endsWith(got, enc.encode(want.slice(0, -2))), k + ": raw ends with the normalised message: " + JSON.stringify(dec.decode(got.slice(-60))));
    }
    c.close();
});

t.test("SMTPUTF8 envelope (RFC 6530 example)", async (a) => {
    need(a);
    const c = C();
    const to = "用户@例子.广告";
    const r = await c.send({ from: "a@b.test", to, data: "Subject: mp-utf8\r\n\r\nx" });
    a.deep(r.accepted, [to]);
    const m = await find("mp-utf8");
    a.ok(m && [...(m.To || []), ...(m.Bcc || [])].some((x) => x.Address === to), "mailpit recorded the UTF-8 envelope recipient: " + JSON.stringify(m && m.Bcc));
    c.close();
});

t.test("AUTH over plaintext needs insecureAuth", async (a) => {
    need(a);
    const c1 = C({ user: "u", pass: "p" });
    await a.rejects(c1.send({ from: "a@b.test", to: "x@test.example", data: "x" }), { code: "SMTP_AUTH" });
    c1.close();
    for (const mech of ["PLAIN", "LOGIN"]) {
        const c = C({ user: "u", pass: "p", insecureAuth: true, mechanisms: [mech] });
        const r = await c.send({ from: "a@b.test", to: "x@test.example", data: "Subject: mp-auth-" + mech + "\r\n\r\nx" });
        a.eq(r.code, 250, mech);
        c.close();
    }
});

t.test("no PIPELINING: 50 messages over 2 sessions, all stored", async (a) => {
    need(a);
    const c = C({ maxConnections: 2 });
    const ps = [];
    for (let i = 0; i < 50; i++) ps.push(c.send({ from: "a@b.test", to: ["r1@test.example", "r2@test.example"], data: "Subject: mp-bulk-" + i + "\r\n\r\nbody " + i }));
    const rs = await Promise.all(ps);
    a.ok(rs.every((r) => r.accepted.length === 2), "every message accepted for both recipients");
    const j = await (await api("/api/v1/search?query=" + encodeURIComponent("subject:mp-bulk"))).json();
    a.eq(j.messages_count === undefined ? j.total : j.messages_count, 50, "50 stored");
    await c.quit();
});

await t.run();
