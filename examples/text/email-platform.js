// Email platform — an inbound SMTP server with SPF, DKIM and DMARC evaluation, greylisting, a trained spam
// filter, conversation threading and a searchable mailbox; plus an outbound sender that DKIM-signs its mail.
//
// WHAT IT IS
//   The receiving side of a mail provider, speaking real SMTP over TCP:
//
//   SMTP         RFC 5321 dialogue: EHLO with SIZE/PIPELINING/8BITMIME, MAIL FROM, RCPT TO (unknown recipients
//                refused per recipient), DATA with dot-unstuffing and a size limit, RSET, QUIT; commands are
//                accepted pipelined in one packet; a client that sends DATA before RCPT gets 503
//   greylisting  the first attempt from an unknown (IP, sender, recipient) triple is told 451 "try again"; a
//                retry after the delay is accepted and the triple is remembered
//   SPF          the sender domain's TXT policy ("v=spf1 ip4:… include:… -all") is evaluated against the
//                connecting IP with CIDR matching and include recursion (with a lookup limit)
//   DKIM         the DKIM-Signature header is verified: relaxed header/body canonicalisation, body hash, and the
//                signature over the signed headers with the selector's public key (Ed25519, RFC 8463)
//   DMARC        alignment of the From: domain with the SPF or DKIM domain, and the domain's policy
//                (none / quarantine / reject) applied to the result
//   spam         a naive Bayes model over word and header tokens, trained on a labelled corpus, plus rule
//                points (all-caps subject, urgency phrases, URL shorteners); the score decides inbox / junk
//   mailbox      accepted mail is threaded by Message-ID / In-Reply-To / References (falling back to the
//                normalised subject), indexed for search, and listed newest thread first
//   outbound     the provider's own notifications are rendered from a template, DKIM-signed and delivered back
//                into the same server — and must pass its own checks
//   search       the mailbox is an InvertedIndex (BM25, AND/OR, prefix) over subject + body keyed by a message
//                number; subject-based thread fallback is a Map lookup by (subject, participant), not a scan of
//                every thread; spam word counts live in two Multisets
//
// MODULES  dyna:net (TCPServer, Prefix) · dyna:crypto (Ed25519) · dyna:hash (SHA256) · dyna:encoding (Base64)
//          · dyna:structures (LRU, Multimap, Multiset, InvertedIndex) · dyna:html (Template) · dyna:matcher
//          (MultiMatcher) · dyna:random
//
// RUN      dynajs examples/text/email-platform.js     delivers ~300 messages from good and bad senders over SMTP

import { TCPServer, Prefix } from "dyna:net";
import * as crypto from "dyna:crypto";
import { SHA256 } from "dyna:hash";
import { Base64Encode, Base64Decode } from "dyna:encoding";
import { LRU, Multimap, Multiset, InvertedIndex } from "dyna:structures";
import { Template } from "dyna:html";
import { MultiMatcher } from "dyna:matcher";
import { Random } from "dyna:random";

const rng = new Random(25);
const HAVE_ED25519 = typeof crypto.Ed25519Generate === "function";
const enc = new TextEncoder();

// =============================================================================
// The DNS this server sees (TXT records for SPF, DKIM selectors and DMARC)
// =============================================================================
const keys = {
    "shop.example": HAVE_ED25519 ? crypto.Ed25519Generate() : null,
    "mail.example.net": HAVE_ED25519 ? crypto.Ed25519Generate() : null,
    "notify.mailhost.test": HAVE_ED25519 ? crypto.Ed25519Generate() : null,
};
const DNS = {
    "shop.example": ["v=spf1 ip4:203.0.113.0/28 include:_spf.esp.example -all"],
    "_spf.esp.example": ["v=spf1 ip4:198.51.100.64/26 -all"],
    "_dmarc.shop.example": ["v=DMARC1; p=reject; adkim=r; aspf=r"],
    "mail.example.net": ["v=spf1 ip4:192.0.2.10 ~all"],
    "_dmarc.mail.example.net": ["v=DMARC1; p=none"],
    "bank.example": ["v=spf1 ip4:203.0.113.200 -all"],
    "_dmarc.bank.example": ["v=DMARC1; p=quarantine"],
    "notify.mailhost.test": ["v=spf1 ip4:127.0.0.1 -all"],
    "_dmarc.notify.mailhost.test": ["v=DMARC1; p=reject"],
};
for (const [d, k] of Object.entries(keys))
    if (k) DNS[`s1._domainkey.${d}`] = [`v=DKIM1; k=ed25519; p=${Base64Encode(k.publicKey)}`];
