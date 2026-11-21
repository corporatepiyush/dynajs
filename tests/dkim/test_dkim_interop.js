// covers: integration dyna:dkim.sign dyna:dkim.verify -- every case signs and then verifies the same message
// test_dkim_interop.js -- differential against Perl Mail::DKIM (a reference implementation, rsa-sha256
// only: the system 0.58 build has no Ed25519) and key lookup through a real DNS exchange (S9 M3, 2026-11-27).
// Mail::DKIM signs (simple/simple, relaxed/relaxed, relaxed/simple; its own folding and h= choice) and we
// verify; we sign (all four c= pairs, l=, oversign, identity, x=) and Mail::DKIM verifies, its DNS query
// answered from the same key table. One tampered byte must fail on both sides. The DNS case serves TXT
// records from a dyna:net UDPSocket (RFC 1035 wire format, a 2048-bit key split into 255-octet strings as
// RFC 6376 3.6.2.2 allows) to a dyna:dns DNSResolver passed as `lookup`; an unknown selector gets NXDOMAIN
// (rcode 3) and must be permerror, a silent server temperror. Mail::DKIM missing = loud skip.
import { suite } from "../lib/kit.js";
import { sign, verify } from "dyna:dkim";
import { RSA } from "dyna:crypto";
import { Exec, Which } from "dyna:sys";
import { UDPSocket } from "dyna:net";
import { DNSResolver } from "dyna:dns";

const t = suite("test_dkim_interop");
const RK = RSA.generate(2048);
const SPKI = RK.publicKey.replace(/-----[^-]+-----|\s/g, "");
const KEYS = { "sel1._domainkey.example.org": "v=DKIM1; k=rsa; p=" + SPKI };
const lookup = (n) => KEYS[n] ?? null;
const MSG = "From: \"Ann Example\" <ann@example.org>\r\nTo: bob@example.net,\r\n  carol@example.net\r\nSubject:   folded\r\n\tsubject  line \r\nDate: Thu, 26 Nov 2026 10:00:00 +0000\r\nMessage-ID: <x1@example.org>\r\nMIME-Version: 1.0\r\n\r\nBody  with \t spaces  \r\n\r\nsecond para\r\n\r\n\r\n";

const PERL = String.raw`
use strict; use warnings; use Mail::DKIM::Signer; use Mail::DKIM::Verifier; use Mail::DKIM::PrivateKey;
use Crypt::OpenSSL::RSA; use Net::DNS;
my $mode = shift @ARGV; local $/; my $msg = <STDIN>;
if ($mode eq 'sign') {
  my $key = Mail::DKIM::PrivateKey->load(Cork => Crypt::OpenSSL::RSA->new_private_key($ENV{DKIM_PEM}));
  my $s = Mail::DKIM::Signer->new(Algorithm => 'rsa-sha256', Method => $ARGV[0], Domain => 'example.org', Selector => 'sel1', Key => $key);
  $s->PRINT($msg); $s->CLOSE; print $s->signature->as_string, "\r\n";
} else {
  my %k = map { split /\t/, $_, 2 } grep { length } split /\n/, $ENV{DKIM_KEYS};
  no warnings 'redefine';
  *Mail::DKIM::DNS::query = sub { my ($n) = @_; my $v = $k{lc $n}; return () unless defined $v;
    return (Net::DNS::RR->new(name => $n, type => 'TXT', txtdata => [ $v =~ /(.{1,255})/g ])); };
  my $v = Mail::DKIM::Verifier->new; $v->PRINT($msg); $v->CLOSE;
  print join("\n", map { $_->result . (defined $_->result_detail ? " " . $_->result_detail : "") } $v->signatures), "\n";
}`;

let perlOk = null;
function havePerl() {
    if (perlOk === null) {
        perlOk = false;
        if (Which("perl")) {
            const r = Exec("perl", ["-MMail::DKIM::Signer", "-MMail::DKIM::Verifier", "-MCrypt::OpenSSL::RSA", "-MNet::DNS", "-e", "print 1"]);
            perlOk = r.code === 0 && r.stdout === "1";
        }
    }
    return perlOk;
}
const env = () => ({ DKIM_PEM: RK.privateKey, DKIM_KEYS: Object.entries(KEYS).map(([n, v]) => n + "\t" + v).join("\n"), PATH: "/usr/bin:/bin" });
function perl(mode, msg, arg = "") {
    const r = Exec("perl", ["-e", PERL, mode, arg], { input: msg, env: env(), timeoutMs: 30000 });
    if (r.code !== 0)
        throw new Error("perl " + mode + " failed: " + r.stderr);
    return r.stdout;
}

