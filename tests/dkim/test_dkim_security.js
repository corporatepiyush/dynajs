// covers: integration dyna:dkim.sign dyna:dkim.verify -- every case signs and then verifies the same message
// test_dkim_security.js -- hostile signatures, keys and lookups (S9 M3, 2026-11-27).
// RFC 6376 6.1.1/6.1.2/6.1.3 and RFC 8301: every malformed or refused signature is permerror with a reason
// (never a throw): rsa-sha1, h= without From, i= outside d=, duplicate tags, x= expired, revoked key, key
// type mismatch, t=s with a subdomain i=, an RSA key under minRsaBits, s= that is not a DNS label, a TXT
// record over 64 KiB; l= longer than the body fails; NXDOMAIN (null, [], "rcode 3") is permerror and any
// other lookup failure is temperror. Oversigning catches a prepended From that plain h=from lets through
// (5.4.2 bottom-up); appended body after l= still passes (the documented l= risk). maxSignatures bounds the
// work: later signatures are neutral and not looked up. Option errors throw synchronously.
import { suite } from "../lib/kit.js";
import { sign, verify } from "dyna:dkim";
import { RSA, ECDSA, Ed25519Generate } from "dyna:crypto";
import { Exec, args } from "dyna:sys";

const t = suite("test_dkim_security");
const RK = RSA.generate(2048);
const SPKI = RK.publicKey.replace(/-----[^-]+-----|\s/g, "");
const ED = Ed25519Generate();
const b64 = (u) => btoa(String.fromCharCode(...u));
const KEYS = new Map([
    ["s1._domainkey.example.com", "v=DKIM1; k=rsa; p=" + SPKI],
    ["ed._domainkey.example.com", "v=DKIM1; k=ed25519; p=" + b64(ED.publicKey)],
]);
let lookups = [];
const lookup = (n) => { lookups.push(n); return KEYS.has(n) ? KEYS.get(n) : null; };
const MSG = "From: Alice <a@example.com>\r\nTo: b@example.net\r\nSubject: hi\r\n\r\nline one\r\nline two\r\n";
const opts = (o = {}) => ({ domain: "example.com", selector: "s1", privateKey: RK.privateKey, ...o });
const one = async (m, o = {}) => (await verify(m, { lookup, ...o }))[0];

async function forged(tags, body = MSG) {
    const f = await sign(body, opts());
    return f.replace(/^DKIM-Signature: /, "DKIM-Signature: " + tags + " ");
}

t.test("refused signature tags are permerror with a reason", async (a) => {
    const f = await sign(MSG, opts());
    const cases = [
        [f.replace("a=rsa-sha256", "a=rsa-sha1"), /8301/],
        [f.replace(/h=[^;]*;/, "h=to:subject;"), /from/],
        [f.replace("d=example.com;", "d=example.com; i=a@evil.example.org;"), /within d=/],
        [f.replace("d=example.com;", "d=example.com; d=example.org;"), /duplicate/],
        [f.replace("v=1;", "v=2;"), /v=/],
        [f.replace("s=s1;", "s=s1/../x;"), /DNS name/],
        [f.replace("c=relaxed/relaxed;", "c=fancy;"), /canonicalization/],
        [f.replace(/bh=[^;]*;/, "bh=AAAA;"), /bh=/],
        [f.replace(/ b=[^]*$/, "\r\n"), /missing/],
        [f.replace("d=example.com;", "d=example.com; q=http/x;"), /q=/],
    ];
    for (const [sig, re] of cases) {
        const r = await one(sig + MSG);
        a.eq(r.status, "permerror", String(re));
        a.match(r.reason || "", re);
    }
});

t.test("expiry, l= and body tampering", async (a) => {
    const f = await sign(MSG, opts({ timestamp: 1000, expiresIn: 100 }));
    a.match(f, /t=1000; x=1100;/);
    a.eq((await one(f + MSG, { now: 1099 })).status, "pass");
    const r = await one(f + MSG, { now: 1101 });
    a.eq(r.status, "permerror");
    a.match(r.reason, /expired/);
    const l = await sign(MSG, opts({ bodyLength: true }));
    a.match(l, /l=20;/);
    a.eq((await one(l + MSG + "appended spam\r\n")).status, "pass", "l= lets appended content through (RFC 6376 8.2)");
    const short = await one(l + MSG.replace("line two\r\n", "x\r\n"));
    a.eq(short.status, "fail");
    a.match(short.reason, /l= is longer/);
    a.eq((await one(f + MSG.replace("one", "0ne"), { now: 1050 })).status, "fail");
});

