// covers: integration dyna:dkim.sign dyna:dkim.verify -- every case signs and then verifies the same message
// test_dkim_rfc.js -- dyna:dkim against published vectors (S9 M3, 2026-11-27).
// RFC 8463 Appendix A: the doubly signed "Is dinner ready?" message verifies with both published keys
// (ed25519 brisbane, rsa-sha256 test); one changed header or body byte turns each into fail. The RFC 8032
// test-1 seed signs a message whose signature verifies against the RFC 8463 p=, so seed -> public key
// matches. RFC 6376 3.4.5's canonicalization example round-trips under all four c= pairs, and a relaxed
// signature survives the whitespace changes 3.4.2/3.4.4 allow while a simple one does not. Bare-LF and
// bare-CR input signs as CRLF (what SmtpClient sends).
import { suite } from "../lib/kit.js";
import { sign, verify } from "dyna:dkim";
import { Ed25519PemFromRaw, RSA } from "dyna:crypto";

const t = suite("test_dkim_rfc");

const ED = "DKIM-Signature: v=1; a=ed25519-sha256; c=relaxed/relaxed;\r\n d=football.example.com; i=@football.example.com;\r\n q=dns/txt; s=brisbane; t=1528637909; h=from : to :\r\n subject : date : message-id : from : subject : date;\r\n bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=;\r\n b=/gCrinpcQOoIfuHNQIbq4pgh9kyIK3AQUdt9OdqQehSwhEIug4D11Bus\r\n Fa3bT3FY5OsU7ZbnKELq+eXdp1Q1Dw==\r\n";
const RS = "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed;\r\n d=football.example.com; i=@football.example.com;\r\n q=dns/txt; s=test; t=1528637909; h=from : to : subject :\r\n date : message-id : from : subject : date;\r\n bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=;\r\n b=F45dVWDfMbQDGHJFlXUNB2HKfbCeLRyhDXgFpEL8GwpsRe0IeIixNTe3\r\n DhCVlUrSjV4BwcVcOF6+FF3Zo9Rpo1tFOeS9mPYQTnGdaSGsgeefOsk2Jz\r\n dA+L10TeYt9BgDfQNZtKdN1WO//KgIqXP7OdEFE4LjFYNcUxZQ4FADY+8=\r\n";
const HDRS = "From: Joe SixPack <joe@football.example.com>\r\nTo: Suzie Q <suzie@shopping.example.net>\r\nSubject: Is dinner ready?\r\nDate: Fri, 11 Jul 2003 21:00:37 -0700 (PDT)\r\nMessage-ID: <20030712040037.46341.5F8J@football.example.com>\r\n";
const BODY = "\r\nHi.\r\n\r\nWe lost the game.  Are you hungry yet?\r\n\r\nJoe.\r\n";
const MSG = HDRS + BODY;
const KEYS = {
    "brisbane._domainkey.football.example.com": "v=DKIM1; k=ed25519; p=11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=",
    "test._domainkey.football.example.com": "v=DKIM1; k=rsa; p=MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDkHlOQoBTzWRiGs5V6NpP3idY6Wk08a5qhdR6wy5bdOKb2jLQiY/J16JYi0Qvx/byYzCNb3W91y3FutACDfzwQ/BC/e/8uBsCR+yz1Lxj+PL6lHvqMKrM3rG4hstT5QjvHO9PzoxZyVYLzBfO2EeC3Ip3G+2kryOTIKT+l/K4w3QIDAQAB",
};
const lookup = (n) => KEYS[n] ?? null;
const hex = (s) => new Uint8Array(s.match(/../g).map((h) => parseInt(h, 16)));
const SEED = hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
const st = (rs) => rs.map((r) => r.status).join(",");

t.test("RFC 8463 A.3: both published signatures pass", async (a) => {
    const rs = await verify(ED + RS + MSG, { lookup });
    a.eq(st(rs), "pass,pass");
    a.eq(rs[0].algorithm, "ed25519-sha256");
    a.eq(rs[1].algorithm, "rsa-sha256");
    a.eq(rs[0].identity, "@football.example.com");
    a.deep(rs[0].signedHeaders, ["from", "to", "subject", "date", "message-id", "from", "subject", "date"]);
    a.eq(rs[0].timestamp, 1528637909);
    const asBytes = await verify(new TextEncoder().encode(ED + RS + MSG), { lookup });
    a.eq(st(asBytes), "pass,pass", "Uint8Array input");
});

