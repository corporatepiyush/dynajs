// timeout: 300
// covers: dyna:otp.HOTPGenerate dyna:otp.TOTPGenerate dyna:otp.HOTPVerify dyna:otp.TOTPVerify dyna:otp.HOTPMatch dyna:otp.TOTPMatch dyna:otp.KeyURIParse dyna:otp.KeyURIBuild dyna:otp.Enroll dyna:otp.Throttle
// dyna:otp (S15): HOTP/TOTP moved out of dyna:crypto as a clean break (crypto exports none of the four names), plus
// otpauth:// key URIs, QR enrolment, look-ahead/drift windows, replay protection and throttling.
// Vectors: RFC 4226 Appendix D (all ten counters) and RFC 6238 Appendix B (all eighteen rows: six times x SHA1/SHA256/
// SHA512 with the 20/32/64-byte ASCII seeds, 8 digits); Google Authenticator Key URI format examples (JBSWY3DPEHPK3PXP is
// "Hello!" DE AD BE EF). Window rules are pinned with constructed collisions: two counters a few steps apart that give
// the same 6-digit code (found by scanning HOTP codes), so "nearest to the centre wins, the lower on a tie" and
// "counters at or below lastCounter are skipped" are observable, and a scan that stops at the first match is caught.
// The enrolment QR must equal dyna:encoding QREncode/QRToString of the same URI byte for byte.
import * as otp from "dyna:otp";
import * as crypto from "dyna:crypto";
import { QREncode, QRToString, Base32Encode } from "dyna:encoding";
import { sections } from "../lib/kit.js";

const S = sections("test_otp_module", { chain: true });

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const eq = (a, b, w) => ok(a === b, w, String(a) + " vs " + String(b));
function throws(f, Ctor, re, w) {
    try { f(); ok(false, w, "no throw"); }
    catch (e) { ok(e instanceof Ctor && (!re || re.test(String(e.message))), w, e.name + ": " + e.message); }
}
const utf8 = (s) => new TextEncoder().encode(s);
const bytesEq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const S20 = "12345678901234567890", S32 = S20 + "123456789012", S64 = S20 + S20 + S20 + "1234";

S.section("clean break: crypto drops the names, otp export list", () => {
for (const n of ["HOTPGenerate", "TOTPGenerate", "HOTPVerify", "TOTPVerify"]) ok(!(n in crypto), "dyna:crypto no longer exports " + n);
eq(Object.keys(otp).sort().join(), "Enroll,HOTPGenerate,HOTPMatch,HOTPVerify,KeyURIBuild,KeyURIParse,TOTPGenerate,TOTPMatch,TOTPVerify,Throttle", "dyna:otp exports");
});

const RFC4226 = ["755224", "287082", "359152", "969429", "338314", "254676", "287922", "162583", "399871", "520489"];
S.section("RFC 4226 Appendix D vectors", () => {
RFC4226.forEach((want, c) => {
    eq(otp.HOTPGenerate(S20, c), want, "RFC 4226 counter " + c);
    ok(otp.HOTPVerify(S20, c, want), "HOTPVerify RFC 4226 counter " + c);
    const m = otp.HOTPMatch(S20, c, want);
    ok(m && m.counter === c && m.next === c + 1, "HOTPMatch RFC 4226 counter " + c, JSON.stringify(m));
});
});

const RFC6238 = [
    [59, "94287082", "46119246", "90693936"],
    [1111111109, "07081804", "68084774", "25091201"],
    [1111111111, "14050471", "67062674", "99943326"],
    [1234567890, "89005924", "91819424", "93441116"],
    [2000000000, "69279037", "90698825", "38618901"],
    [20000000000, "65353130", "77737706", "47863826"],
];
S.section("RFC 6238 Appendix B vectors", () => {
for (const [t, a, b, c] of RFC6238) {
    for (const [algo, seed, want] of [["sha1", S20, a], ["sha256", S32, b], ["sha512", S64, c]]) {
        eq(otp.TOTPGenerate(seed, { atSec: t, digits: 8, algo }), want, `RFC 6238 T=${t} ${algo}`);
        ok(otp.TOTPVerify(seed, want, { atSec: t, digits: 8, algo }), `TOTPVerify RFC 6238 T=${t} ${algo}`);
        const m = otp.TOTPMatch(utf8(seed), want, { atSec: t, digits: 8, algo });
        ok(m && m.counter === Math.floor(t / 30) && m.drift === 0, `TOTPMatch RFC 6238 T=${t} ${algo}`, JSON.stringify(m));
    }
}
});

