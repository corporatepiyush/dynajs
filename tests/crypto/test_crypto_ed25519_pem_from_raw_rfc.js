// covers: dyna:crypto.Ed25519PemFromRaw
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_ed25519_pem_from_raw_rfc");
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

__kitS.section("PKCS#8 label per RFC 7468", () => {
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
});
__kitS.end();

function hexToBytes(h) {
    const u = new Uint8Array(h.length / 2);
    for (let i = 0; i < u.length; i++)
        u[i] = parseInt(h.substr(i * 2, 2), 16);
    return u;
}

print("test_crypto_pem_rfc: " + n + " assertions, " + bad + " failures, " + skip + " skipped");
if (bad) throw new Error(bad + " failures");