t.test("RFC 8463: a changed body or header byte fails both", async (a) => {
    a.eq(st(await verify(ED + RS + MSG.replace("hungry", "Hungry"), { lookup })), "fail,fail");
    const r = await verify(ED + RS + MSG.replace("Is dinner", "Is lunch"), { lookup });
    a.eq(st(r), "fail,fail");
    a.match(r[0].reason, /signature did not verify/);
    const b = await verify(ED + RS + MSG.replace("Joe.", "Joe!"), { lookup });
    a.match(b[0].reason, /body hash/);
    a.eq(st(await verify(ED + RS + MSG.replace("Subject: Is", "Subject:   Is"), { lookup })), "pass,pass", "relaxed ignores WSP runs");
    a.eq(st(await verify(ED + RS + MSG.replace("\r\nTo:", "\r\nTo:\r\n  "), { lookup })), "pass,pass", "relaxed ignores refolding");
    a.eq(st(await verify(ED + RS + HDRS + BODY + "\r\n\r\n\r\n", { lookup })), "pass,pass", "trailing empty lines ignored");
    a.eq(st(await verify(ED + RS + "X-Added: yes\r\n" + MSG, { lookup })), "pass,pass", "unsigned header added");
});

t.test("RFC 8032 seed signs what the RFC 8463 p= verifies", async (a) => {
    const f = await sign(MSG, { domain: "football.example.com", selector: "brisbane", privateKey: SEED, timestamp: 1528637909 });
    a.match(f, /^DKIM-Signature: v=1; a=ed25519-sha256; c=relaxed\/relaxed;\r\n d=football\.example\.com; s=brisbane; t=1528637909;/);
    a.match(f, /bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=;/, "RFC 8463 body hash");
    a.ok(f.endsWith("\r\n") && f.split("\r\n").every((l) => l.length <= 78), "folded <= 78 columns");
    a.eq(st(await verify(f + MSG, { lookup })), "pass");
    const pem = Ed25519PemFromRaw({ privateKey: SEED }).privateKey;
    const g = await sign(MSG, { domain: "football.example.com", selector: "brisbane", privateKey: pem, timestamp: 1528637909 });
    a.eq(g, f, "PEM and raw seed give the same deterministic Ed25519 field");
});

const EX = "A: X\r\nB : Y\t\r\n\tZ  \r\nFrom: f@example.com\r\n\r\n C \r\nD \t E\r\n\r\n\r\n";
const RK = RSA.generate(2048);
const SPKI = RK.publicKey.replace(/-----[^-]+-----|\s/g, "");
const RKEYS = { "s1._domainkey.example.com": "v=DKIM1; p=" + SPKI };
const rlookup = (n) => RKEYS[n] ?? null;

t.test("RFC 6376 3.4.5 example: every c= pair round-trips; simple rejects what relaxed allows", async (a) => {
    for (const c of ["simple/simple", "simple/relaxed", "relaxed/simple", "relaxed/relaxed", "simple", "relaxed"]) {
        const f = await sign(EX, { domain: "example.com", selector: "s1", privateKey: RK.privateKey, canonicalization: c, headers: ["from", "a", "b"] });
        const want = c.includes("/") ? c : c + "/simple";
        a.match(f, new RegExp("c=" + want.replace("/", "\\/") + ";"), c);
        a.eq(st(await verify(f + EX, { lookup: rlookup })), "pass", c);
        const ws = EX.replace("B : Y", "B :  Y").replace(" C \r\n", " C  \r\n");
        const exp = (want.startsWith("relaxed") ? "" : "h") + (want.endsWith("relaxed") ? "" : "b");
        const r = (await verify(f + ws, { lookup: rlookup }))[0];
        a.eq(r.status, exp ? "fail" : "pass", c + " after WSP changes");
    }
});

t.test("bare LF and bare CR input sign as CRLF", async (a) => {
    const crlf = await sign(MSG, { domain: "example.com", selector: "s1", privateKey: RK.privateKey, timestamp: 1, canonicalization: "simple/simple" });
    const lf = await sign(MSG.replace(/\r\n/g, "\n"), { domain: "example.com", selector: "s1", privateKey: RK.privateKey, timestamp: 1, canonicalization: "simple/simple" });
    const cr = await sign(MSG.replace(/\r\n/g, "\r"), { domain: "example.com", selector: "s1", privateKey: RK.privateKey, timestamp: 1, canonicalization: "simple/simple" });
    a.eq(lf, crlf, "LF");
    a.eq(cr, crlf, "CR");
    a.eq(st(await verify(lf + MSG, { lookup: rlookup })), "pass");
    a.eq(st(await verify(crlf + MSG.replace(/\r\n/g, "\n"), { lookup: rlookup })), "pass", "verify LF form");
});

t.test("empty body: RFC 6376 3.4.3/3.4.4 hashes", async (a) => {
    const h = "From: a@example.com\r\n\r\n";
    const s = await sign(h, { domain: "example.com", selector: "s1", privateKey: RK.privateKey, canonicalization: "simple/simple" });
    a.match(s, /bh=frcCV1k9oG9oKj3dpUqdJg1PxRT2RSN\/XKdLCPjaYaY=;/);
    const r = await sign("From: a@example.com\r\n", { domain: "example.com", selector: "s1", privateKey: RK.privateKey });
    a.match(r, /bh=47DEQpj8HBSa\+\/TImW\+5JCeuQeRkm5NMpJWZG3hSuFU=;/, "no body at all, relaxed");
    a.eq(st(await verify(s + h, { lookup: rlookup })), "pass");
});
await t.run();
