/* test_crypto_curve.js -- Ed25519 signatures and X25519 agreement (design 12).
 *
 * Oracle = the RFCs' own vectors, not a sign/verify round trip: a pair that
 * agrees with itself passes just as well when both halves are wrong. RFC 8032
 * s7.1 pins Ed25519 signatures byte-for-byte; RFC 7748 s6.1 pins the X25519
 * shared secret AND both public keys derived from their private halves.
 *
 * Needs CONFIG_TLS=y (absent otherwise, by design).
 */
import * as crypto from "dyna:crypto";

let pass = 0, fail = 0;
function ok(cond, what, detail) {
    if (cond) { pass++; print("  ok    " + what); }
    else { fail++; print("  FAIL  " + what + (detail ? "  [" + detail + "]" : "")); }
}
function throws(fn, what) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    ok(t, what, t ? "" : "did NOT throw");
}
const hex = (s) => {
    const b = new Uint8Array(s.length / 2);
    for (let i = 0; i < b.length; i++) b[i] = parseInt(s.substr(i * 2, 2), 16);
    return b;
};
const tohex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");

if (!crypto.Ed25519Sign) {
    print("test_crypto_curve: SKIP -- built without CONFIG_TLS");
    print("test_crypto_curve: 0 passed, 0 failed");
} else {
    /* ---------- RFC 8032 s7.1: Ed25519 test vectors ---------- */
    const VEC = [
        { name: "TEST 1 (empty message)",
          priv: "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
          pub:  "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
          msg:  "",
          sig:  "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e0652249015" +
                "55fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { name: "TEST 2 (one byte)",
          priv: "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
          pub:  "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
          msg:  "72",
          sig:  "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da" +
                "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { name: "TEST 3 (two bytes)",
          priv: "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
          pub:  "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
          msg:  "af82",
          sig:  "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac" +
                "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    ];
    for (const v of VEC) {
        const sig = crypto.Ed25519Sign(hex(v.priv), hex(v.msg));
        ok(tohex(sig) === v.sig, "Ed25519 " + v.name + " signature matches RFC 8032");
        ok(crypto.Ed25519Verify(hex(v.pub), hex(v.msg), hex(v.sig)) === true,
           "Ed25519 " + v.name + " verifies against the RFC's public key");
    }

    /* Forgeries must be REJECTED, and each differs in exactly one way. */
    const v = VEC[1];
    const flip = (h, i) => { const b = hex(h); b[i] ^= 1; return b; };
    ok(crypto.Ed25519Verify(hex(v.pub), hex(v.msg), flip(v.sig, 0)) === false,
       "a flipped signature byte does not verify");
    ok(crypto.Ed25519Verify(hex(v.pub), hex(v.msg), flip(v.sig, 63)) === false,
       "a flipped byte in the signature's SECOND half does not verify");
    ok(crypto.Ed25519Verify(flip(v.pub, 0), hex(v.msg), hex(v.sig)) === false,
       "the wrong public key does not verify");
    ok(crypto.Ed25519Verify(hex(v.pub), hex("73"), hex(v.sig)) === false,
       "a different message does not verify");
    ok(crypto.Ed25519Verify(hex(v.pub), hex(v.msg), hex("00")) === false,
       "a wrong-SIZE signature returns false, it does not throw " +
       "(an attacker supplies that, and it must look like any other forgery)");
    throws(() => crypto.Ed25519Verify(hex("00"), hex(v.msg), hex(v.sig)),
           "a wrong-size PUBLIC KEY throws -- that is a caller error, not a forgery");
    throws(() => crypto.Ed25519Sign(hex("0011"), hex("00")),
           "a wrong-size private key is refused");

    /* Generation must produce a usable, self-consistent pair. */
    {
        const kp = crypto.Ed25519Generate();
        ok(kp.privateKey.length === 32 && kp.publicKey.length === 32,
           "Ed25519Generate returns a 32/32 pair");
        const m = new TextEncoder().encode("hello");
        ok(crypto.Ed25519Verify(kp.publicKey, m, crypto.Ed25519Sign(kp.privateKey, m)),
           "a generated key signs and verifies");
        const kp2 = crypto.Ed25519Generate();
        ok(tohex(kp2.privateKey) !== tohex(kp.privateKey),
           "two generated keys differ (the generator is not a constant)");
        ok(crypto.Ed25519Verify(kp2.publicKey, m, crypto.Ed25519Sign(kp.privateKey, m)) === false,
           "and one key's signature does not verify under the other");
    }

    /* ---------- RFC 7748 s6.1: X25519 ---------- */
    {
        const aPriv = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
        const aPub  = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
        const bPriv = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
        const bPub  = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
        const want  = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

        ok(tohex(crypto.X25519Derive(hex(aPriv), hex(bPub))) === want,
           "X25519 shared secret matches RFC 7748 s6.1 (Alice's side)");
        ok(tohex(crypto.X25519Derive(hex(bPriv), hex(aPub))) === want,
           "and Bob's side derives the SAME secret");

        const kp = crypto.X25519Generate();
        ok(kp.privateKey.length === 32 && kp.publicKey.length === 32,
           "X25519Generate returns a 32/32 pair");
        ok(tohex(crypto.X25519Derive(kp.privateKey, hex(bPub))) !==
           tohex(crypto.X25519Derive(hex(aPriv), hex(bPub))),
           "a fresh key derives a different secret");
        throws(() => crypto.X25519Derive(hex("00"), hex(bPub)),
               "a wrong-size private key is refused");
        /* An all-zero peer point is small-order: the derive must FAIL, not
           hand back an all-zero "secret" the caller would then use. */
        throws(() => crypto.X25519Derive(hex(aPriv), hex("00".repeat(32))),
               "an all-zero (small-order) peer key is refused, not silently zero");
    }

    print("test_crypto_curve: " + pass + " passed, " + fail + " failed");
    if (fail) throw new Error("test_crypto_curve: " + fail + " failures");
}
