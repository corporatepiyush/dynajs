/* test_crypto_aead.js -- AESGCM and ChaCha20Poly1305 (design 12).
 *
 * The oracle is the SPECIFICATION's own vectors, not a round trip: seal/open
 * agreeing with each other passes just as well when both are wrong in the same
 * way. RFC 8439 s2.8.2 for ChaCha20-Poly1305, the GCM spec's Test Case 3 for
 * AES-128-GCM.
 *
 * The interesting cases are the REFUSALS: a flipped tag, a flipped ciphertext
 * byte, altered AAD, a short message, a wrong-size nonce. Each must throw.
 *
 * Needs CONFIG_TLS=y (the ciphers are absent otherwise, by design).
 */
import * as crypto from "dyna:crypto";

let pass = 0, fail = 0;
function ok(cond, what, detail) {
    if (cond) { pass++; print("  ok    " + what); }
    else { fail++; print("  FAIL  " + what + (detail ? "  [" + detail + "]" : "")); }
}
function throws(fn, what) {
    let threw = false, msg = "";
    try { fn(); } catch (e) { threw = true; msg = String(e.message || e); }
    ok(threw, what, threw ? "" : "did NOT throw");
    return msg;
}
const hex = (s) => {
    const b = new Uint8Array(s.length / 2);
    for (let i = 0; i < b.length; i++) b[i] = parseInt(s.substr(i * 2, 2), 16);
    return b;
};
const tohex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");

if (!crypto.AESGCM || !crypto.ChaCha20Poly1305) {
    print("test_crypto_aead: SKIP -- built without CONFIG_TLS, ciphers absent");
    print("test_crypto_aead: 0 passed, 0 failed");
} else {
    /* ---------- RFC 8439 s2.8.2, the AEAD example ---------- */
    {
        const key   = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        const nonce = hex("070000004041424344454647");
        const aad   = hex("50515253c0c1c2c3c4c5c6c7");
        const pt    = new TextEncoder().encode(
            "Ladies and Gentlemen of the class of '99: If I could offer you " +
            "only one tip for the future, sunscreen would be it.");
        const wantCt = "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a7" +
                       "36ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b29" +
                       "05d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4" +
                       "fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b" +
                       "6116";
        const wantTag = "1ae10b594f09e26a7e902ecbd0600691";

        const c = new crypto.ChaCha20Poly1305(key);
        const sealed = c.seal(nonce, pt, aad);
        ok(tohex(sealed.slice(0, sealed.length - 16)) === wantCt,
           "ChaCha20-Poly1305 ciphertext matches RFC 8439 s2.8.2");
        ok(tohex(sealed.slice(sealed.length - 16)) === wantTag,
           "ChaCha20-Poly1305 tag matches RFC 8439 s2.8.2");
        const back = c.open(nonce, sealed, aad);
        ok(new TextDecoder().decode(back) === new TextDecoder().decode(pt),
           "and it opens back to the same plaintext");

        /* --- refusals: each must THROW, never return a value --- */
        const bad1 = sealed.slice(); bad1[bad1.length - 1] ^= 1;
        throws(() => c.open(nonce, bad1, aad), "a flipped TAG bit is refused");
        const bad2 = sealed.slice(); bad2[0] ^= 1;
        throws(() => c.open(nonce, bad2, aad), "a flipped CIPHERTEXT bit is refused");
        const badAad = aad.slice(); badAad[0] ^= 1;
        throws(() => c.open(nonce, sealed, badAad), "altered AAD is refused");
        throws(() => c.open(nonce, sealed), "MISSING aad is refused (it is authenticated)");
        const badNonce = nonce.slice(); badNonce[0] ^= 1;
        throws(() => c.open(badNonce, sealed, aad), "a different nonce is refused");
        throws(() => c.open(nonce, sealed.slice(0, 8), aad),
               "a message shorter than its tag is refused");
        throws(() => c.open(hex("0011223344"), sealed, aad),
               "a wrong-SIZE nonce is refused");
        throws(() => new crypto.ChaCha20Poly1305(hex("0011223344")),
               "a short key is refused at construction");
    }

    /* ---------- GCM spec Test Case 3 (AES-128, no AAD) ---------- */
    {
        const key   = hex("feffe9928665731c6d6a8f9467308308");
        const nonce = hex("cafebabefacedbaddecaf888");
        const pt    = hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da" +
                          "2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525" +
                          "b16aedf5aa0de657ba637b391aafd255");
        const wantCt = "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e0" +
                       "35c17e2329aca12e21d514b25466931c7d8f6a5aac84aa05" +
                       "1ba30b396a0aac973d58e091473f5985";
        const wantTag = "4d5c2af327cd64a62cf35abd2ba6fab4";

        const g = new crypto.AESGCM(key);
        const sealed = g.seal(nonce, pt);
        ok(tohex(sealed.slice(0, sealed.length - 16)) === wantCt,
           "AES-128-GCM ciphertext matches GCM Test Case 3");
        ok(tohex(sealed.slice(sealed.length - 16)) === wantTag,
           "AES-128-GCM tag matches GCM Test Case 3");
        ok(tohex(g.open(nonce, sealed)) === tohex(pt),
           "and it opens back to the same plaintext");

        const forged = sealed.slice(); forged[forged.length - 1] ^= 0x80;
        throws(() => g.open(nonce, forged), "AESGCM refuses a forged tag");

        /* Key length selects the variant, so all three must construct. */
        for (const n of [16, 24, 32]) {
            const k = new Uint8Array(n).fill(7);
            const gg = new crypto.AESGCM(k);
            const s = gg.seal(nonce, pt);
            ok(tohex(gg.open(nonce, s)) === tohex(pt),
               "AES-" + n * 8 + "-GCM round trips");
        }
        throws(() => new crypto.AESGCM(new Uint8Array(20)),
               "a 20-byte key is refused (not an AES size)");

        /* close() must make the key unusable, not merely tidy. */
        const g2 = new crypto.AESGCM(key);
        g2.close();
        throws(() => g2.seal(nonce, pt), "seal after close() is refused");
    }

    /* An empty message is legal and still authenticated. */
    {
        const c = new crypto.ChaCha20Poly1305(new Uint8Array(32).fill(3));
        const n = new Uint8Array(12).fill(9);
        const s = c.seal(n, new Uint8Array(0));
        ok(s.length === 16, "an empty message seals to exactly the tag");
        ok(c.open(n, s).length === 0, "and opens back to empty");
        const bad = s.slice(); bad[0] ^= 1;
        throws(() => c.open(n, bad), "an empty message's tag is still checked");
    }

    print("test_crypto_aead: " + pass + " passed, " + fail + " failed");
    if (fail) throw new Error("test_crypto_aead: " + fail + " failures");
}
