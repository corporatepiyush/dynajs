// covers: integration dyna:dkim.sign dyna:dkim.verify dyna:smtp.SmtpClient dyna:smtp.SmtpServer -- sign, submit, receive and verify end to end
// test_dkim_e2e.js -- sign, submit with dyna:smtp SmtpClient, receive with SmtpServer, verify in onData
// (S9 M3, 2026-11-27). The signed bytes are the ones the client puts on the wire after RFC 5321 dot-stuffing
// is undone: DATA (with lines starting "." and bare LF that the client turns into CRLF) and BDAT
// (RFC 3030, no stuffing) both deliver a message whose rsa-sha256 and ed25519-sha256 signatures pass, and
// whose body changed in onData fails. 50 concurrent messages verify while the server keeps accepting.
import { suite } from "../lib/kit.js";
import { sign, verify } from "dyna:dkim";
import { RSA, Ed25519Generate } from "dyna:crypto";
import { SmtpClient, SmtpServer } from "dyna:smtp";

const t = suite("test_dkim_e2e");
const RK = RSA.generate(2048);
const ED = Ed25519Generate();
const KEYS = {
    "r._domainkey.example.com": "v=DKIM1; p=" + RK.publicKey.replace(/-----[^-]+-----|\s/g, ""),
    "e._domainkey.example.com": "v=DKIM1; k=ed25519; p=" + btoa(String.fromCharCode(...ED.publicKey)),
};
const lookup = async (n) => KEYS[n] ?? null;
const servers = [];
t.after(() => { for (const s of servers) if (!s.closed) s.close(); });

async function roundtrip(bdat, n) {
    const got = [];
    const s = new SmtpServer({ port: 0, tls: "none", name: "mx.test", maxMessagesPerSession: 1000,
        onData(sess, m) { got.push(verify(m.data, { lookup }).then((rs) => ({ rs, tampered: null, data: m.data }))); } });
    s.listen();
    servers.push(s);
    const c = new SmtpClient({ host: "127.0.0.1", port: s.port, tls: "none", maxConnections: 4, maxMessagesPerConnection: 0 });
    const sends = [];
    for (let i = 0; i < n; i++) {
        const msg = "From: a@example.com\r\nTo: b@example.net\r\nSubject: e2e " + i + "\r\n\r\n.leading dot line\r\nbare lf\nlast " + i + "\r\n";
        const sig = (await sign(msg, { domain: "example.com", selector: "r", privateKey: RK.privateKey }))
            + (await sign(msg, { domain: "example.com", selector: "e", privateKey: ED.privateKey }));
        sends.push(c.send({ from: "a@example.com", to: "b@example.net", data: sig + msg, bdat }));
    }
    const rs = await Promise.all(sends);
    await c.quit();
    c.close();
    const vs = await Promise.all(got);
    s.close();
    return { codes: rs.map((r) => r.code), vs };
}

for (const bdat of [false, true])
    t.test((bdat ? "BDAT" : "DATA") + ": signatures survive submission", async (a) => {
        const { codes, vs } = await roundtrip(bdat, 50);
        a.ok(codes.every((c) => c === 250), "all accepted");
        a.eq(vs.length, 50);
        a.ok(vs.every((v) => v.rs.map((r) => r.status).join(",") === "pass,pass"), JSON.stringify(vs.find((v) => v.rs.some((r) => r.status !== "pass"))?.rs));
        const d = vs[0].data.slice();
        d[d.length - 3] ^= 1;
        a.eq((await verify(d, { lookup })).map((r) => r.status).join(","), "fail,fail", "changed body byte");
    });

await t.run();