S.section("TOTP functions default to the current time; TOTPVerify window defaults to 1 step", () => {
    const nowSec = () => Math.floor(Date.now() / 1000);
    let same = false;
    for (let i = 0; i < 3 && !same; i++) {
        const t0 = nowSec(), g = otp.TOTPGenerate(S20), t1 = nowSec();
        same = Math.floor(t0 / 30) === Math.floor(t1 / 30) && g === otp.TOTPGenerate(S20, { atSec: t0 });
    }
    ok(same, "TOTPGenerate without atSec uses the current Unix time");
    let fresh = false;
    for (let i = 0; i < 3 && !fresh; i++) {
        const t0 = nowSec(), code = otp.TOTPGenerate(S20, { atSec: t0 });
        fresh = otp.TOTPVerify(S20, code) && Math.floor(nowSec() / 30) - Math.floor(t0 / 30) <= 1;
    }
    ok(fresh, "TOTPVerify without atSec accepts the code of the current step");
    const s = 40000000, at = s * 30 + 7;
    const prev = otp.TOTPGenerate(S20, { atSec: at - 30 }), next = otp.TOTPGenerate(S20, { atSec: at + 30 }), far = otp.TOTPGenerate(S20, { atSec: at - 60 });
    ok(otp.TOTPVerify(S20, prev, { atSec: at }) && otp.TOTPVerify(S20, next, { atSec: at }), "TOTPVerify window defaults to 1: the previous and next steps verify");
    ok(!otp.TOTPVerify(S20, far, { atSec: at }), "TOTPVerify default window stops at 1 step");
    ok(!otp.TOTPVerify(S20, prev, { atSec: at, window: 0 }), "window 0 accepts only the current step");
    const tc = otp.TOTPGenerate(S20, { atSec: 59 });
    ok(otp.TOTPVerify(S20, tc, { atSec: 89 }), "TOTPVerify default window covers the next step");
    throws(() => otp.TOTPVerify(S20, tc, { window: 4097 }), RangeError, /4096/, "TOTPVerify window cap 4096");
    ok(!otp.HOTPVerify(S20, 0, "75522a") && !otp.HOTPVerify(S20, 0, "7552244"), "HOTPVerify malformed codes are false");
});

S.section("HOTPMatch look-ahead", () => {
    const c = 2;
    ok(otp.HOTPMatch(S20, c, RFC4226[7]) === null, "HOTPMatch without lookAhead tries one counter");
    const m = otp.HOTPMatch(S20, c, RFC4226[7], { lookAhead: 5 });
    ok(m && m.counter === 7 && m.next === 8, "HOTPMatch lookAhead 5 reaches counter 7", JSON.stringify(m));
    ok(otp.HOTPMatch(S20, c, RFC4226[7], { lookAhead: 4 }) === null, "lookAhead 4 stops at counter 6");
    ok(otp.HOTPMatch(S20, 8, RFC4226[7], { lookAhead: 100 }) === null, "lookAhead never goes backwards");
    ok(otp.HOTPMatch(S20, 0, "12345a", { lookAhead: 9 }) === null && otp.HOTPMatch(S20, 0, "7552240") === null, "HOTPMatch malformed codes are null");
    const m8 = otp.HOTPMatch(S20, 3, otp.HOTPGenerate(S20, 5, { digits: 8, algo: "sha256" }), { digits: 8, algo: "sha256", lookAhead: 3 });
    ok(m8 && m8.counter === 5, "HOTPMatch honours digits and algo", JSON.stringify(m8));
    throws(() => otp.HOTPMatch(S20, 0, "755224", { lookAhead: 4097 }), RangeError, /lookAhead/, "lookAhead cap 4096");
    ok(otp.HOTPMatch(S20, 0, "755224", { lookAhead: 4096 }) !== null, "lookAhead 4096 accepted");
    throws(() => otp.HOTPMatch(S20, -1, "755224"), RangeError, /counter/, "HOTPMatch negative counter");
    throws(() => otp.HOTPMatch(S20, 1.5, "755224"), RangeError, /counter/, "HOTPMatch fractional counter");
    throws(() => otp.HOTPMatch(S20, "1", "755224"), TypeError, /counter/, "HOTPMatch counter must be a number");
    throws(() => otp.HOTPMatch(S20, 0, 755224), TypeError, /code/, "HOTPMatch code must be a string");
    throws(() => otp.HOTPMatch(S20, 0, "755224", { window: 1 }), TypeError, null, "HOTPMatch unknown option");
    throws(() => otp.HOTPMatch(S20, 0, "755224", { algo: "md5" }), TypeError, /algo/, "HOTPMatch bad algo");
    throws(() => otp.HOTPMatch(5, 0, "755224"), TypeError, /secret/, "HOTPMatch secret type");
});