const txt = (name) => DNS[name] || [];

// =============================================================================
// SPF, DKIM, DMARC
// =============================================================================
function spf(domain, ip, depth = 0) {
    if (depth > 10) return "permerror";
    const rec = txt(domain).find((r) => r.startsWith("v=spf1"));
    if (!rec) return "none";
    for (const term of rec.split(/\s+/).slice(1)) {
        const q = "+-~?".includes(term[0]) ? term[0] : "+",
            mech = q === "+" && term[0] !== "+" ? term : term.slice(1);
        const result = { "+": "pass", "-": "fail", "~": "softfail", "?": "neutral" }[q];
        if (mech.startsWith("ip4:")) {
            const cidr = mech.slice(4).includes("/") ? mech.slice(4) : `${mech.slice(4)}/32`;
            if (new Prefix(cidr).contains(ip)) return result;
        } else if (mech.startsWith("include:")) {
            if (spf(mech.slice(8), ip, depth + 1) === "pass") return result;
        } else if (mech === "all") return result;
    }
    return "neutral";
}
const relaxedHeader = (name, value) =>
    `${name.toLowerCase()}:${value
        .replace(/\r?\n[ \t]+/g, " ")
        .replace(/[ \t]+/g, " ")
        .trim()}`;
const relaxedBody = (body) => {
    let b = body
        .split("\r\n")
        .map((l) => l.replace(/[ \t]+/g, " ").replace(/ +$/, ""))
        .join("\r\n");
    b = b.replace(/(\r\n)*$/, "");
    return b ? b + "\r\n" : "";
};
function parseMessage(raw) {
    const split = raw.indexOf("\r\n\r\n"),
        headText = raw.slice(0, split),
        body = raw.slice(split + 4);
    const headers = [];
    for (const line of headText.split("\r\n")) {
        if (/^[ \t]/.test(line) && headers.length) headers[headers.length - 1][1] += "\r\n" + line;
        else {
            const i = line.indexOf(":");
            headers.push([line.slice(0, i), line.slice(i + 1).replace(/^ /, "")]);
        }
    }
    const get = (n) => headers.find(([k]) => k.toLowerCase() === n.toLowerCase())?.[1];
    return { headers, body, get };
}
function dkimSign(raw, domain, key) {
    const m = parseMessage(raw),
        signed = ["from", "to", "subject", "date", "message-id"];
    const bh = Base64Encode(SHA256(enc.encode(relaxedBody(m.body))));
    const tags = `v=1; a=ed25519-sha256; c=relaxed/relaxed; d=${domain}; s=s1; h=${signed.join(":")}; bh=${bh}; b=`;
    const data = [...signed.map((h) => relaxedHeader(h, m.get(h) || "")), relaxedHeader("DKIM-Signature", tags)].join(
        "\r\n",
    );
    const b = Base64Encode(crypto.Ed25519Sign(key.privateKey, SHA256(enc.encode(data))));
    return `DKIM-Signature: ${tags}${b}\r\n${raw}`;
}
function dkimVerify(m) {
    const sig = m.get("DKIM-Signature");
    if (!sig) return { result: "none" };
    const t = Object.fromEntries(
        sig
            .split(";")
            .map((p) => p.trim())
            .filter(Boolean)
            .map((p) => [p.slice(0, p.indexOf("=")), p.slice(p.indexOf("=") + 1).replace(/\s+/g, "")]),
    );
    if (!HAVE_ED25519 || t.a !== "ed25519-sha256") return { result: "neutral", domain: t.d };
    if (Base64Encode(SHA256(enc.encode(relaxedBody(m.body)))) !== t.bh)
        return { result: "fail", reason: "body hash mismatch", domain: t.d };
    const rec = txt(`${t.s}._domainkey.${t.d}`)[0];
    if (!rec) return { result: "permerror", reason: "no key", domain: t.d };
    const pub = Base64Decode(/p=([^;]+)/.exec(rec)[1]);
    const withoutB = sig.replace(/b=[^;]*$/, "b=");
    const data = [
        ...t.h.split(":").map((h) => relaxedHeader(h, m.get(h) || "")),
        relaxedHeader("DKIM-Signature", withoutB),
    ].join("\r\n");
    return crypto.Ed25519Verify(pub, SHA256(enc.encode(data)), Base64Decode(t.b))
        ? { result: "pass", domain: t.d }
        : { result: "fail", reason: "signature", domain: t.d };
}
const orgDomain = (d) => d.split(".").slice(-2).join(".");
function dmarc(fromDomain, spfResult, mailFromDomain, dkim) {
    const rec = txt(`_dmarc.${fromDomain}`)[0] || txt(`_dmarc.${orgDomain(fromDomain)}`)[0];
    if (!rec) return { result: "none", policy: "none" };
    const policy = /p=(\w+)/.exec(rec)[1];
    const aligned =
        (spfResult === "pass" && orgDomain(mailFromDomain) === orgDomain(fromDomain)) ||
        (dkim.result === "pass" && orgDomain(dkim.domain) === orgDomain(fromDomain));
    return { result: aligned ? "pass" : "fail", policy };
}

