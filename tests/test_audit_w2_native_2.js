// flags: --std
// timeout: 300
// tests/test_audit_w2_native_2.js -- crypto, HTTP/TLS, markdown, HTML and
// process regressions from audit wave 2 (second batch). Every assertion is
// red on the pre-fix binary.
//   D3-02 PEM text outlives the BIO that reads it       D3-03 keys are never stringified
//   D3-04 HS* refuses a PEM key                          D3-08 digestHex == digest for long XOF
//   D3-09 crit refused, non-canonical signature refused  D3-11 encrypted PEM does not prompt
//   D3-13 random serial, SAN injection refused
//   N1-14 strict chunk size   N1-27 TCPProxy defaults    N1-29 header string form
//   D2-01/02/03/04 markdown budget  D2-07/08/09/10 html
// build-note: needs the native modules and CONFIG_TLS=y
import * as crypto from "dyna:crypto";
import { Hasher } from "dyna:hash";
import { MarkdownToHTML, HTMLParse, Selector, Sanitizer, rewriteLinks } from "dyna:html";
import { HTTPClient } from "dyna:http";
import { TCPProxy } from "dyna:net";

let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got).slice(0, 80) + ", want " + String(want).slice(0, 80) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
function errMsg(fn) {
    try { fn(); return ""; } catch (e) { return String(e.message); }
}

// ---- D3-03 -----------------------------------------------------------------
{
    const tok = crypto.JWTSign({ a: 1 }, "undefined", { alg: "HS256" });
    for (const [label, bad] of [["undefined", undefined], ["null", null], ["number", 5], ["object", {}], ["boolean", true]]) {
        eq(errName(() => crypto.JWTVerify(tok, bad, { algorithms: ["HS256"] })), "TypeError", "JWTVerify refuses a " + label + " key");
        eq(errName(() => crypto.JWTSign({ a: 1 }, bad, { alg: "HS256" })), "TypeError", "JWTSign refuses a " + label + " key");
        eq(errName(() => crypto.HMACHex("sha256", bad, "x")), "TypeError", "HMACHex refuses a " + label + " key");
        eq(errName(() => new crypto.Hmac("sha256", bad)), "TypeError", "Hmac refuses a " + label + " key");
        eq(errName(() => crypto.TOTPGenerate(bad)), "TypeError", "TOTPGenerate refuses a " + label + " secret");
        eq(errName(() => crypto.TimingSafeEqual(bad, "undefined")), "TypeError", "TimingSafeEqual refuses a " + label);
    }
    eq(JSON.stringify(crypto.JWTVerify(tok, "undefined", { algorithms: ["HS256"] })), '{"a":1}', "a real string key still verifies (control)");
    eq(crypto.HMACHex("sha256", new Uint8Array([1, 2, 3]), "x").length, 64, "a byte-view key still works (control)");
    eq(errName(() => crypto.JWTVerify(crypto.JWTSign({ a: 1 }, "k", { alg: "HS256" }), "", { algorithms: ["HS256"] })), "TypeError", "JWTVerify refuses an empty HMAC key");
    eq(crypto.SHA256Hex({ toString() { return "q"; } }), crypto.SHA256Hex("q"), "DATA arguments keep their string coercion (control)");
}

// ---- D3-04 / D3-09 -----------------------------------------------------------
{
    const kp = crypto.RSA.generate ? crypto.RSA.generate(2048) : null;
    if (kp) {
        const forged = crypto.JWTSign({ admin: true }, kp.publicKey, { alg: "HS256" });
        eq(errName(() => crypto.JWTVerify(forged, kp.publicKey, { algorithms: ["RS256", "HS256"] })), "TypeError", "an HS256 token keyed with the RSA public PEM is refused");
        const good = crypto.JWTSign({ u: 1 }, kp.privateKey, { alg: "RS256" });
        eq(JSON.stringify(crypto.JWTVerify(good, kp.publicKey, { algorithms: ["RS256", "HS256"] })), '{"u":1}', "a real RS256 token verifies under the mixed allowlist (control)");
    } else {
        print("  note: RSA.generate missing, D3-04 case skipped");
    }
    const t = crypto.JWTSign({ a: 1 }, "secret-key", { alg: "HS256" });
    const parts = t.split(".");
    const sig = parts[2];
    const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const last = B64.indexOf(sig[sig.length - 1]);
    const twin = sig.slice(0, -1) + B64[last ^ 1];
    eq(sig.length % 4, 3, "an HS256 signature leaves two unused bits (precondition of the malleability case)");
    eq(errName(() => crypto.JWTVerify(parts[0] + "." + parts[1] + "." + twin, "secret-key", { algorithms: ["HS256"] })), "TypeError", "a signature with flipped unused bits is refused");
    eq(errName(() => crypto.JWTVerify(t + "=", "secret-key", { algorithms: ["HS256"] })), "TypeError", "a padded signature is refused");
    eq(JSON.stringify(crypto.JWTVerify(t, "secret-key", { algorithms: ["HS256"] })), '{"a":1}', "the canonical token verifies (control)");

    const b64u = (s) => { const bytes = new TextEncoder().encode(s); let bin = ""; for (const b of bytes) bin += String.fromCharCode(b); return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); };
    const head = b64u(JSON.stringify({ alg: "HS256", typ: "JWT", crit: ["exp2"] }));
    const body = b64u(JSON.stringify({ a: 1 }));
    const mac = crypto.HMAC("sha256", "secret-key", head + "." + body);
    let macBin = ""; for (const b of mac) macBin += String.fromCharCode(b);
    const critTok = head + "." + body + "." + btoa(macBin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    ok(/crit/.test(errMsg(() => crypto.JWTVerify(critTok, "secret-key", { algorithms: ["HS256"] }))), "a correctly signed token carrying `crit` is refused for that reason");
}