function collisions(secret, maxGap, want) {
    const seen = new Map(), out = [];
    for (let c = 0; c < 3000000 && out.length < want; c++) {
        const code = otp.HOTPGenerate(secret, c);
        const p = seen.get(code);
        if (p !== undefined && p >= 40 && c - p <= maxGap && (c - p) % 2 === 0) out.push([p, c, code]);
        seen.set(code, c);
    }
    return out;
}
S.section("window collisions: nearest wins, ties, lastCounter, drift", () => {
    const sec = "collision-secret";
    const pairs = collisions(sec, 8, 1);
    ok(pairs.length === 1, "found a 6-digit collision at an even gap of at most 8 steps", JSON.stringify(pairs));
    if (pairs.length) {
        const [a, b, code] = pairs[0], P = 30, mid = (a + b) / 2;
        const at = (step) => step * P + 7;
        const w = b - a;
        let m = otp.TOTPMatch(sec, code, { atSec: at(a), window: w });
        ok(m && m.counter === a && m.drift === 0, "the counter nearest the centre wins (lower one at the centre)", JSON.stringify(m));
        m = otp.TOTPMatch(sec, code, { atSec: at(b), window: w });
        ok(m && m.counter === b && m.drift === 0, "the counter nearest the centre wins (higher one at the centre: a first-match scan fails here)", JSON.stringify(m));
        m = otp.TOTPMatch(sec, code, { atSec: at(mid), window: w });
        ok(m && m.counter === a && m.drift === a - mid, "an exact tie goes to the lower counter", JSON.stringify(m));
        m = otp.TOTPMatch(sec, code, { atSec: at(a), window: w, lastCounter: a });
        ok(m && m.counter === b, "lastCounter skips the used counter and the later one still matches", JSON.stringify(m));
        ok(otp.TOTPMatch(sec, code, { atSec: at(a), window: w, lastCounter: b }) === null, "counters at or below lastCounter never match");
        m = otp.TOTPMatch(sec, code, { atSec: at(a - 40), drift: 40 + (b - a), window: 0 });
        ok(m && m.counter === b && m.drift === b - (a - 40), "drift moves the centre; the result reports the offset from the current step", JSON.stringify(m));
        const hm = otp.HOTPMatch(sec, a, code, { lookAhead: w });
        ok(hm && hm.counter === a, "HOTPMatch returns the lowest matching counter", JSON.stringify(hm));
        const hm2 = otp.HOTPMatch(sec, a + 1, code, { lookAhead: w });
        ok(hm2 && hm2.counter === b && hm2.next === b + 1, "HOTPMatch after the first use finds the later counter", JSON.stringify(hm2));
    }
});

