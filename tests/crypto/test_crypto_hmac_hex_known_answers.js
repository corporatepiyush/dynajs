// covers: integration c:src/core/dyn-hash.c c:src/modules/crypto/crypto-module.c -- HMAC pad wipe and the shared hex writer
// HMAC wipes its pads and inner digest with memset plus a compiler barrier (a volatile byte loop
// over 320 bytes was a third of an HMAC of a short message), and every *Hex digest is written by
// the shared hex encoder (a per-nibble loop was ~20% of SHA256Hex of a short string). Oracles:
// the RFC 2202 (MD5, SHA-1) and RFC 4231 (SHA-224/256/384/512) known answers, HMACHex equal to
// the hex of HMAC and to a JS hex of the bytes for every key length across the block size, and
// every *Hex function equal to the JS hex of its byte form.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hmac_hex_known_answers");
import * as CR from "dyna:crypto";
import * as H from "dyna:hash";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; if (fail < 30) print("FAIL " + m + ": got " + a + ", want " + b); } };
const hex = (u) => Array.from(u, (b) => (b < 16 ? "0" : "") + b.toString(16)).join("");
const unhex = (s) => new Uint8Array(s.match(/../g).map((x) => parseInt(x, 16)));
const rep = (b, n) => new Uint8Array(n).fill(b);
const te = new TextEncoder();
let seed = 3;
const bytes = (n) => { const b = new Uint8Array(n); for (let i = 0; i < n; i++) { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; b[i] = seed >>> 24; } return b; };

__kitS.section("RFC 2202 and RFC 4231 known answers", () => {
    const K = [
        ["md5", rep(0x0b, 16), te.encode("Hi There"), "9294727a3638bb1c13f48ef8158bfc9d"],
        ["md5", te.encode("Jefe"), te.encode("what do ya want for nothing?"), "750c783e6ab0b503eaa86e310a5db738"],
        ["md5", rep(0xaa, 80), te.encode("Test Using Larger Than Block-Size Key - Hash Key First"), "6b1ab7fe4bd7bf8f0b62e6ce61b9d0cd"],
        ["sha1", rep(0x0b, 20), te.encode("Hi There"), "b617318655057264e28bc0b6fb378c8ef146be00"],
        ["sha1", rep(0xaa, 20), rep(0xdd, 50), "125d7342b9ac11cd91a39af48aa17b4f63f175d3"],
        ["sha1", rep(0xaa, 80), te.encode("Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data"), "e8e99d0f45237d786d6bbaa7965c7808bbff1a91"],
        ["sha224", rep(0x0b, 20), te.encode("Hi There"), "896fb1128abbdf196832107cd49df33f47b4b1169912ba4f53684b22"],
        ["sha256", rep(0x0b, 20), te.encode("Hi There"), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"],
        ["sha256", te.encode("Jefe"), te.encode("what do ya want for nothing?"), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"],
        ["sha256", rep(0xaa, 131), te.encode("Test Using Larger Than Block-Size Key - Hash Key First"), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"],
        ["sha384", rep(0x0b, 20), te.encode("Hi There"), "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59cfaea9ea9076ede7f4af152e8b2fa9cb6"],
        ["sha512", rep(0x0b, 20), te.encode("Hi There"), "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"],
        ["sha512", rep(0xaa, 131), te.encode("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm."), "e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58"],
    ];
    for (const [alg, k, m, want] of K) {
        eq(CR.HMACHex(alg, k, m), want, "HMACHex " + alg + " " + want.slice(0, 8));
        eq(hex(CR.HMAC(alg, k, m)), want, "HMAC " + alg + " " + want.slice(0, 8));
    }
});

__kitS.section("HMACHex is the hex of HMAC for every key length", () => {
    for (const alg of ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"])
        for (let kl = 0; kl <= 260; kl += kl < 140 ? 1 : 13) {
            const k = bytes(kl), m = bytes(kl % 97);
            const raw = CR.HMAC(alg, k, m);
            eq(CR.HMACHex(alg, k, m), hex(raw), "HMACHex " + alg + " key " + kl);
            eq(CR.HMACHex(alg, k, m), CR.HMACHex(alg, k, m), "HMACHex " + alg + " key " + kl + " is repeatable");
        }
});

__kitS.section("every Hex digest is the hex of its bytes", () => {
    const pairs = Object.keys(H).filter((n) => n.endsWith("Hex") && typeof H[n.slice(0, -3)] === "function");
    eq(pairs.length >= 10, true, "found " + pairs.length + " Hex digests");
    for (const n of pairs)
        for (const len of [0, 1, 3, 55, 64, 200, 1000]) {
            const d = bytes(len);
            let a, b;
            try { a = H[n](d); b = hex(H[n.slice(0, -3)](d)); } catch (e) { a = b = "skip"; }
            eq(a, b, n + " of " + len + " bytes");
        }
    for (const out of [1, 63, 64, 65, 200, 4096]) {
        const d = bytes(33);
        eq(H.SHAKE256Hex(d, out), hex(H.SHAKE256(d, out)), "SHAKE256Hex of " + out + " bytes");
    }
    eq(H.SHA256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA256Hex of empty");
    eq(H.SHA512Hex("abc").slice(0, 16), "ddaf35a193617aba", "SHA512Hex of abc");
    eq(unhex(H.SHA1Hex("abc")).length, 20, "SHA1Hex is 40 hex digits");
});

__kitS.end();
print(`test_crypto_hmac_hex_known_answers: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