// ---- D3-02 / D3-11 -------------------------------------------------------------
{
    const ed = crypto.Ed25519Generate();
    const pem = crypto.Ed25519PemFromRaw(ed).privateKey;
    const viaObj = crypto.Ed25519PemToRaw({ toString() { return pem + "\n"; } });
    ok(viaObj && typeof viaObj === "object", "Ed25519PemToRaw reads a PEM that only exists as a temporary C string");
    eq(errName(() => crypto.Ed25519PemToRaw(pem + "é")), "none", "a PEM followed by a non-ASCII character parses without touching freed memory");
    const ENC = "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMIGjMF8GCSqGSIb3DQEFDTBSMDEGCSqGSIb3DQEFDDAkBBBvt0gDhtbIhWDbcG7e\nwmW8AgIIADAMBggqhkiG9w0CCQUAMB0GCWCGSAFlAwQBKgQQyvWn9XrCvHSfy1j3\nxgC1AARAU7lO0J/7Wz5N0kRkXh0hYp2aIl2kUlqDlPxxG6h8Ef4Mh0Cb0qHxE9bS\nR3nWZ5ZlVwJ5gq7W6m8xUuUuGwqzZQ==\n-----END ENCRYPTED PRIVATE KEY-----\n";
    const t0 = performance.now();
    eq(errName(() => crypto.Ed25519PemToRaw(ENC)), "TypeError", "an encrypted PEM is refused");
    ok(performance.now() - t0 < 2000, "and refused promptly, with no passphrase prompt");
}

// ---- D3-08 -----------------------------------------------------------------------
{
    const mk = () => { const h = new Hasher("shake128", { length: 300 }); h.update("abc"); return h; };
    const raw = mk().digest();
    let hex = "";
    for (const b of raw) hex += (b < 16 ? "0" : "") + b.toString(16);
    eq(raw.length, 300, "SHAKE128 digest() honours a 300-byte length (control)");
    eq(mk().digestHex(), hex, "digestHex() returns the same 300 bytes as digest()");
}

// ---- D3-13 -----------------------------------------------------------------------
{
    const kp = crypto.Ed25519Generate();
    const keyPem = crypto.Ed25519PemFromRaw(kp).privateKey;
    const a = crypto.X509.parse(crypto.X509.generateSelfSigned({ key: keyPem, subject: "w2.test" }));
    const b = crypto.X509.parse(crypto.X509.generateSelfSigned({ key: keyPem, subject: "w2.test" }));
    ok(a.serialNumber !== b.serialNumber, "two self-signed certificates get different serial numbers");
    ok(/^[0-9A-F]{32}$/.test(a.serialNumber), "the serial is a 16-byte value (got " + a.serialNumber + ")");
    eq(errName(() => crypto.X509.generateSelfSigned({ key: keyPem, sans: ["a.test,DNS:evil.test"] })), "TypeError", "a SAN entry carrying a comma is refused");
    eq(errName(() => crypto.X509.generateSelfSigned({ key: keyPem, sans: ["a.test\nDNS:evil.test"] })), "TypeError", "a SAN entry carrying a line break is refused");
    const okc = crypto.X509.parse(crypto.X509.generateSelfSigned({ key: keyPem, sans: ["a.test", "b.test"] }));
    ok(JSON.stringify(okc).includes("b.test") && !JSON.stringify(okc).includes("evil"), "ordinary SAN lists still work (control)");
}

// ---- N1-27 / N1-29 ----------------------------------------------------------------
{
    eq(errName(() => new TCPProxy({ port: 0, upstream: { port: 9 }, host: "" })), "RangeError", "TCPProxy refuses an empty host");
    eq(errName(() => new TCPProxy({ port: 0, upstream: { port: 9 }, hots: "x" })), "TypeError", "TCPProxy option bag stays strict");
    const p = new TCPProxy({ port: 0, host: "127.0.0.1", upstream: { host: "127.0.0.1", port: 9 } });
    p.start();
    ok(p.port > 0, "TCPProxy binds the requested host");
    p.close();

    const c = new HTTPClient();
    eq(errName(() => c.get("http://127.0.0.1:9/", "Content-Length : 5")), "TypeError", "a header string whose name is not a token is refused");
    eq(errName(() => c.get("http://127.0.0.1:9/", "no colon here")), "TypeError", "a header string without a colon is refused");
    c.close();
}