S.section("TOTPMatch drift and replay", () => {
    const s = utf8("drift-and-replay");
    const now = 1700000000, step = Math.floor(now / 30);
    const code = otp.TOTPGenerate(s, { atSec: now + 30 });
    ok(otp.TOTPMatch(s, code, { atSec: now }) === null, "TOTPMatch window 0 by default");
    let m = otp.TOTPMatch(s, code, { atSec: now, window: 1 });
    ok(m && m.counter === step + 1 && m.drift === 1, "window 1 accepts the next step and reports drift 1", JSON.stringify(m));
    ok(otp.TOTPMatch(s, code, { atSec: now, window: 1, lastCounter: step + 1 }) === null, "replay of the same step is refused");
    ok(otp.TOTPMatch(s, code, { atSec: now, window: 1, lastCounter: step }) !== null, "a lower lastCounter allows it");
    const far = otp.TOTPGenerate(s, { atSec: now + 30 * 7 });
    ok(otp.TOTPMatch(s, far, { atSec: now, window: 1 }) === null, "7 steps of drift is outside window 1");
    m = otp.TOTPMatch(s, far, { atSec: now, window: 1, drift: 6 });
    ok(m && m.drift === 7, "a stored drift of 6 recentres the window", JSON.stringify(m));
    m = otp.TOTPMatch(s, otp.TOTPGenerate(s, { atSec: 0 }), { atSec: 0, window: 3 });
    ok(m && m.counter === 0, "counters below 0 are skipped at the epoch", JSON.stringify(m));
    m = otp.TOTPMatch(s, otp.TOTPGenerate(s, { atSec: now, period: 60, digits: 8, algo: "sha512" }), { atSec: now, period: 60, digits: 8, algo: "sha512" });
    ok(m && m.counter === Math.floor(now / 60), "period, digits and algo apply", JSON.stringify(m));
    const t = Math.floor(Date.now() / 1000);
    ok(otp.TOTPMatch(s, otp.TOTPGenerate(s, { atSec: t }), { window: 1 }) !== null, "TOTPMatch atSec defaults to the current time");
    ok(otp.TOTPMatch(s, "12345", { atSec: now }) === null && otp.TOTPMatch(s, "1234567", { atSec: now }) === null && otp.TOTPMatch(s, "12a456", { atSec: now }) === null, "malformed codes are null");
    for (const [o, re, C] of [[{ window: 4097 }, /window/, RangeError], [{ window: -1 }, /window/, RangeError], [{ atSec: -1 }, /atSec/, RangeError],
        [{ period: 0 }, /period/, RangeError], [{ drift: 0.5 }, /drift/, RangeError], [{ lastCounter: -1 }, /lastCounter/, RangeError],
        [{ digits: 9 }, /digits/, RangeError], [{ digits: 5 }, /digits/, RangeError], [{ atSec: "1" }, /atSec/, TypeError], [{ algo: "SHA1" }, /algo/, TypeError],
        [{ lookAhead: 1 }, null, TypeError], [{ window: NaN }, /window/, RangeError], [{ atSec: Infinity }, /atSec/, RangeError]])
        throws(() => otp.TOTPMatch(s, "123456", o), C, re, "TOTPMatch option " + JSON.stringify(o));
    throws(() => otp.TOTPMatch(s), TypeError, /TOTPMatch/, "TOTPMatch needs a code");
    throws(() => otp.TOTPMatch(s, "123456", 5), TypeError, null, "TOTPMatch opts must be an object");
});

