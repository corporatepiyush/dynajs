// covers: dyna:crypto.Argon2id
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_argon2id_crypto_cov_bytes_compress_encoding_hash");
import { MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex, SHA256, SHA256Hex, SHA384, SHA384Hex, SHA512, SHA512Hex, CRC32, CRC32C, SHA3_224, SHA3_224Hex, SHA3_256, SHA3_256Hex, SHA3_384, SHA3_384Hex, SHA3_512, SHA3_512Hex, Keccak256, Keccak256Hex, SHAKE128, SHAKE128Hex, SHAKE256, SHAKE256Hex, BLAKE3, BLAKE3Hex, BLAKE2b, BLAKE2bHex, BLAKE2s, BLAKE2sHex, Murmur3_128, Murmur3_128Hex, XXHash32, XXHash64, Hasher } from "dyna:hash";
import * as CryptoNS from "dyna:crypto";

const { HMAC, HMACHex, Hmac, PBKDF2, HKDF, RandomBytes, TimingSafeEqual, JWTSign, JWTVerify } = CryptoNS;
const HAS_TLS = typeof CryptoNS.Scrypt === "function" && typeof CryptoNS.AESGCM === "function";
let n=0,fails=0;
function assert(c,m){n++; if(!c){fails++; print("FAIL: "+m)}}
function eq(a,b,m){assert(a===b,m+" got "+a+" want "+b)}
async function throws(fn,re,m){n++; try{fn(); fails++; print("FAIL: "+m+" did not throw")}catch(e){if(re && !re.test(String(e.message))) {fails++; print("FAIL: "+m+" wrong msg: "+e.message)}}}
function u8(...b){ return new Uint8Array(b); }
function eqArr(a,b){ if(a.length!==b.length) return false; for(let i=0;i<a.length;i++) if(a[i]!==b[i]) return false; return true; }
function hex(u){ return Array.from(u,b=>b.toString(16).padStart(2,"0")).join(""); }
function logStep(s){ try{ }catch(e){} }
logStep("bytes start");
print("encoding done: "+n+" assertions");