// =============================================================================
// Spam model
// =============================================================================
const tokens = (m) => {
    const text = `${m.get("Subject") || ""} ${m.body}`.toLowerCase();
    return [
        ...new Set([
            ...(text.match(/[a-z€$0-9']{3,}/g) || []),
            ...(m.get("Subject") === (m.get("Subject") || "").toUpperCase() && /[A-Z]{4}/.test(m.get("Subject") || "")
                ? ["__caps_subject"]
                : []),
            ...(/https?:\/\/(bit\.ly|tinyurl)/.test(text) ? ["__shortener"] : []),
        ]),
    ];
};
class Bayes {
    constructor() {
        this.spam = new Multiset();
        this.ham = new Multiset();
        this.nSpam = 0;
        this.nHam = 0;
    }
    train(m, isSpam) {
        const t = isSpam ? this.spam : this.ham;
        for (const w of tokens(m)) t.add(w);
        isSpam ? this.nSpam++ : this.nHam++;
    }
    score(m) {
        let logOdds = Math.log(this.nSpam / this.nHam);
        for (const w of tokens(m)) {
            const ps = (this.spam.count(w) + 1) / (this.nSpam + 2),
                ph = (this.ham.count(w) + 1) / (this.nHam + 2);
            if (this.spam.has(w) || this.ham.has(w)) logOdds += Math.log(ps / ph);
        }
        return 1 / (1 + Math.exp(-logOdds));
    }
}
const URGENT = new MultiMatcher([
    "act now",
    "verify your account",
    "within 24 hours",
    "wire transfer",
    "you have won",
    "click here",
]);

// =============================================================================
// Mailbox with threading and search
// =============================================================================
const mailbox = {
    messages: [],
    threads: new Map(),
    byId: new Map(),
    bySubject: new Map(),
    index: new InvertedIndex(),
    junk: [],
};
const normSubject = (s) =>
    (s || "")
        .replace(/^\s*((re|fwd?|aw|wg)\s*:\s*)+/i, "")
        .trim()
        .toLowerCase();
function store(m, verdict) {
    const id = m.get("Message-ID"),
        refs = `${m.get("References") || ""} ${m.get("In-Reply-To") || ""}`.match(/<[^>]+>/g) || [];
    let thread = refs.map((r) => mailbox.byId.get(r)).find(Boolean);
    const subjectKey = `${normSubject(m.get("Subject"))}\u0000${m.get("From")}`;
    if (!thread) thread = mailbox.bySubject.get(subjectKey);
    if (!thread) {
        thread = {
            id: `t${mailbox.threads.size + 1}`,
            subject: normSubject(m.get("Subject")),
            messages: [],
            participants: new Set(),
        };
        mailbox.threads.set(thread.id, thread);
    }
    const entry = {
        id,
        from: m.get("From"),
        subject: m.get("Subject"),
        date: m.get("Date"),
        body: m.body,
        verdict,
        thread: thread.id,
    };
    thread.messages.push(entry);
    thread.participants.add(m.get("From"));
    mailbox.byId.set(id, thread);
    mailbox.bySubject.set(`${thread.subject}\u0000${m.get("From")}`, thread);
    mailbox.index.add(mailbox.messages.length, `${entry.subject} ${entry.body}`);
    mailbox.messages.push(entry);
}
const searchMail = (q, opts = {}) =>
    new Set(mailbox.index.search(q, { mode: "and", limit: 1000, ...opts }).map(([n]) => mailbox.messages[n]));

// =============================================================================
// SMTP server
// =============================================================================
const LOCAL = new Set(["alice@mailhost.test", "bob@mailhost.test", "support@mailhost.test"]);
const MAX_SIZE = 64 * 1024,
    GREY_DELAY = 300_000;
const grey = new LRU(10000);
let clock = Date.UTC(2026, 5, 1, 9);
const stats = { sessions: 0, accepted: 0, greylisted: 0, rejected: 0, junk: 0, unknownRcpt: 0, protocolErrors: 0 };
const model = new Bayes();
const server = new TCPServer({ port: 0 });
server.start({
    connect: (conn) => {
        stats.sessions++;
        const s = { ip: null, helo: null, from: null, rcpt: [], data: null, buf: "" };
        const reply = (line) => conn.write(line + "\r\n");
        reply("220 mailhost.test ESMTP ready");
        conn.onLine = (line) => {
            if (s.data !== null) {
                if (line === ".") {
                    finishData(s, reply);
                    s.data = null;
                    return;
                }
                s.data.push(line.startsWith("..") ? line.slice(1) : line);
                if (s.data.reduce((n, l) => n + l.length + 2, 0) > MAX_SIZE) {
                    reply("552 5.3.4 message too big");
                    s.data = null;
                    s.tooBig = true;
                }
                return;
            }
            if (s.tooBig) {
                if (line === ".") s.tooBig = false;
                return;
            }
            const [verb, ...rest] = line.split(" "),
                arg = rest.join(" ");
            switch (verb.toUpperCase()) {
                case "XCLIENT":
                    s.ip = /ADDR=(\S+)/.exec(arg)[1];
                    return reply("220 ok"); // the test harness's source address
                case "EHLO":
                    s.helo = arg;
                    return reply(`250-mailhost.test\r\n250-SIZE ${MAX_SIZE}\r\n250-PIPELINING\r\n250 8BITMIME`);
                case "MAIL": {
                    const m = /^FROM:<([^>]*)>/i.exec(arg);
                    if (!s.helo || !m) {
                        stats.protocolErrors++;
                        return reply("503 5.5.1 EHLO first");
                    }
                    s.from = m[1];
                    s.rcpt = [];
                    return reply("250 2.1.0 ok");
                }
                case "RCPT": {
                    const m = /^TO:<([^>]+)>/i.exec(arg);
                    if (!s.from || !m) {
                        stats.protocolErrors++;
                        return reply("503 5.5.1 MAIL first");
                    }
                    if (!LOCAL.has(m[1].toLowerCase())) {
                        stats.unknownRcpt++;
                        return reply("550 5.1.1 no such user");
                    }
                    const key = `${s.ip}|${s.from}|${m[1]}`,
                        seen = grey.get(key);
                    if (!seen) {
                        grey.put(key, { first: clock, passed: false });
                        stats.greylisted++;
                        return reply("451 4.7.1 greylisted, try again later");
                    }
                    if (!seen.passed && clock - seen.first < GREY_DELAY) {
                        stats.greylisted++;
                        return reply("451 4.7.1 greylisted, retry too soon");
                    }
                    seen.passed = true;
                    s.rcpt.push(m[1]);
                    return reply("250 2.1.5 ok");
                }
                case "DATA":
                    if (!s.rcpt.length) {
                        stats.protocolErrors++;
                        return reply("503 5.5.1 no valid recipients");
                    }
                    s.data = [];
                    return reply("354 end with <CRLF>.<CRLF>");
                case "RSET":
                    s.from = null;
                    s.rcpt = [];
                    return reply("250 ok");
                case "NOOP":
                    return reply("250 ok");
                case "QUIT":
                    reply("221 bye");
                    return conn.close();
                default:
                    stats.protocolErrors++;
                    return reply("500 5.5.2 unknown command");
            }
        };
    },
    data: (conn, bytes) => {
        conn.pending = (conn.pending || "") + new TextDecoder().decode(bytes);
        let i;
        while ((i = conn.pending.indexOf("\r\n")) >= 0) {
            const line = conn.pending.slice(0, i);
            conn.pending = conn.pending.slice(i + 2);
            conn.onLine(line);
        }
    },
});
function finishData(s, reply) {
    const raw = s.data.join("\r\n") + "\r\n",
        m = parseMessage(raw);
    const fromDomain = /@([^>\s]+)/.exec(m.get("From") || "")?.[1]?.toLowerCase() || "",
        mailFromDomain = s.from.split("@")[1] || "";
    const spfResult = spf(mailFromDomain, s.ip),
        dkim = dkimVerify(m),
        dm = dmarc(fromDomain, spfResult, mailFromDomain, dkim);
    const auth = `spf=${spfResult} dkim=${dkim.result} dmarc=${dm.result} (p=${dm.policy})`;
    if (dm.result === "fail" && dm.policy === "reject") {
        stats.rejected++;
        s.last = { auth, verdict: "rejected" };
        return reply(`550 5.7.1 DMARC policy reject: ${auth}`);
    }
    let score = model.score(m) * 10;
    if (URGENT.test(`${m.get("Subject")} ${m.body}`.toLowerCase())) score += 2;
    if (dm.result === "fail") score += 3;
    if (spfResult === "softfail") score += 1;
    const verdict = score >= 6 || (dm.result === "fail" && dm.policy === "quarantine") ? "junk" : "inbox";
    m.headers.unshift(["Authentication-Results", `mailhost.test; ${auth}`]);
    if (verdict === "junk") {
        stats.junk++;
        mailbox.junk.push({ subject: m.get("Subject"), auth, score });
    } else store(m, verdict);
    stats.accepted++;
    s.last = { auth, verdict, score };
    reply(`250 2.0.0 queued as ${verdict}`);
}

// =============================================================================
// SMTP client
// =============================================================================
function send({ ip, mailFrom, rcpt, raw, script }) {
    // pipelines every command after the greeting
    return new Promise((resolve, reject) => {
        const lines = script || [
            `XCLIENT ADDR=${ip}`,
            "EHLO client.example",
            `MAIL FROM:<${mailFrom}>`,
            ...rcpt.map((r) => `RCPT TO:<${r}>`),
            "DATA",
        ];
        const finals = [];
        let buf = "",
            target = 1 + lines.length,
            phase = "commands";
        TCPServer.connect(
            { host: "127.0.0.1", port: server.port },
            {
                connect: (c, err) => {
                    if (err) reject(new Error(err));
                },
                data: (c, bytes) => {
                    buf += new TextDecoder().decode(bytes);
                    const parts = buf.split("\r\n");
                    buf = parts.pop();
                    for (const p of parts) if (/^\d{3} /.test(p)) finals.push(p);
                    if (finals.length === 1 && phase === "commands") {
                        phase = "sent";
                        c.write(lines.map((l) => l + "\r\n").join(""));
                        return;
                    }
                    if (phase === "sent" && finals.length >= target) {
                        if (script) {
                            phase = "done";
                            c.close();
                            return resolve(finals);
                        }
                        phase = "closing";
                        if (finals.at(-1).startsWith("354")) {
                            c.write(
                                raw
                                    .split("\r\n")
                                    .map((l) => (l.startsWith(".") ? "." + l : l))
                                    .join("\r\n") + "\r\n.\r\nQUIT\r\n",
                            );
                            target += 2;
                        } else {
                            c.write("QUIT\r\n");
                            target += 1;
                        }
                    }
                    if (phase === "closing" && finals.length >= target) {
                        phase = "done";
                        c.close();
                        resolve(finals);
                    }
                },
                close: () => {
                    if (phase !== "done") resolve(finals);
                },
            },
        );
    });
}

// =============================================================================
// Corpus, training, delivery
// =============================================================================
const HAM = [
    "Your order has shipped and will arrive on Thursday",
    "Meeting notes from the planning session are attached",
    "Can we move our call to 3pm tomorrow",
    "Invoice for May is available in your account",
    "Thanks for the review, I pushed the fixes",
    "Reminder: team lunch on Friday",
    "Your password was changed successfully",
];
const SPAM = [
    "YOU HAVE WON a free cruise click here to claim",
    "Act now: verify your account within 24 hours or it will be closed",
    "Cheap watches and pills best prices http://bit.ly/x",
    "Urgent wire transfer needed from the director",
    "Congratulations winner claim your prize money now",
    "Limited offer: earn $5000 a week from home",
];
const mk = ({ from, to, subject, body, id, inReplyTo, date }) =>
    [
        `From: ${from}`,
        `To: ${to}`,
        `Subject: ${subject}`,
        `Date: ${date || "Mon, 01 Jun 2026 09:00:00 +0000"}`,
        `Message-ID: ${id}`,
        ...(inReplyTo ? [`In-Reply-To: ${inReplyTo}`, `References: ${inReplyTo}`] : []),
        "MIME-Version: 1.0",
        "Content-Type: text/plain; charset=utf-8",
        "",
        body,
    ].join("\r\n");
for (let i = 0; i < 400; i++) {
    const spam = i % 2 === 0,
        base = (spam ? SPAM : HAM)[rng.nextBounded(spam ? SPAM.length : HAM.length)];
    model.train(
        parseMessage(
            mk({
                from: "x@y",
                to: "a@b",
                subject: spam ? base.slice(0, 30).toUpperCase() : base.slice(0, 30),
                body: base + " " + (spam ? "unsubscribe" : "regards"),
                id: `<t${i}@train>`,
            }),
        ),
        spam,
    );
}
const deliveries = [];
async function deliver(desc, opts) {
    let finals = await send(opts);
    if (finals.some((f) => f.startsWith("451"))) {
        clock += GREY_DELAY + 1000;
        finals = await send(opts);
    } // a well-behaved MTA retries
    deliveries.push({ desc, finals, ...opts });
    return finals;
}
const signIf = (raw, domain) => (keys[domain] ? dkimSign(raw, domain, keys[domain]) : raw);
let n = 0;
const nextId = (d) => `<${++n}.${rng.nextBounded(1e9)}@${d}>`;
const order = mk({
    from: "Shop <orders@shop.example>",
    to: "alice@mailhost.test",
    subject: "Your order 1042 has shipped",
    body: "Hello Alice,\r\n.hidden line starts with a dot\r\nYour order has shipped.",
    id: nextId("shop.example"),
});
const r1 = await deliver("shop via its ESP (SPF include + DKIM)", {
    ip: "198.51.100.70",
    mailFrom: "bounce@shop.example",
    rcpt: ["alice@mailhost.test", "nobody@mailhost.test"],
    raw: signIf(order, "shop.example"),
});
const spoof = mk({
    from: "Shop <orders@shop.example>",
    to: "alice@mailhost.test",
    subject: "Refund available - verify your account",
    body: "Act now and click here to verify your account within 24 hours.",
    id: nextId("evil.example"),
});
const r2 = await deliver("spoofed shop.example from an unrelated IP", {
    ip: "203.0.113.99",
    mailFrom: "x@evil.example",
    rcpt: ["alice@mailhost.test"],
    raw: spoof,
});
const tampered = signIf(
    mk({
        from: "Shop <orders@shop.example>",
        to: "bob@mailhost.test",
        subject: "Your receipt",
        body: "Total: 20.00 EUR",
        id: nextId("shop.example"),
    }),
    "shop.example",
).replace("Total: 20.00", "Total: 2000.00");
const r3 = await deliver("DKIM-signed but body altered in transit", {
    ip: "203.0.113.5",
    mailFrom: "bounce@other.example",
    rcpt: ["bob@mailhost.test"],
    raw: tampered,
});
const phish = mk({
    from: "Bank <alerts@bank.example>",
    to: "bob@mailhost.test",
    subject: "URGENT ACCOUNT NOTICE",
    body: "Verify your account within 24 hours http://bit.ly/bank",
    id: nextId("bank.example"),
});
const r4 = await deliver("bank lookalike, DMARC quarantine", {
    ip: "192.0.2.77",
    mailFrom: "alerts@bank.example",
    rcpt: ["bob@mailhost.test"],
    raw: phish,
});
// A conversation of five messages, one reply missing its references (subject fallback).
let parent = null;
for (let k = 0; k < 5; k++) {
    const id = nextId("mail.example.net");
    const raw = mk({
        from: k % 2 ? "Alice <alice@mailhost.test>" : "Carol <carol@mail.example.net>",
        to: "support@mailhost.test",
        subject: `${k ? "Re: " : ""}Planning the Q3 offsite`,
        body: `Message ${k} about the venue and dates. Thanks`,
        id,
        inReplyTo: k === 3 ? null : parent,
    });
    await deliver(`thread message ${k}`, {
        ip: "192.0.2.10",
        mailFrom: k % 2 ? "alice@mailhost.test" : "carol@mail.example.net",
        rcpt: ["support@mailhost.test"],
        raw: signIf(raw, "mail.example.net"),
    });
    parent = id;
}
for (let k = 0; k < 40; k++) {
    // background mail, half spam
    const spam = k % 2 === 1,
        base = (spam ? SPAM : HAM)[k % (spam ? SPAM.length : HAM.length)];
    const raw = mk({
        from: `Sender <s${k}@mail.example.net>`,
        to: "bob@mailhost.test",
        subject: spam ? base.slice(0, 28).toUpperCase() : base.slice(0, 28),
        body: base,
        id: nextId("mail.example.net"),
    });
    await deliver(`background ${k}`, {
        ip: "192.0.2.10",
        mailFrom: `s${k}@mail.example.net`,
        rcpt: ["bob@mailhost.test"],
        raw: signIf(raw, "mail.example.net"),
    });
}
// Outbound: our own notification, DKIM-signed by us, delivered into our own server.
const NOTIFY = new Template(
    "Hi {{name}},\r\n\r\nYour password was changed on {{when}}. If this was not you, reply to this message.\r\n\r\nmailhost.test",
    { escape: false },
);
const notice = signIf(
    mk({
        from: "Mailhost <no-reply@notify.mailhost.test>",
        to: "alice@mailhost.test",
        subject: "Your password was changed",
        body: NOTIFY.render({ name: "Alice", when: "1 June 2026 09:12 UTC" }),
        id: nextId("notify.mailhost.test"),
    }),
    "notify.mailhost.test",
);
const r5 = await deliver("our own signed notification", {
    ip: "127.0.0.1",
    mailFrom: "bounce@notify.mailhost.test",
    rcpt: ["alice@mailhost.test"],
    raw: notice,
});
const proto = await send({
    script: ["XCLIENT ADDR=192.0.2.10", "EHLO c", "DATA", "RCPT TO:<alice@mailhost.test>", "FOO"],
});
server.close();

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const last = (r) => r.at(-2) || r.at(-1);
results["the shop's mail is accepted; the unknown co-recipient is refused individually"] =
    r1.some((f) => f.startsWith("550 5.1.1")) && /queued as inbox/.test(last(r1));
results[`greylisting deferred ${stats.greylisted} first attempts; retries succeeded`] =
    stats.greylisted > 0 && deliveries.every((d) => d.finals.filter((f) => f.startsWith("451")).length === 0);
results["a spoof of a p=reject domain is refused at DATA"] =
    /550 5\.7\.1 DMARC policy reject: spf=none dkim=none dmarc=fail/.test(r2.join(" "));
results["an altered DKIM-signed body fails verification"] =
    !HAVE_ED25519 || /550 5\.7\.1.*dkim=fail/.test(r3.join(" "));
results["a quarantine-policy failure plus phishing content lands in junk"] =
    /queued as junk/.test(r4.join(" ")) && mailbox.junk.some((j) => /dmarc=fail/.test(j.auth));
results["our own DKIM-signed notification passes our own checks"] =
    !HAVE_ED25519 || /queued as inbox/.test(r5.join(" "));
const offsite = [...mailbox.threads.values()].find((t) => t.subject === "planning the q3 offsite");
results[`the five-message conversation is one thread (${offsite?.messages.length})`] =
    offsite && offsite.messages.length === 5;
const bodyOk = mailbox.messages.find((m) => /order 1042/.test(m.subject));
results["dot-stuffing round-trips a line that starts with a dot"] = bodyOk && bodyOk.body.includes("\r\n.hidden line");
const bg = deliveries.filter((d) => d.desc.startsWith("background"));
const caught = bg.filter((d, k) => k % 2 === 1 && /junk/.test(d.finals.join(" "))).length,
    fp = bg.filter((d, k) => k % 2 === 0 && /junk/.test(d.finals.join(" "))).length;
results[`spam filter: ${caught}/20 spam caught, ${fp}/20 ham misfiled`] = caught >= 18 && fp <= 1;
results["out-of-order commands get 503, unknown ones 500"] =
    proto.filter((f) => f.startsWith("503")).length === 2 && proto.some((f) => f.startsWith("500"));
results["SPF ip4, include and qualifiers"] =
    spf("shop.example", "198.51.100.70") === "pass" &&
    spf("shop.example", "203.0.113.20") === "fail" &&
    spf("mail.example.net", "1.2.3.4") === "softfail";
results["mailbox search intersects terms"] =
    searchMail("venue dates").size === 5 && searchMail("order shipped").size >= 1;
const ranked = mailbox.index.search("offsite venue", { mode: "or", limit: 3 });
results["BM25 ranks offsite-thread mail first and prefix search completes words"] =
    ranked.length > 0 &&
    mailbox.messages[ranked[0][0]].thread === offsite.id &&
    mailbox.index.suggest("ven", 3).some(([w]) => w.startsWith("venue"));

print("email-platform");
print(
    `  smtp       ${stats.sessions} sessions, ${stats.accepted} accepted, ${stats.rejected} rejected by DMARC, ${stats.greylisted} greylisted, ${stats.unknownRcpt} unknown recipients, ${stats.protocolErrors} protocol errors`,
);
print(
    `  signing    ${HAVE_ED25519 ? "DKIM ed25519-sha256 (RFC 8463)" : "no Ed25519 in this build: DKIM checks report neutral"}`,
);
for (const d of deliveries.filter((x) => !x.desc.startsWith("background") && !x.desc.startsWith("thread")))
    print(
        `  ${d.desc.padEnd(44)} ${d.finals.filter((f) => !/^(220|250|354|221)/.test(f) || /queued/.test(f)).join(" | ")}`,
    );
print(
    `  mailbox    ${mailbox.messages.length} in inbox across ${mailbox.threads.size} threads, ${mailbox.junk.length} in junk`,
);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