S.section("KeyURIParse", () => {
    const g = otp.KeyURIParse("otpauth://totp/Example:alice@google.com?secret=JBSWY3DPEHPK3PXP&issuer=Example");
    ok(g.type === "totp" && g.issuer === "Example" && g.account === "alice@google.com" && g.algo === "sha1" && g.digits === 6 && g.period === 30 && !("counter" in g), "Google example fields", JSON.stringify(g));
    ok(g.secret instanceof Uint8Array && bytesEq(g.secret, [72, 101, 108, 108, 111, 33, 0xde, 0xad, 0xbe, 0xef]), "JBSWY3DPEHPK3PXP decodes to Hello! DE AD BE EF");
    const a = otp.KeyURIParse("otpauth://totp/ACME%20Co:john.doe@email.com?secret=HXDMVJECJJWSRB3HWIZR4IFUGFTMXBOZ&issuer=ACME%20Co&algorithm=SHA1&digits=6&period=30");
    ok(a.issuer === "ACME Co" && a.account === "john.doe@email.com" && a.secret.length === 20, "Google example with all parameters", JSON.stringify(a));
    const h = otp.KeyURIParse("OTPAUTH://HOTP/alice?secret=gezdgnbvgy3tqojqgezdgnbvgy3tqojq&counter=9007199254740991&algorithm=sha256&digits=8");
    ok(h.type === "hotp" && !("issuer" in h) && h.account === "alice" && h.counter === 9007199254740991 && h.algo === "sha256" && h.digits === 8 && !("period" in h), "hotp, no issuer, lower-case secret and algorithm", JSON.stringify(h));
    ok(new TextDecoder().decode(h.secret) === S20, "lower-case base32 decodes");
    const c = otp.KeyURIParse("otpauth://totp/Big%20Corp%3A%20%20bob?secret=GEZDGNBVGY======&issuer=Big+Corp&period=60&image=https%3A%2F%2Fx&foo");
    ok(c.issuer === "Big Corp" && c.account === "bob" && c.period === 60 && new TextDecoder().decode(c.secret) === "123456", "%3A colon, spaces after it dropped, + in a parameter is a space, padding accepted, unknown parameters ignored", JSON.stringify(c));
    const d = otp.KeyURIParse("otpauth://totp/a+b?secret=GE");
    ok(d.account === "a+b" && new TextDecoder().decode(d.secret) === "1", "+ in the label stays a plus; one-byte secret");
    const e = otp.KeyURIParse("otpauth://totp/%E6%97%A5%E6%9C%AC:%F0%9F%94%91?secret=GE&issuer=%E6%97%A5%E6%9C%AC");
    ok(e.issuer === "日本" && e.account === "🔑", "UTF-8 names", JSON.stringify(e));
    const bad = [
        ["http://totp/a?secret=GE", /scheme/], ["otpauth:/totp/a?secret=GE", /scheme/], ["otpauth://totp", /label/], ["otpauth://motp/a?secret=GE", /type/],
        ["otpauth://totp/?secret=GE", /account/], ["otpauth://totp/Iss:?secret=GE", /account/], ["otpauth://totp/:bob?secret=GE", /account/],
        ["otpauth://totp/a:b:c?secret=GE", /colon/], ["otpauth://totp/a%3Ab%3Ac?secret=GE", /colon/],
        ["otpauth://totp/X:a?secret=GE&issuer=Y", /differs/], ["otpauth://totp/a", /secret/], ["otpauth://totp/a?secret=", /secret/],
        ["otpauth://totp/a?secret=G1", /secret/], ["otpauth://totp/a?secret=G0", /secret/], ["otpauth://totp/a?secret=G", /secret/],
        ["otpauth://totp/a?secret=GEZ", /secret/], ["otpauth://totp/a?secret=GEZDGN", /secret/], ["otpauth://totp/a?secret=GF", /secret/],
        ["otpauth://totp/a?secret=GEZDGNBVGY====", /secret/], ["otpauth://totp/a?secret=GE%20", /secret/],
        ["otpauth://totp/a%ZZ?secret=GE", /percent/], ["otpauth://totp/a%4?secret=GE", /percent/], ["otpauth://totp/a?secret=GE&issuer=%", /percent/],
        ["otpauth://totp/a%FF?secret=GE", /UTF-8/], ["otpauth://totp/a%C0%80?secret=GE", /UTF-8/], ["otpauth://totp/a%ED%A0%80?secret=GE", /UTF-8/],
        ["otpauth://totp/a%00?secret=GE", /UTF-8/], ["otpauth://totp/a%0A?secret=GE", /UTF-8/], ["otpauth://totp/a?secret=GE&issuer=x%7F", /UTF-8/],
        ["otpauth://totp/a?secret=GE#frag", /fragment/], ["otpauth://totp/a?secret=GE&secret=GE", /twice/], ["otpauth://totp/a?secret=GE&digits=6&digits=6", /twice/],
        ["otpauth://totp/a?secret=GE&algorithm=MD5", /algorithm/], ["otpauth://totp/a?secret=GE&digits=7", /digits/], ["otpauth://totp/a?secret=GE&digits=10", /digits/],
        ["otpauth://totp/a?secret=GE&digits=6a", /digits/], ["otpauth://totp/a?secret=GE&digits=", /digits/], ["otpauth://totp/a?secret=GE&period=0", /period/],
        ["otpauth://totp/a?secret=GE&period=-30", /period/], ["otpauth://hotp/a?secret=GE", /counter/], ["otpauth://hotp/a?secret=GE&counter=-1", /counter/],
        ["otpauth://hotp/a?secret=GE&counter=9007199254740992", /counter/], ["otpauth://hotp/a?secret=GE&counter=1.0", /counter/],
        ["otpauth://totp/a?secret=GE&issuer=", /issuer/], ["otpauth://totp/a?secret=GE&issuer=a%3Ab", /issuer/],
    ];
    for (const [u, re] of bad) throws(() => otp.KeyURIParse(u), SyntaxError, re, "KeyURIParse refuses " + u);
    throws(() => otp.KeyURIParse(5), TypeError, /string/, "KeyURIParse needs a string");
    throws(() => otp.KeyURIParse(), TypeError, /string/, "KeyURIParse needs an argument");
});