await __kitS.section("HMAC sha256 digest size", async () => {
    const mac = HMAC("sha256", "key", "msg");
    eq(mac.length, 32, "HMAC sha256 digest size");
    eq(HMACHex("sha256", "key", "msg").length, 64, "HMACHex length");

    const hs = new Hmac("sha256", "k");
    hs.update("a"); hs.update("b");
    eq(hs.digestHex(), HMACHex("sha256", "k", "ab"), "streaming == one-shot");
    eq(hs.digestSize, 32, "Hmac digestSize");
    eq(hs.verify("ab", HMAC("sha256", "k", "ab")), true, "verify accepts good tag");
    eq(hs.verify("ab", HMAC("sha256", "k", "xy")), false, "verify rejects wrong tag");
    await throws(() => new Hmac("no-such-alg", "k"), /./, "Hmac unknown algo throws");

    function bytesEq(a, b) { if (!a || !b || a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

    eq((await PBKDF2({ password: "p", salt: "s", iterations: 1000, length: 32 })).length, 32, "PBKDF2 length");
    assert(bytesEq(await PBKDF2({ password: "p", salt: "s", iterations: 1000, length: 32 }),
                   await PBKDF2({ password: "p", salt: "s", iterations: 1000, length: 32 })), "PBKDF2 deterministic");
    assert(!bytesEq(await PBKDF2({ password: "p", salt: "s1", iterations: 1000, length: 32 }),
                    await PBKDF2({ password: "p", salt: "s2", iterations: 1000, length: 32 })), "PBKDF2 salt-sensitive");
    eq(HKDF({ key: "secret", salt: "salt", info: "ctx", length: 32 }).length, 32, "HKDF length");
    assert(!bytesEq(HKDF({ key: "secret", salt: "salt", info: "ctx", length: 32 }),
                    HKDF({ key: "secret", salt: "other", info: "ctx", length: 32 })), "HKDF salt-sensitive");
    if (HAS_TLS) eq((await CryptoNS.Scrypt("p", "s", { N: 1024, r: 1, p: 1, keyLen: 16 })).length, 16, "Scrypt keyLen");
    else { n++; }

    const rb = RandomBytes(16);
    eq(rb.length, 16, "RandomBytes count");
    assert(RandomBytes(16)[3] !== undefined && rb.some((b) => b !== 0), "RandomBytes non-degenerate");
    eq(TimingSafeEqual(new Uint8Array([1, 2]), new Uint8Array([1, 2])), true, "TSE equal");
    eq(TimingSafeEqual(new Uint8Array([1]), new Uint8Array([2])), false, "TSE differ");
    eq(TimingSafeEqual(new Uint8Array([1]), new Uint8Array([1, 2])), false, "TSE length mismatch false not throw");

    const tok = JWTSign({ sub: "u", n: 1 }, "secret", { alg: "HS256" });
    eq(tok.split(".").length, 3, "JWT has 3 segments");
    const pl = JWTVerify(tok, "secret", { algorithms: ["HS256"] });
    eq(pl.sub, "u", "JWT payload round-trip");
    eq(pl.n, 1, "JWT numeric field survives");
    await throws(() => JWTVerify(tok, "wrong-secret", { algorithms: ["HS256"] }), /verify|signature/i, "JWT forged signature refused");
    await throws(() => JWTVerify(tok, "secret", { algorithms: ["none"] }), /allow/i, "JWT allowlist enforced (alg confusion closed)");

    {
        const key = new Uint8Array(32).fill(7);
        if (!HAS_TLS) { n++; } else {
        const g = new CryptoNS.AESGCM(key);
        const nonce = new Uint8Array(12).fill(1);
        const sealed = g.seal(nonce, "hello");
        eq(sealed.length, 5 + 16, "AESGCM output = pt + tag");
        eq(new TextDecoder().decode(g.open(nonce, sealed)), "hello", "AESGCM round-trip");
        const bad = sealed.slice(); bad[0] ^= 0xff;
        await throws(() => g.open(nonce, bad), /authentication failed/, "AESGCM tampered ciphertext refused");
        const s2 = g.seal(nonce, "x", new Uint8Array([1]));
        await throws(() => g.open(nonce, s2, new Uint8Array([2])), /authentication failed/, "AESGCM wrong AAD refused");
        await throws(() => g.seal(new Uint8Array(11), "x"), /12|nonce/i, "AESGCM nonce must be 12 bytes");
        g.close(); g.close();
        }
    }
    {
        const key = new Uint8Array(32).fill(9);
        if (!HAS_TLS) { n++; } else {
        const ch = new CryptoNS.ChaCha20Poly1305(key);
        const n2 = new Uint8Array(12).fill(9);
        const cs = ch.seal(n2, "chacha");
        eq(new TextDecoder().decode(ch.open(n2, cs)), "chacha", "ChaCha round-trip");
        const cbad = cs.slice(); cbad[cs.length - 1] ^= 0x80;
        await throws(() => ch.open(n2, cbad), /authentication failed/i, "ChaCha tampered tag refused");
        ch.close();
        }
    }

    {
        if (!HAS_TLS) { n++; print("note: crypto AEAD/KDF/password section skipped (no-TLS build)"); }
        if (HAS_TLS) {
        const bh = await CryptoNS.Bcrypt.hash("pass", 4);
        assert(bh.startsWith("$2b$"), "Bcrypt $2b$ prefix");
        eq(await CryptoNS.Bcrypt.verify("pass", bh), true, "Bcrypt verify ok");
        eq(await CryptoNS.Bcrypt.verify("nope", bh), false, "Bcrypt verify reject");
        const salt = new Uint8Array(16).fill(3);
        const aphc = await CryptoNS.Argon2id.hash("pw", salt, { iterations: 1, memory: 1024 });
        eq(typeof aphc, "string", "Argon2id default form is the PHC string");
        assert(aphc.startsWith("$argon2id$v=19$m=1024,t=1,"), "Argon2id PHC params in string");
        const ah = await CryptoNS.Argon2id.hash("pw", salt, { iterations: 1, memory: 1024, encoded: false });
        eq(ah.length, 32, "Argon2id default hashLen");
        eq(await CryptoNS.Argon2id.verify("pw", salt, ah, { iterations: 1, memory: 1024 }), true, "Argon2id verify ok");
        eq(await CryptoNS.Argon2id.verify("bad", salt, ah, { iterations: 1, memory: 1024 }), false, "Argon2id verify reject");
        assert(!await CryptoNS.Argon2id.verify("pw", salt, ah, { iterations: 2, memory: 1024 }), "Argon2id param drift detected");
        }
    }
});
__kitS.end();
if(fails){print("test_crypto_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_crypto_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