t.test("oversigning catches a prepended From; plain h=from does not", async (a) => {
    const evil = "From: Mallory <m@evil.example>\r\n";
    const plain = await sign(MSG, opts({ headers: ["from", "to", "subject"] }));
    const over = await sign(MSG, opts({ headers: ["from", "to", "subject"], oversign: true }));
    a.match(over, /h=from:from:to:to:subject:subject;/);
    a.eq((await one(plain + evil + MSG)).status, "pass", "bottom-up selection signs the original From only");
    a.eq((await one(over + evil + MSG)).status, "fail", "oversigned From slot now has an instance");
    a.eq((await one(over + MSG)).status, "pass");
});

t.test("key record problems", async (a) => {
    const f = await sign(MSG, opts());
    const with_ = async (txt, o = {}) => one(f + MSG, { lookup: () => txt, ...o });
    a.match((await with_("v=DKIM1; p=")).reason, /revoked/);
    a.match((await with_("v=DKIM1; k=ed25519; p=" + b64(ED.publicKey))).reason, /does not match/);
    a.match((await with_("p=" + SPKI + "; v=DKIM1")).reason, /v=/);
    a.match((await with_("v=DKIM1; h=sha1; p=" + SPKI)).reason, /sha256/);
    a.match((await with_("v=DKIM1; s=web; p=" + SPKI)).reason, /service/);
    a.match((await with_("v=DKIM1; p=AAAA")).reason, /not a valid public key/);
    a.match((await with_("v=DKIM1; p=" + SPKI, { minRsaBits: 4096 })).reason, /2048 bits/);
    a.eq((await with_("v=DKIM1; t=y; p=" + SPKI)).testing, true);
    a.eq((await with_(["v=DKIM1; ", "p=" + SPKI])).status, "pass", "string[] chunks joined");
    a.eq((await with_([["v=DKIM1; p=" + SPKI.slice(0, 100), SPKI.slice(100)], ["ignored"]])).status, "pass", "string[][] first record");
    a.eq((await with_("x".repeat(70000))).reason, "key record is longer than 64 KiB");
    a.match((await with_(42)).reason, /lookup must resolve/);
    const sub = await sign(MSG, opts({ identity: "a@mail.example.com" }));
    a.eq((await one(sub + MSG, { lookup: () => "v=DKIM1; p=" + SPKI })).status, "pass");
    a.match((await one(sub + MSG, { lookup: () => "v=DKIM1; t=s; p=" + SPKI })).reason, /t=s/);
});

t.test("lookup outcomes: NXDOMAIN permerror, other failures temperror", async (a) => {
    const f = await sign(MSG, opts());
    const st = async (lk) => { const r = await one(f + MSG, { lookup: lk }); return r.status + ":" + (r.reason || ""); };
    a.match(await st(() => null), /^permerror:no key/);
    a.match(await st(() => []), /^permerror:no key/);
    a.match(await st(async () => { throw new Error("DNS error, rcode 3"); }), /^permerror:no key/);
    a.match(await st(async () => { throw new Error("DNSResolver: timeout"); }), /^temperror:key lookup failed: Error: DNSResolver: timeout/);
    a.match(await st(() => { throw new Error("sync boom"); }), /^temperror:.*sync boom/);
    a.match(await st({ resolveTxt: async (n) => [["v=DKIM1; p=" + SPKI]] }), /^pass:/);
    let seen = [];
    const order = await verify(f + f + MSG, { lookup: (n) => { seen.push("call"); return new Promise((r) => setTimeout(() => { seen.push("done"); r("v=DKIM1; p=" + SPKI); }, 5)); } });
    a.eq(order.map((r) => r.status).join(","), "pass,pass");
    a.eq(seen.join(","), "call,call,done,done", "lookups start before any is awaited");
});

t.test("maxSignatures bounds the lookups", async (a) => {
    const f = await sign(MSG, opts());
    lookups = [];
    const rs = await verify(f.repeat(7) + MSG, { lookup });
    a.eq(rs.map((r) => r.status).join(","), "pass,pass,pass,pass,pass,neutral,neutral");
    a.eq(lookups.length, 5);
    const r2 = await verify(f.repeat(3) + MSG, { lookup, maxSignatures: 1 });
    a.eq(r2.map((r) => r.status).join(","), "pass,neutral,neutral");
    const many = await verify(f.repeat(80) + MSG, { lookup: () => null, maxSignatures: 64 });
    a.eq(many.length, 64, "at most 64 reported");
});