S.section("KeyURIBuild", () => {
    const u = otp.KeyURIBuild({ account: "alice@google.com", issuer: "Example Co", secret: S20 });
    eq(u, "otpauth://totp/Example%20Co:alice%40google.com?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=Example%20Co&algorithm=SHA1&digits=6&period=30", "KeyURIBuild exact form");
    eq(otp.KeyURIBuild({ type: "hotp", account: "bob", secret: new Uint8Array([0xde, 0xad, 0xbe, 0xef]), counter: 42, digits: 8, algo: "sha512" }),
        "otpauth://hotp/bob?secret=32W353Y&algorithm=SHA512&digits=8&counter=42", "hotp form: no issuer, unpadded base32, counter");
    eq(otp.KeyURIBuild({ type: "hotp", account: "bob", secret: "x" }), "otpauth://hotp/bob?secret=PA&algorithm=SHA1&digits=6&counter=0", "hotp counter defaults to 0");
    const names = ["a b", "p%q", "r&s=t", "u+v", "w#x?y/z", "日本語", "🔑 key", "~._-", "'\"<>"];
    for (const issuer of names) for (const account of ["x", names[(names.indexOf(issuer) + 1) % names.length]]) {
        const sec = utf8(issuer + account);
        const k = otp.KeyURIParse(otp.KeyURIBuild({ issuer, account, secret: sec, period: 45, digits: 8, algo: "sha256" }));
        ok(k.issuer === issuer && k.account === account && bytesEq(k.secret, sec) && k.period === 45 && k.digits === 8 && k.algo === "sha256", "round trip " + JSON.stringify([issuer, account]), JSON.stringify(k));
    }
    for (let len = 1; len <= 41; len++) {
        const sec = new Uint8Array(len).map((_, i) => (i * 37 + len) & 255);
        const uri = otp.KeyURIBuild({ account: "a", secret: sec });
        const b32 = Base32Encode(sec).replace(/=+$/, "");
        ok(uri.includes("secret=" + b32 + "&") && bytesEq(otp.KeyURIParse(uri).secret, sec), "base32 of " + len + " bytes matches dyna:encoding and round-trips");
    }
    const k = { account: "a", secret: "s" };
    for (const [o, C, re] of [[{ secret: "s" }, TypeError, /account/], [{ account: "a" }, TypeError, /secret/], [{ account: "a", secret: "" }, RangeError, /secret/],
        [{ ...k, digits: 7 }, RangeError, /digits/], [{ ...k, type: "hotp", period: 30 }, TypeError, /period/], [{ ...k, counter: 1 }, TypeError, /counter/],
        [{ ...k, extra: 1 }, TypeError, null], [{ ...k, type: "motp" }, TypeError, /type/], [{ ...k, account: "a:b" }, RangeError, /colon/],
        [{ ...k, issuer: "" }, RangeError, /issuer/], [{ ...k, account: "a\nb" }, RangeError, /control/], [{ ...k, account: 5 }, TypeError, /account/],
        [{ ...k, period: 0 }, RangeError, /period/], [{ ...k, secret: 5 }, TypeError, /secret/], [{ ...k, algo: "sha384" }, TypeError, /algo/]])
        throws(() => otp.KeyURIBuild(o), C, re, "KeyURIBuild refuses " + JSON.stringify(o));
    throws(() => otp.KeyURIBuild("x"), TypeError, /object/, "KeyURIBuild needs an object");
});