t.test("Mail::DKIM signs, dyna:dkim verifies", async (a) => {
    if (!havePerl()) return a.skip("perl with Mail::DKIM, Crypt::OpenSSL::RSA and Net::DNS not found");
    for (const c of ["simple/simple", "relaxed/relaxed", "relaxed/simple", "simple/relaxed"]) {
        const f = perl("sign", MSG, c);
        a.match(f, /^DKIM-Signature:/, c);
        const r = (await verify(f + MSG, { lookup }))[0];
        a.eq(r.status, "pass", c + ": " + (r.reason || ""));
        a.eq((await verify(f + MSG.replace("second", "Second"), { lookup }))[0].status, "fail", c + " tampered");
    }
});

t.test("dyna:dkim signs, Mail::DKIM verifies", async (a) => {
    if (!havePerl()) return a.skip("perl with Mail::DKIM, Crypt::OpenSSL::RSA and Net::DNS not found");
    const now = Math.floor(Date.now() / 1000);
    const variants = [
        { canonicalization: "simple/simple" }, { canonicalization: "simple/relaxed" },
        { canonicalization: "relaxed/simple" }, { canonicalization: "relaxed/relaxed" },
        { bodyLength: true }, { oversign: true }, { identity: "ann@mail.example.org" },
        { timestamp: now, expiresIn: 3600 }, { timestamp: null }, { headers: ["From", "Subject", "To", "To", "Cc"] },
    ];
    for (const v of variants) {
        const f = await sign(MSG, { domain: "example.org", selector: "sel1", privateKey: RK.privateKey, ...v });
        const out = perl("verify", f + MSG);
        a.match(out, /^pass/, JSON.stringify(v) + " -> " + out.trim());
        a.match(perl("verify", f + MSG.replace("Body", "B0dy")), /^fail/, JSON.stringify(v) + " tampered");
    }
    const lf = await sign(MSG.replace(/\r\n/g, "\n"), { domain: "example.org", selector: "sel1", privateKey: RK.privateKey });
    a.match(perl("verify", lf + MSG), /^pass/, "LF-signed verifies over the CRLF message");
});

function dnsServer(records) {
    const s = new UDPSocket({ host: "127.0.0.1", port: 0 });
    const seen = [];
    s.start({ message(q, from) {
        let o = 12, labels = [];
        while (q[o]) { labels.push(String.fromCharCode(...q.subarray(o + 1, o + 1 + q[o]))); o += q[o] + 1; }
        const qend = o + 5, name = labels.join(".").toLowerCase(), qtype = (q[o + 1] << 8) | q[o + 2];
        seen.push(name);
        if (records.silent) return;
        const txt = qtype === 16 ? records[name] : undefined;
        const out = [q[0], q[1], 0x81, txt === undefined ? 0x83 : 0x80, 0, 1, 0, txt === undefined ? 0 : 1, 0, 0, 0, 0, ...q.subarray(12, qend)];
        if (txt !== undefined) {
            const rd = [];
            for (let i = 0; i < txt.length; i += 255) { const c = txt.slice(i, i + 255); rd.push(c.length, ...[...c].map((ch) => ch.charCodeAt(0))); }
            out.push(0xc0, 12, 0, 16, 0, 1, 0, 0, 0, 60, rd.length >> 8, rd.length & 255, ...rd);
        }
        s.send(new Uint8Array(out), from.address, from.port);
    } });
    return { s, seen };
}

t.test("DNS fixture: DNSResolver as lookup, NXDOMAIN permerror, silence temperror", async (a) => {
    const srv = dnsServer({ "sel1._domainkey.example.org": KEYS["sel1._domainkey.example.org"] });
    const quiet = dnsServer({ silent: true });
    const r = new DNSResolver({ server: "127.0.0.1", port: srv.s.port, timeoutMs: 2000 });
    const rq = new DNSResolver({ server: "127.0.0.1", port: quiet.s.port, timeoutMs: 300 });
    try {
        a.ok(KEYS["sel1._domainkey.example.org"].length > 255, "record spans several strings");
        const f = await sign(MSG, { domain: "example.org", selector: "sel1", privateKey: RK.privateKey });
        a.eq((await verify(f + MSG, { lookup: r }))[0].status, "pass");
        a.deep(srv.seen, ["sel1._domainkey.example.org"]);
        const g = await sign(MSG, { domain: "example.org", selector: "nosuch", privateKey: RK.privateKey });
        const nx = (await verify(g + MSG, { lookup: r }))[0];
        a.eq(nx.status + ":" + nx.reason, "permerror:no key record for selector");
        const tmo = (await verify(f + MSG, { lookup: rq }))[0];
        a.eq(tmo.status, "temperror", tmo.reason);
    } finally {
        r.close();
        rq.close();
        srv.s.close();
        quiet.s.close();
    }
});

await t.run();