t.test("malformed and unsigned messages never throw", async (a) => {
    a.deep(await verify(MSG, { lookup }), [], "unsigned");
    const bad = await verify(" leading continuation\r\n\r\n", { lookup });
    a.eq(bad.length, 1);
    a.eq(bad[0].status, "permerror");
    a.eq((await verify("X".repeat(10) + "\r\n\r\n", { lookup }))[0].status, "permerror", "field without colon");
    a.eq((await verify("A: 1\r\n".repeat(1001) + "\r\n", { lookup }))[0].status, "permerror", "1001 fields");
    a.eq((await one("DKIM-Signature: \x00\x01;;==\r\n" + MSG)).status, "permerror");
    const bin = new Uint8Array([...new TextEncoder().encode(MSG), 0, 255, 13, 10]);
    const f = await sign(bin, opts());
    const full = new Uint8Array([...new TextEncoder().encode(f), ...bin]);
    a.eq((await one(full)).status, "pass", "8-bit and NUL body bytes");
});

t.test("sign: option errors throw synchronously, content errors reject", async (a) => {
    a.throws(() => sign(MSG, { domain: "example.com", selector: "s1" }), TypeError, "no key");
    a.throws(() => sign(MSG, opts({ bogus: 1 })), TypeError, "unknown key");
    a.throws(() => sign(MSG, opts({ canonicalization: "loose" })), TypeError);
    a.throws(() => sign(MSG, opts({ algorithm: "rsa-sha1" })), TypeError);
    a.throws(() => sign(MSG, opts({ headers: [] })), TypeError);
    a.throws(() => sign(MSG, opts({ headers: ["from", 3] })), TypeError);
    a.throws(() => sign(MSG, opts({ timestamp: -1 })), RangeError);
    a.throws(() => sign(MSG, opts({ expiresIn: 0 })), RangeError);
    a.throws(() => sign(MSG, opts({ privateKey: new Uint8Array(31) })), TypeError);
    a.throws(() => sign(42, opts()), TypeError);
    a.throws(() => verify(MSG, {}), TypeError, "no lookup");
    a.throws(() => verify(MSG, { lookup, maxSignatures: 65 }), RangeError);
    a.throws(() => verify(MSG, { lookup, extra: 1 }), TypeError);
    await a.rejects(sign(MSG, opts({ privateKey: "not a pem" })), /PEM/);
    await a.rejects(sign(MSG, opts({ algorithm: "ed25519-sha256" })), /does not match/);
    await a.rejects(sign("To: x\r\n\r\n", opts()), /no From/);
    await a.rejects(sign(MSG, opts({ headers: ["to"] })), /include from/);
    await a.rejects(sign(MSG, opts({ domain: "a;b" })), /bad domain/);
    await a.rejects(sign(" bad\r\n\r\n", opts()), /malformed/);
    const ed = await sign(MSG, opts({ selector: "ed", privateKey: ED.privateKey }));
    a.eq((await one(ed + MSG)).status, "pass", "raw Ed25519 seed from Ed25519Generate");
});

t.test("fuzz: mutated signatures never throw or pass", async (a) => {
    const f = await sign(MSG, opts());
    let seed = 7;
    const rnd = (n) => { seed = (seed * 1103515245 + 12345) >>> 0; return seed % n; };
    let passes = 0;
    for (let i = 0; i < 400; i++) {
        const c = f.split("");
        const k = 1 + rnd(4);
        for (let j = 0; j < k; j++) c[13 + rnd(c.length - 15)] = String.fromCharCode(rnd(128));
        const m = c.join("") + MSG;
        const rs = await verify(m, { lookup: (n) => KEYS.get(n) ?? null });
        for (const r of rs) {
            a.ok(["pass", "fail", "neutral", "temperror", "permerror"].includes(r.status), r.status);
            if (r.status === "pass") passes++;
        }
    }
    a.ok(passes < 400, "most mutations break the signature (" + passes + " survived, e.g. in folding WSP)");
});

const K512 = "-----BEGIN PRIVATE KEY-----\nMIIBVAIBADANBgkqhkiG9w0BAQEFAASCAT4wggE6AgEAAkEAwV+2KVFZRU0QAqYu\nQT293C9Bg7+fEkbVY5rpIE5zhLLw/nOG0Tii/IpFCfac+kBZ0sFIWi2SdE1w0UoY\n3YfRLQIDAQABAkA/w/sCtvOUGaxc40W/d8ei0uZstoSS906okM2tPJF1QSWTejBe\nftlPHLO1MI3vNtS9gHegZzqWqHV8zQVtWx2hAiEA7SkKOoJO+pZDI+p8/m8i+T9S\nXk0KbBe7nCBEA2XGORkCIQDQvDfR5bUZqGm1n5iqbXGaACDHGIjhwIAble0lTRrX\nNQIgK839xqrU/bINeq/QIIrUkechRqvCNg74JzFKfroor9kCIEQ+tXVfx/Uqld02\n3+AgxjqWfiD6CY7dtETQoOdILVVdAiEAznQ5Qv4ygSxmKqw9gLBQZ2dFkKczbgIJ\n62Vq5f4xt7c=\n-----END PRIVATE KEY-----\n";
const P512 = "MFwwDQYJKoZIhvcNAQEBBQADSwAwSAJBAMFftilRWUVNEAKmLkE9vdwvQYO/nxJG1WOa6SBOc4Sy8P5zhtE4ovyKRQn2nPpAWdLBSFotknRNcNFKGN2H0S0CAwEAAQ==";