S.section("Enroll", () => {
    const e = otp.Enroll({ issuer: "Example", account: "dana@example.com" });
    ok(e.secret instanceof Uint8Array && e.secret.length === 20, "Enroll draws 20 random bytes by default");
    eq(e.secretBase32, Base32Encode(e.secret).replace(/=+$/, ""), "secretBase32 is the unpadded base32 of secret");
    const k = otp.KeyURIParse(e.uri);
    ok(k.issuer === "Example" && k.account === "dana@example.com" && bytesEq(k.secret, e.secret) && k.type === "totp", "the URI carries the key", e.uri);
    const q = QREncode(e.uri);
    ok(e.qr.version === q.version && e.qr.size === q.size && bytesEq(e.qr.modules, q.modules), "qr equals QREncode(uri)");
    eq(e.qrText, QRToString(e.uri), "qrText equals QRToString(uri)");
    const h = otp.Enroll({ account: "x", type: "hotp", counter: 5, secretBytes: 32, qr: { ecc: "H" } });
    const qh = QREncode(h.uri, { ecc: "H" });
    ok(h.secret.length === 32 && otp.KeyURIParse(h.uri).counter === 5 && h.qr.size === qh.size && bytesEq(h.qr.modules, qh.modules) && h.qrText === QRToString(h.uri, { ecc: "H" }), "hotp enrolment with QR options", h.uri);
    const n = otp.Enroll({ account: "y", dyna: { qr: false } });
    ok(!("qr" in n) && !("qrText" in n) && typeof n.uri === "string", "dyna.qr false skips the QR renderings");
    const fixed = new Uint8Array(16).fill(7);
    const f = otp.Enroll({ account: "z", secret: fixed, dyna: { qr: false } });
    ok(bytesEq(f.secret, fixed) && f.secret !== fixed && bytesEq(otp.KeyURIParse(f.uri).secret, fixed), "a given secret is used and copied");
    ok(!bytesEq(otp.Enroll({ account: "r", dyna: { qr: false } }).secret, otp.Enroll({ account: "r", dyna: { qr: false } }).secret), "two enrolments draw different secrets");
    for (const [o, C, re] of [[{ account: "a", secretBytes: 15 }, RangeError, /secretBytes/], [{ account: "a", secretBytes: 65 }, RangeError, /secretBytes/],
        [{ account: "a", secret: new Uint8Array(15) }, RangeError, /16\.\.64/], [{ account: "a", secret: new Uint8Array(65) }, RangeError, /16\.\.64/],
        [{ account: "a", secret: new Uint8Array(16), secretBytes: 16 }, TypeError, /not both/], [{ account: "a", dyna: { qr: 1 } }, TypeError, /boolean/],
        [{ account: "a", dyna: { qrText: false } }, TypeError, null], [{ account: "a", dyna: 1 }, TypeError, /dyna/], [{ account: "a", bogus: 1 }, TypeError, null],
        [{ secretBytes: 20 }, TypeError, /account/], [{ account: "a", digits: 7 }, RangeError, /digits/], [{ account: "a", qr: { ecc: "Z" } }, Error, null],
        [{ account: "a".repeat(3000) }, RangeError, /2953/]])
        throws(() => otp.Enroll(o), C, re, "Enroll refuses " + JSON.stringify(o).slice(0, 80));
    throws(() => otp.Enroll(), TypeError, /object/, "Enroll needs options");
});

S.section("Throttle", () => {
    let r = otp.Throttle(0);
    ok(r.allowed && !r.locked && r.retryAtSec === 0, "no failures: allowed", JSON.stringify(r));
    r = otp.Throttle(3, 100, { atSec: 114 });
    ok(!r.allowed && !r.locked && r.retryAtSec === 115, "3 failures wait 3 x 5 s", JSON.stringify(r));
    ok(otp.Throttle(3, 100, { atSec: 115 }).allowed, "allowed at retryAtSec");
    r = otp.Throttle(9, 100, { atSec: 145 });
    ok(r.allowed && r.retryAtSec === 145, "9 failures: 45 s", JSON.stringify(r));
    r = otp.Throttle(10, 100, { atSec: 1e9 });
    ok(!r.allowed && r.locked && r.retryAtSec === Infinity, "10 failures lock by default", JSON.stringify(r));
    r = otp.Throttle(50, 100, { atSec: 349, lockoutAfter: 0 });
    ok(!r.allowed && !r.locked && r.retryAtSec === 350, "lockoutAfter 0 disables the lockout", JSON.stringify(r));
    r = otp.Throttle(4, 100, { atSec: 100, stepSec: 0, lockoutAfter: 5 });
    ok(r.allowed, "stepSec 0 adds no delay", JSON.stringify(r));
    ok(otp.Throttle(5, 100, { atSec: 1e9, lockoutAfter: 5 }).locked, "custom lockoutAfter");
    ok(otp.Throttle(1, Math.floor(Date.now() / 1000) + 3600).allowed === false, "atSec defaults to the current time");
    throws(() => otp.Throttle(1), TypeError, /lastFailureSec/, "lastFailureSec required after a failure");
    throws(() => otp.Throttle(-1, 0), RangeError, /failures/, "negative failures");
    throws(() => otp.Throttle(1, 0, { stepSec: 86401 }), RangeError, /stepSec/, "stepSec cap");
    throws(() => otp.Throttle(1, 0, { window: 1 }), TypeError, null, "Throttle unknown option");
    throws(() => otp.Throttle("1", 0), TypeError, /failures/, "failures must be a number");
});

print(`test_otp_module: ${pass} passed, ${fail} failed`);
S.end();
if (fail) throw new Error("test_otp_module: " + fail + " failing checks");
