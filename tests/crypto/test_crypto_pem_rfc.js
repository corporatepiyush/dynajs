// flags: --std
import { Ed25519PemToRaw, Ed25519PemFromRaw, X25519PemToRaw, X25519PemFromRaw,
         Ed25519Sign, Ed25519Verify } from "dyna:crypto";

let n = 0, bad = 0, skip = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { ok(a === b, what + " (got " + a + ", want " + b + ")"); }
function throws(fn, re, what) {
    let t = null;
    try { fn(); } catch (e) { t = e; }
    ok(t !== null, "expected throw: " + what);
    if (t !== null && re)
        ok(re.test(String(t && t.message)),
           what + " (message [" + String(t && t.message) + "] !~ " + re + ")");
}
const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");

const RFC_ED_PRIV_PEM =
    "-----BEGIN PRIVATE KEY-----\n" +
    "MC4CAQAwBQYDK2VwBCIEINTuctv5E1hK1bbY8fdp+K06/nwoy/HU++CXqI9EdVhC\n" +
    "-----END PRIVATE KEY-----\n";
const RFC_ED_PRIV_ATTR_PEM =
    "-----BEGIN PRIVATE KEY-----\n" +
    "MHICAQEwBQYDK2VwBCIEINTuctv5E1hK1bbY8fdp+K06/nwoy/HU++CXqI9EdVhC\n" +
    "oB8wHQYKKoZIhvcNAQkJFDEPDA1DdXJkbGUgQ2hhaXJzgSEAGb9ECWmEzf6FQbrB\n" +
    "Z9w7lshQhqowtrbLDFw4rXAxZuE=\n" +
    "-----END PRIVATE KEY-----\n";
const RFC_ED_PUB_PEM =
    "-----BEGIN PUBLIC KEY-----\n" +
    "MCowBQYDK2VwAyEAGb9ECWmEzf6FQbrBZ9w7lshQhqowtrbLDFw4rXAxZuE=\n" +
    "-----END PUBLIC KEY-----\n";
const RFC_X_PUB_PEM =
    "-----BEGIN PUBLIC KEY-----\n" +
    "MCowBQYDK2VuAyEAhSDwCYkwp1R0i33ctD73Wg2/Og0mOBr066SpjqqbTmo=\n" +
    "-----END PUBLIC KEY-----\n";
const ED_RAW_PRIV = "d4ee72dbf913584ad5b6d8f1f769f8ad3afe7c28cbf1d4fbe097a88f44755842";
const ED_RAW_PUB  = "19bf44096984cdfe8541bac167dc3b96c85086aa30b6b6cb0c5c38ad703166e1";
const X_RAW_PUB   = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";

{
    const k = Ed25519PemToRaw(RFC_ED_PRIV_PEM);
    eq(hex(k.privateKey), ED_RAW_PRIV, "RFC 8410 s10.3 raw private key");
    eq(hex(k.publicKey), ED_RAW_PUB,
       "the derived public half matches RFC 8410 s10.2's [1] field");
    // The attributes form is a valid PKCS#8 (the public key rides in the
    // implicit [1] tag), but some libcrypto builds refuse to decode it -
    // observed on Alpine's OpenSSL 3.3.7 with "sequence length mismatch",
    // which accepts the plain form. Probing with the PLAIN key keeps this a
    // capability skip: if that one fails, the engine is broken and this must
    // still fail loudly.
    let attrUsable = true;
    try { Ed25519PemToRaw(RFC_ED_PRIV_ATTR_PEM); }
    catch (e) {
        let plainOk = true;
        try { Ed25519PemToRaw(RFC_ED_PRIV_PEM); }
        catch (e2) { plainOk = false; }
        if (plainOk) {
            attrUsable = false;
            n++; skip++;
            print("  SKIP  RFC 8410 s10.3 attributes PEM: this libcrypto "
                  + "cannot decode the attributes form (" + e.message + ")");
        } else {
            ok(false, "attributes PEM failed AND the plain one did too");
        }
    }
    if (attrUsable) {
        const a = Ed25519PemToRaw(RFC_ED_PRIV_ATTR_PEM);
        eq(hex(a.privateKey), ED_RAW_PRIV, "the attributes variant carries the same private key");
        eq(hex(a.publicKey), ED_RAW_PUB, "and the same public key");
    }
    const p = Ed25519PemToRaw(RFC_ED_PUB_PEM);
    ok(p.privateKey === undefined, "an SPKI PEM has no private half");
    eq(hex(p.publicKey), ED_RAW_PUB, "RFC 8410 s4/10.1 raw public key");
    const x = X25519PemToRaw(RFC_X_PUB_PEM);
    ok(x.privateKey === undefined, "the X25519 SPKI has no private half");
    eq(hex(x.publicKey), X_RAW_PUB, "RFC 8410 s10.2 raw X25519 public key");
    const sig = Ed25519Sign(k.privateKey, new Uint8Array([1, 2, 3]));
    eq(Ed25519Verify(p.publicKey, new Uint8Array([1, 2, 3]), sig), true,
       "the RFC 8410 key pair signs and verifies");
}

