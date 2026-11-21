// covers: dyna:crypto.RSA
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_rsa");
import {
    MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex,
    SHA256, SHA256Hex, SHA384, SHA384Hex, SHA512, SHA512Hex,
    CRC32, CRC32C, Hasher,
} from "dyna:hash";
import { HMAC, HMACHex, PBKDF2, RSA, Argon2id, Hmac, XXH3_64, Ed25519Generate, Ed25519PemToRaw, Ed25519PemFromRaw, X509, ECDSA, AESGCM } from "dyna:crypto";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
async function assertThrows(fn, msg) {
    n++;
    let threw = false;
    try { await fn(); } catch (e) { threw = true; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
}
function toHex(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i++) s += u8[i].toString(16).padStart(2, "0");
    return s;
}
function bytes(...b) { return new Uint8Array(b); }
function rep(byteVal, count) { return new Uint8Array(count).fill(byteVal); }

const HASH = {
    "md5":    { bin: MD5,    hex: MD5Hex,    size: 16 },
    "sha1":   { bin: SHA1,   hex: SHA1Hex,   size: 20 },
    "sha224": { bin: SHA224, hex: SHA224Hex, size: 28 },
    "sha256": { bin: SHA256, hex: SHA256Hex, size: 32 },
    "sha384": { bin: SHA384, hex: SHA384Hex, size: 48 },
    "sha512": { bin: SHA512, hex: SHA512Hex, size: 64 },
};

await __kitS.section("BEGIN PRIVATE KEY", async () => {
    const PRIV1024 = `-----BEGIN PRIVATE KEY-----
MIICdwIBADANBgkqhkiG9w0BAQEFAASCAmEwggJdAgEAAoGBAMbU16LBvUXpNadE
/KZNWwr0Q1bgpuKCJX5W8iHB/0KoWFuc+2mLxTRikBJT8m1H4EWuFgmFAUwCISmc
sH1f84n12k10as48H3rmPmUDRCKoXn0fJ5v7L6a9VL6oni/O1WSIt4SPKWoyZ082
bpKqCjgdawtkdSua11rOuutAnrVDAgMBAAECgYBo1GUQiab6Chfgpe2y3IwIvXoX
fwBd4hJUhxDIHZkwksNrIdwn7Ko/kUu2bg4d8cxb0IoL4NUrNuA3KUagA4vJndD9
lv5K69bm2XWmQiyemtmHaGbLn7uUhgmp7HiaUB1dUb5wD2+JVAfCSff+mFI2kh+R
q50fFdxYMXcCQCjJMQJBAO6PTxzrpwTObftpOZsn24CcKg+LNoPTsLSR9YF4++oR
+06Bo4CnEVBPAHKAK6qYOGH+xde5XAHO/T3quTty+NsCQQDVXgJZoZ68ULJ3eOiU
YxJbH8GcHKjAAmwqKKp6oeVvc/v9WRwkxqMI5Q7SMt8MqCTgvZYCVjb9PgzPW5a0
vU25AkA6kaCgvxJgUbYa1Xi5KQ3xA2YoLsuRjfWCP5uA0A05KHUEjztvKJVNnDfF
5MXTuk+Nb5lvHykBoKn3+5rCh7WlAkEAo1qBFBxPyj9CE4QHFKww/oD2ms26SJ7S
LwjJVHAkl0rGLzYM/P7zDTlJOpbl++uzqDid45YcLZBXAF5PqHqSSQJBAI+YE/xh
7NCU52FIRodZEnclRSiIEcnuubj8QUdU6y1a04/Kket0NbXL1kvjW3igUVwmNN/Q
l4oaODk7Gjw68LE=
-----END PRIVATE KEY-----`;
    const PUB1024 = `-----BEGIN PUBLIC KEY-----
MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDG1Neiwb1F6TWnRPymTVsK9ENW
4KbigiV+VvIhwf9CqFhbnPtpi8U0YpASU/JtR+BFrhYJhQFMAiEpnLB9X/OJ9dpN
dGrOPB965j5lA0QiqF59Hyeb+y+mvVS+qJ4vztVkiLeEjylqMmdPNm6Sqgo4HWsL
ZHUrmtdazrrrQJ61QwIDAQAB
-----END PUBLIC KEY-----`;
    await assertThrows(() => RSA.sign("sha256", PRIV1024, "msg"),
        "RSA.sign: refuses a 1024-bit private key");
    await assertThrows(() => RSA.verify("sha256", PUB1024, "msg", new Uint8Array(128)),
        "RSA.verify: refuses a 1024-bit public key");
    let msg1024 = null;
    try { RSA.sign("sha256", PRIV1024, "msg"); } catch (e) { msg1024 = e.message; }
    assert(msg1024 !== null && /2048/.test(msg1024),
        "RSA.sign: the 1024-bit refusal mentions the 2048-bit floor");
});

await __kitS.section("Argon2id.hash PHC default (argon2-cffi reference string)", async () => {
    const salt = new TextEncoder().encode("somesalt");
    const phc = await Argon2id.hash("password", salt, { iterations: 2, memory: 65536, parallelism: 1 });
    assert(phc === "$argon2id$v=19$m=65536,t=2,p=1$c29tZXNhbHQ$CTFhFdXPJO1aFaMaO6Mm5c8y7cJHAph8ArZWb2GRPPc",
       "Argon2id.hash PHC default (argon2-cffi reference string)");
    assert(await Argon2id.verify(phc, "password") === true, "PHC verify accepts");
    assert(await Argon2id.verify(phc, "Password") === false, "PHC verify rejects");
    assert((await Argon2id.hash("password", salt, { iterations: 2, memory: 64, parallelism: 1, encoded: false })).length === 32,
       "{encoded:false} keeps the raw byte form");

    assert(XXH3_64("") === "2d06800538d394c2", "XXH3_64 empty constant (xxHash 0.8 spec)");

    if (RSA && RSA.signPSS) {
        const kp = RSA.generate(2048);
        const sig = RSA.signPSS("sha256", kp.privateKey, "pss");
        assert(sig.length === 256 && RSA.verifyPSS("sha256", kp.publicKey, "pss", sig) === true,
           "RSA.signPSS/verifyPSS roundtrip");
        const sealed = RSA.OAEP.seal(kp.publicKey, "oaep");
        assert(new TextDecoder().decode(RSA.OAEP.open(kp.privateKey, sealed)) === "oaep",
           "RSA.OAEP seal/open roundtrip");
        assert(Ed25519PemToRaw(Ed25519PemFromRaw(Ed25519Generate()).privateKey).privateKey.length === 32,
           "Ed25519 PEM<->raw roundtrip");
        const cert = X509.generateSelfSigned({ key: kp.privateKey, sans: ["a.test", "127.0.0.1", "u@a.test"] });
        const info = X509.parse(cert);
        assert(info.sans.dns[0] === "a.test" && info.sans.ip.length === 1 && info.sans.email.length === 1,
           "X509.generateSelfSigned sans classified");
        assert(ECDSA.generate("P-521").publicKey.includes("BEGIN"), "ECDSA P-521 generate");
        const aes = new AESGCM(new Uint8Array(32).fill(2));
        const out = new Uint8Array(64);
        assert(aes.sealInto(out, new Uint8Array(12).fill(1), "x") === 17, "AESGCM.sealInto bytes written");
        aes.close();
    }
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