// ---- markdown ------------------------------------------------------------------------
{
    const time = (src) => { const t0 = performance.now(); const out = MarkdownToHTML(src); return [performance.now() - t0, out]; };
    time("warm *up* `x`");
    for (const [name, mk] of [
        ["star run", (n) => "*".repeat(n) + "a" + "*".repeat(n)],
        ["underscore run", (n) => "_".repeat(n) + "a"],
        ["tilde run", (n) => "~".repeat(n) + "a"],
    ]) {
        const [t1] = time(mk(20000)), [t4] = time(mk(80000));
        ok(t4 < Math.max(t1, 0.5) * 12, "markdown " + name + " is linear (20k " + t1.toFixed(1) + "ms, 80k " + t4.toFixed(1) + "ms)");
    }
    {
        let s = "";
        for (let i = 1; i <= 1200; i++) s += "`".repeat(i) + " ";
        const [t] = time(s);
        ok(t < 3000, "backtick runs of every length stay inside the work budget (" + t.toFixed(0) + "ms)");
    }
    eq(MarkdownToHTML("*****a*****").replace(/\s+$/, ""), "<p>**<strong>a</strong>**</p>", "long delimiter runs render exactly as before (control)");
    eq(MarkdownToHTML("****a**** ~~~b~~~ ______c").replace(/\s+$/, ""), "<p>*<strong>a</strong>* ~<del>b</del>~ ______c</p>", "mixed over-long runs render exactly as before (control)");
    eq(MarkdownToHTML("**b** and `c` and ~~d~~").replace(/\s+$/, ""), "<p><strong>b</strong> and <code>c</code> and <del>d</del></p>", "ordinary inline markup (control)");
    {
        const links = MarkdownToHTML("[a](http://x) ".repeat(2000));
        eq(links.split("<a ").length - 1, 2000, "2000 links in one paragraph all render");
    }
    {
        const block = "[a".repeat(2000);
        const plain = (block + "\n\n").repeat(200), quoted = ("> " + block + "\n\n").repeat(200);
        const [tp] = time(plain), [tq] = time(quoted);
        ok(tq < Math.max(tp, 1) * 6, "blockquotes share the document's work budget (plain " + tp.toFixed(1) + "ms, quoted " + tq.toFixed(1) + "ms)");
    }
}

// ---- html ---------------------------------------------------------------------------------
{
    let tag = "<a";
    for (let i = 0; i < 520; i++) tag += " a" + i + "=1";
    tag += ' x="<script>alert(1)</script>">t</a>';
    const tree = HTMLParse(tag);
    const flat = JSON.stringify(tree);
    ok(!flat.includes('"script"'), "attributes past the cap are discarded, never re-read as markup");
    const a = Array.isArray(tree) ? tree[0] : tree;
    eq(JSON.stringify(a.children), '["t"]', "the element keeps its single text child");
    const sl = HTMLParse('<a/href="/x">t</a>');
    const a2 = Array.isArray(sl) ? sl[0] : sl;
    eq(a2.attrs && a2.attrs.href, "/x", "a slash between attributes is whitespace, as in browsers");
    const sc = HTMLParse("<br/>after");
    eq(JSON.stringify(sc).includes("after"), true, "a real self-closing slash still works (control)");

    const doc = HTMLParse("<p>x</p><div id=a>y</div>");
    eq(new Selector("[constructor]").all(doc).length, 0, "[constructor] matches no element");
    eq(new Selector("[__proto__]").all(doc).length, 0, "[__proto__] matches no element");
    eq(new Selector("[toString]").all(doc).length, 0, "[toString] matches no element");
    eq(new Selector("[id]").all(doc).length, 1, "[id] matches the one element that has it (control)");
    eq(new Selector("[id=a]").all(doc).length, 1, "[id=a] value match (control)");

    const s7 = new Sanitizer({ allow: { a: ["href"] }, protocols: { "a.href": ["http", "https", "mailto", "ftp", "tel", "sms", "geo"] } });
    ok(s7.clean('<a href="geo:1,2">g</a>').includes("geo:1,2"), "the seventh scheme of one protocols key is honoured");
    ok(!s7.clean('<a href="javascript:1">g</a>').includes("javascript"), "an unlisted scheme is still dropped (control)");

    let deep = { name: "b", attrs: {}, children: ["x"] };
    for (let i = 0; i < 300; i++) deep = { name: "a", attrs: { href: "/p" }, children: [deep] };
    eq(errName(() => new Selector("a b").all(deep)), "RangeError", "Selector refuses a tree deeper than the parser's bound instead of skipping it");
    eq(errName(() => rewriteLinks(deep, (u) => u)), "RangeError", "rewriteLinks refuses the same tree instead of rewriting only the top 256 levels");
}

if (failures) {
    print("test_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_audit_w2_native_2 failed");
}
print("test_audit_w2_native_2: " + checks + " checks passed");