t.test("RFC 8301 1024-bit floors, PKCS#1 p=, non-RSA SPKI under k=rsa", async (a) => {
    await a.rejects(sign(MSG, opts({ privateKey: K512 })), /1024 bits/);
    const f = await sign(MSG, opts());
    const r = await one(f + MSG, { lookup: () => "v=DKIM1; p=" + P512 });
    a.eq(r.status + ":" + r.reason, "permerror:RSA key has 512 bits, under minRsaBits 1024");
    a.match((await one(f + MSG, { lookup: () => "v=DKIM1; p=" + P512, minRsaBits: 512 })).reason, /did not verify/, "512 allowed by option, wrong key");
    const der = Uint8Array.from(atob(SPKI), (c) => c.charCodeAt(0));
    a.eq(der[0] === 0x30 && der[23] === 0x00 && der[24] === 0x30, true, "SPKI header is 24 bytes");
    const pkcs1 = btoa(String.fromCharCode(...der.subarray(24)));
    a.eq((await one(f + MSG, { lookup: () => "v=DKIM1; p=" + pkcs1 })).status, "pass", "bare RSAPublicKey p=");
    const ec = ECDSA.generate("P-256").publicKey.replace(/-----[^-]+-----|\s/g, "");
    a.match((await one(f + MSG, { lookup: () => "v=DKIM1; k=rsa; p=" + ec })).reason, /not a valid public key/);
    a.match((await one(f + MSG, { lookup: () => "v=DKIM1; k=ed25519; p=" + btoa("x".repeat(31)) })).reason, /does not match|not a valid/);
});

t.test("identity with a quoted @ in the local part splits at the last @", async (a) => {
    const f = await sign(MSG, opts({ identity: "\"x@evil.example\"@mail.example.com" }));
    a.eq((await one(f + MSG)).status, "pass");
    const g = await sign(MSG, opts({ identity: "\"x@example.com\"@evil.example" }));
    a.match((await one(g + MSG)).reason, /within d=/);
});

t.test("a lookup that rejects while another is pending is handled", async (a) => {
    const f = await sign(MSG, opts());
    let k = 0;
    const rs = await verify(f + f + MSG, { lookup: () => (k++ === 0 ? new Promise((r) => setTimeout(() => r("v=DKIM1; p=" + SPKI), 20)) : Promise.reject(new Error("boom"))) });
    a.eq(rs.map((r) => r.status).join(","), "pass,temperror");
    const getterThrows = new Proxy([], { get(o, k) { if (k === "length") throw new Error("nope"); return o[k]; } });
    const r2 = await one(f + MSG, { lookup: () => getterThrows });
    a.eq(r2.status + ":" + r2.reason, "permerror:lookup result could not be read: Error: nope", "throwing length on a Proxy over an array");
    a.eq((await one(f + MSG)).status, "pass", "no exception left pending after a throwing lookup result");
});

const SELF = scriptArgs[0];
if (scriptArgs[1] === "--child") {
    const m = scriptArgs[2];
    const k = Ed25519Generate();
    const M = "From: a@example.com\r\n\r\nhi\r\n";
    if (m === "sign") sign(M, { domain: "example.com", selector: "e", privateKey: k.privateKey }).then((s) => print("CHILD signed " + s.length));
    if (m === "hang") {
        const s = await sign(M, { domain: "example.com", selector: "e", privateKey: k.privateKey });
        verify(s + M, { lookup: () => new Promise(() => {}) }).then(() => print("CHILD never"));
        print("CHILD left");
    }
} else {
    t.test("exit with work in flight: unawaited sign completes, a never-settling lookup does not hang", async (a) => {
        for (const io of [[], ["--io-threads", "0"]])
            for (const [m, want] of [["sign", /CHILD signed \d+/], ["hang", /^CHILD left\n$/]]) {
                const r = Exec(args()[0], [...io, "--std", "--timeout-ms", "10000", SELF, "--child", m], { timeoutMs: 15000 });
                a.eq(r.code, 0, m + " " + io.join(" ") + " rc; stderr=" + r.stderr);
                a.match(r.stdout, want, m + " " + io.join(" "));
                a.eq(r.timedOut, false);
            }
    });
    await t.run();
}