{
    const privBytes = hexToBytes(ED_RAW_PRIV), pubBytes = hexToBytes(ED_RAW_PUB);
    const back = Ed25519PemFromRaw({ privateKey: privBytes });
    ok(/^-----BEGIN PRIVATE KEY-----\n/.test(back.privateKey),
       "PKCS#8 label per RFC 7468");
    ok(!back.publicKey, "a private-only input writes exactly the halves given");
    eq(hex(Ed25519PemToRaw(back.privateKey).privateKey), ED_RAW_PRIV,
       "PemFromRaw -> PemToRaw returns the cited raw key");
    const pair = Ed25519PemFromRaw({ privateKey: privBytes, publicKey: pubBytes });
    ok(/^-----BEGIN PUBLIC KEY-----\n/.test(pair.publicKey),
       "SPKI label per RFC 7468");
    eq(hex(Ed25519PemToRaw(pair.publicKey).publicKey), ED_RAW_PUB,
       "regenerated SPKI reads back to the cited raw key");
    const pubOnly = Ed25519PemFromRaw({ publicKey: pubBytes });
    ok(!pubOnly.privateKey, "public-only input writes only the SPKI");
    eq(hex(X25519PemToRaw(X25519PemFromRaw({
        publicKey: hexToBytes(X_RAW_PUB) }).publicKey).publicKey), X_RAW_PUB,
       "X25519 SPKI round-trips");
}

{
    const SHORT1 =
        "-----BEGIN PRIVATE KEY-----\n" +
        "MFICAQEwBQYDK2VwBCIEIC3GfeUYbZGTAhwLEE2cbvJL7ivTlcy17VottfN6L8Hw\n" +
        "oSIDIADBfk2Lv/J8H7YYwj/OmIcDx++jzVkKrKwS0/HjyQyM\n" +
        "-----END PRIVATE KEY-----\n";
    const SHORT2 =
        "-----BEGIN PRIVATE KEY-----\n" +
        "MFICAQEwBQYDK2VwBCIEILJXn1VaLqvausjUaZexwI/ozmOFjfEk78KcYN+7hsNJ\n" +
        "oSIDIACdQhJwzi/MCGcsQeQnIUh2JFybDxSrZxuLudJmpJLk\n" +
        "-----END PRIVATE KEY-----\n";
    for (const [i, pem] of [[1, SHORT1], [2, SHORT2]]) {
        let k = null, refused = false;
        try { k = Ed25519PemToRaw(pem); } catch (e) { refused = true; }
        ok(refused || (k.privateKey.length === 32 && k.publicKey.length === 32),
           "Appendix A short key #" + i + ": refused OR repaired to 32-byte " +
           "halves, never a 31-byte raw key");
    }
    const UNMASKED =
        "-----BEGIN PRIVATE KEY-----\n" +
        "MFMCAQEwBQYDK2VuBCIEIPj///////////////////////////////////////8/\n" +
        "oSMDIQCEfA0sN1I082XmYJVRh6NzWg92E9FgnTpqTYxTrqpaIg==\n" +
        "-----END PRIVATE KEY-----\n";
    let raw = null, refused = false;
    try { raw = X25519PemToRaw(UNMASKED); } catch (e) { refused = true; }
    ok(refused || hex(raw.privateKey) ===
       "f8ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff3f",
       "the Appendix A unmasked X25519 key is refused OR returned byte-exact");
}

{
    throws(() => Ed25519PemToRaw(""), /not a parseable/, "empty input refuses");
    throws(() => Ed25519PemToRaw("garbage"), /not a parseable/, "no PEM at all");
    throws(() => Ed25519PemToRaw("-----BEGIN PRIVATE KEY-----\n!!!\n-----END PRIVATE KEY-----"),
           /not a parseable/, "non-base64 body refuses");
    throws(() => Ed25519PemToRaw("-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEINTu\n-----END PRIVATE KEY-----"),
           /not a parseable/, "truncated DER refuses");
    throws(() => Ed25519PemToRaw(RFC_ED_PUB_PEM.replace("PUBLIC", "PRIVATE")),
           /not a parseable|wrong algorithm/,
           "SPKI bytes under a PRIVATE label refuse");
    throws(() => X25519PemToRaw(RFC_ED_PUB_PEM), /wrong algorithm/,
           "an Ed25519 PEM refuses in the X25519 converter");
    throws(() => Ed25519PemToRaw(RFC_X_PUB_PEM), /wrong algorithm/,
           "an X25519 PEM refuses in the Ed25519 converter");
    throws(() => Ed25519PemFromRaw({}), /expected \{ privateKey\?, publicKey \}/,
           "an empty bag refuses");
    throws(() => Ed25519PemFromRaw({ privateKey: hexToBytes("00".repeat(31)) }),
           /32-byte/, "31 raw bytes refuse");
    throws(() => Ed25519PemFromRaw({ publicKey: hexToBytes("00".repeat(33)) }),
           /32-byte/, "33 raw bytes refuse");
    throws(() => Ed25519PemToRaw(), /not a parseable/, "missing argument refuses");
}

function hexToBytes(h) {
    const u = new Uint8Array(h.length / 2);
    for (let i = 0; i < u.length; i++)
        u[i] = parseInt(h.substr(i * 2, 2), 16);
    return u;
}

print("test_crypto_pem_rfc: " + n + " assertions, " + bad + " failures, " + skip + " skipped");
if (bad) throw new Error(bad + " failures");
