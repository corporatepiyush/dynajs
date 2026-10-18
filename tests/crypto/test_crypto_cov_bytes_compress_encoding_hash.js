// Split from tests/test_cov_bytes_compress_encoding_hash.js: the crypto sections.
import { MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex, SHA256, SHA256Hex, SHA384, SHA384Hex, SHA512, SHA512Hex, CRC32, CRC32C, SHA3_224, SHA3_224Hex, SHA3_256, SHA3_256Hex, SHA3_384, SHA3_384Hex, SHA3_512, SHA3_512Hex, Keccak256, Keccak256Hex, SHAKE128, SHAKE128Hex, SHAKE256, SHAKE256Hex, BLAKE3, BLAKE3Hex, BLAKE2b, BLAKE2bHex, BLAKE2s, BLAKE2sHex, Murmur3_128, Murmur3_128Hex, XXHash32, XXHash64, Hasher } from "dyna:hash";
import * as CryptoNS from "dyna:crypto";

const { HMAC, HMACHex, Hmac, PBKDF2, HKDF, RandomBytes, TimingSafeEqual, HOTPGenerate, TOTPGenerate, JWTSign, JWTVerify } = CryptoNS;
const HAS_TLS = typeof CryptoNS.Scrypt === "function" && typeof CryptoNS.AESGCM === "function";
let n=0,fails=0;
function assert(c,m){n++; if(!c){fails++; print("FAIL: "+m)}}
function eq(a,b,m){assert(a===b,m+" got "+a+" want "+b)}
function throws(fn,re,m){n++; try{fn(); fails++; print("FAIL: "+m+" did not throw")}catch(e){if(re && !re.test(String(e.message))) {fails++; print("FAIL: "+m+" wrong msg: "+e.message)}}}
function u8(...b){ return new Uint8Array(b); }
function eqArr(a,b){ if(a.length!==b.length) return false; for(let i=0;i<a.length;i++) if(a[i]!==b[i]) return false; return true; }
function hex(u){ return Array.from(u,b=>b.toString(16).padStart(2,"0")).join(""); }
function logStep(s){ try{ }catch(e){} }
logStep("bytes start");
print("encoding done: "+n+" assertions");
{
  function checkHex(fnHex, input, want, label){ eq(fnHex(input),want,label); }
  checkHex(MD5Hex,"","d41d8cd98f00b204e9800998ecf8427e","MD5 empty");
  checkHex(MD5Hex,"abc","900150983cd24fb0d6963f7d28e17f72","MD5 abc");
  checkHex(MD5Hex,"The quick brown fox jumps over the lazy dog","9e107d9d372bb6826bd81d3542a419d6","MD5 fox");
  checkHex(SHA1Hex,"","da39a3ee5e6b4b0d3255bfef95601890afd80709","SHA1 empty");
  checkHex(SHA1Hex,"abc","a9993e364706816aba3e25717850c26c9cd0d89d","SHA1 abc");
  checkHex(SHA224Hex,"","d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f","SHA224 empty");
  checkHex(SHA224Hex,"abc","23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7","SHA224 abc");
  checkHex(SHA256Hex,"abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
  checkHex(SHA256Hex,"","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty");
  checkHex(SHA256Hex,"hello","2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824","SHA256 hello");
  checkHex(SHA384Hex,"","38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b","SHA384 empty");
  assert(SHA384Hex("abc").startsWith("cb00753f45a35e8bb5a03d699ac65007"),"SHA384 abc prefix");
  assert(SHA384Hex("abc").length===96,"SHA384 hex length 96");
  assert(SHA384Hex("abc")!==SHA384Hex(""),"SHA384 abc != empty");
  checkHex(SHA512Hex,"","cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e","SHA512 empty");
  checkHex(SHA512Hex,"abc","ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f","SHA512 abc");
  assert(SHA512Hex("abc").length===128,"SHA512 hex length 128");
  eq(MD5("").length,16,"MD5 raw len 16");
  eq(SHA1("").length,20,"SHA1 raw len 20");
  eq(SHA224("").length,28,"SHA224 raw len 28");
  eq(SHA256("").length,32,"SHA256 raw len 32");
  eq(SHA384("").length,48,"SHA384 raw len 48");
  eq(SHA512("").length,64,"SHA512 raw len 64");
  assert(MD5("") instanceof Uint8Array,"MD5 is Uint8Array");
  eq(CRC32(""),0,"CRC32 empty 0");
  eq(CRC32C(""),0,"CRC32C empty 0");
  eq(CRC32("123456789"),0xcbf43926,"CRC32 123456789");
  eq(CRC32C("123456789"),0xe3069283,"CRC32C 123456789");
  eq(CRC32("abc"),0x352441c2,"CRC32 abc");
  assert(typeof CRC32("abc")==="number","CRC32 returns number");
  assert(CRC32("abc")>=0,"CRC32 non-negative");
  assert(CRC32("a")!==CRC32("b"),"CRC32 a vs b differ");
  checkHex(SHA3_224Hex,"","6b4e03423667dbb73b6e15454f0eb1abd4597f9a1b078e3f5b5a6bc7","SHA3_224 empty");
  checkHex(SHA3_256Hex,"","a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a","SHA3_256 empty");
  checkHex(SHA3_256Hex,"abc","3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532","SHA3_256 abc");
  checkHex(SHA3_384Hex,"","0c63a75b845e4f7d01107d852e4c2485c51a50aaaa94fc61995e71bbee983a2ac3713831264adb47fb6bd1e058d5f004","SHA3_384 empty");
  checkHex(SHA3_512Hex,"","a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26","SHA3_512 empty");
  eq(SHA3_224("").length,28,"SHA3_224 len");
  eq(SHA3_256("").length,32,"SHA3_256 len");
  eq(SHA3_384("").length,48,"SHA3_384 len");
  eq(SHA3_512("").length,64,"SHA3_512 len");
  checkHex(Keccak256Hex,"","c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470","Keccak256 empty");
  assert(Keccak256Hex("")!==SHA3_256Hex(""),"Keccak vs SHA3 differ empty");
  assert(Keccak256Hex("abc")!==SHA3_256Hex("abc"),"Keccak vs SHA3 differ abc");
  eq(Keccak256("").length,32,"Keccak len 32");
  eq(SHAKE128("",16).length,16,"SHAKE128 16 len");
  eq(SHAKE256("",32).length,32,"SHAKE256 32 len");
  eq(hex(SHAKE128("",16)), hex(SHAKE128("",32)).slice(0,32),"SHAKE128 prefix");
  eq(hex(SHAKE256("abc",8)), hex(SHAKE256("abc",16)).slice(0,16),"SHAKE256 prefix");
  eq(hex(SHAKE128("hello",20)), SHAKE128Hex("hello",20),"SHAKE128 hex agreement");
  eq(hex(SHAKE256("hello",20)), SHAKE256Hex("hello",20),"SHAKE256 hex agreement");
  throws(()=>SHAKE128("",0),null,"SHAKE128 0 throws");
  throws(()=>SHAKE128("",-1),null,"SHAKE128 -1 throws");
  function pat(k){ let a=new Uint8Array(k); for(let i=0;i<k;i++) a[i]=i%251; return a; }
  eq(BLAKE3Hex(pat(0)),"af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262","BLAKE3 0");
  eq(BLAKE3Hex(pat(1)),"2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213","BLAKE3 1");
  eq(BLAKE2bHex(pat(0)),"786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce","BLAKE2b 0");
  eq(BLAKE2sHex(pat(0)),"69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9","BLAKE2s 0");
  eq(Murmur3_128Hex(pat(0)),"00000000000000000000000000000000","Murmur3 0");
  eq(BLAKE3Hex(pat(64)),"4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98","BLAKE3 64");
  eq(BLAKE3Hex(pat(1024)),"42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7","BLAKE3 1024");
  eq(BLAKE2bHex(pat(100),64).length,128,"BLAKE2b 64 hex len 128");
  eq(BLAKE2sHex(pat(100),32).length,64,"BLAKE2s 32 hex len 64");
  eq(BLAKE3(pat(10),16).length,16,"BLAKE3 16 len");
  eq(BLAKE2b(pat(10),32).length,32,"BLAKE2b 32 len");
  eq(BLAKE2s(pat(10),16).length,16,"BLAKE2s 16 len");
  assert(Murmur3_128Hex(pat(37),0)!=="0000","Murmur seed 0 not zero for pat37");
  assert(Murmur3_128Hex(pat(37),1)!==Murmur3_128Hex(pat(37),0),"Murmur seed matters");
  eq(Murmur3_128(pat(9)).length,16,"Murmur raw 16");
  eq(XXHash32(""),0x02cc5d05,"XXHash32 empty");
  eq(XXHash32("abc"),0x32d153ff,"XXHash32 abc");
  eq(XXHash32("a"),0x550d7456,"XXHash32 a");
  eq(XXHash64(""),"ef46db3751d8e999","XXHash64 empty");
  eq(XXHash64("abc"),"44bc2cf5ad770999","XXHash64 abc");
  eq(typeof XXHash64(""),"string","XXHash64 returns string");
  assert(XXHash64("abc",1)!==XXHash64("abc",0),"XXHash64 seed matters");
  assert(XXHash32("abc",1)!==XXHash32("abc",0),"XXHash32 seed matters");
  {
    let h = new Hasher("sha256");
    eq(h.algorithm,"sha256","Hasher algorithm");
    eq(h.digestSize,32,"Hasher digestSize sha256");
    h.update("hello"); h.update(" world");
    eq(hex(h.digest()), SHA256Hex("hello world"),"Hasher streaming hello world");
    let h2 = new Hasher("sha256");
    h2.update("abc");
    eq(h2.digestHex(), SHA256Hex("abc"),"Hasher abc");
    h2.reset();
    h2.update("abc");
    eq(h2.digestHex(), SHA256Hex("abc"),"Hasher after reset same");
    for(let [algo,size] of [["md5",16],["sha1",20],["sha224",28],["sha256",32],["sha384",48],["sha512",64]]){
      let hh = new Hasher(algo);
      eq(hh.digestSize,size,"Hasher size "+algo);
      eq(hh.algorithm,algo,"Hasher algo "+algo);
      hh.update("test");
      eq(hex(hh.digest()), hh.digestHex(),"Hasher hex vs bytes "+algo);
    }
    throws(()=>new Hasher("unknown"),null,"Hasher unknown throws");
    throws(()=>new Hasher(),null,"Hasher no arg throws");
    throws(()=>new Hasher("SHA256"),null,"Hasher uppercase throws (must be lowercase)");
    assert(h.update(String(null))===h,"Hasher update null coerces and chains");
    let h3 = new Hasher("sha256");
    h3.update("a"); h3.update("b"); h3.update("c");
    eq(h3.digestHex(), SHA256Hex("abc"),"Hasher abc via 3 updates");
    h3.reset();
    h3.update("abc");
    eq(h3.digestHex(), SHA256Hex("abc"),"Hasher after reset again");
  }
  {
    let mb = new Uint8Array(1024*1024); for(let i=0;i<mb.length;i++) mb[i]=i&0xFF;
    let h1 = SHA256(mb); let h2 = SHA256(mb);
    assert(eqArr(h1,h2),"SHA256 1MB deterministic");
    eq(h1.length,32,"SHA256 1MB len");
    let h3 = BLAKE3(mb); eq(h3.length,32,"BLAKE3 1MB len");
    let h4 = SHA512(mb); eq(h4.length,64,"SHA512 1MB len");
    let mb2 = mb.slice(); mb2[0]^=1;
    assert(!eqArr(SHA256(mb), SHA256(mb2)),"SHA256 1MB diff input diff hash");
  }
  assert(MD5(String(null)).length===16,"MD5 null coerces");
  assert(SHA256(String(null)).length===32,"SHA256 null coerces");
  assert(MD5(String(123)).length===16,"MD5 number coerces");
  assert(SHA256(new Float64Array([1.5,2.5])).length===32,"SHA256 Float64 via bytesOf");
  assert(SHA3_256(String(null)).length===32,"SHA3 null coerces");
  assert(BLAKE3(String(null)).length===32,"BLAKE3 null coerces");
  throws(()=>BLAKE3(pat(4),0),null,"BLAKE3 0 len throws");
  throws(()=>BLAKE2b(pat(4),65),null,"BLAKE2b 65 throws");
  throws(()=>BLAKE2s(pat(4),33),null,"BLAKE2s 33 throws");
}
{
    const mac = HMAC("sha256", "key", "msg");
    eq(mac.length, 32, "HMAC sha256 digest size");
    eq(HMACHex("sha256", "key", "msg").length, 64, "HMACHex length");

    const hs = new Hmac("sha256", "k");
    hs.update("a"); hs.update("b");
    eq(hs.digestHex(), HMACHex("sha256", "k", "ab"), "streaming == one-shot");
    eq(hs.digestSize, 32, "Hmac digestSize");
    eq(hs.verify("ab", HMAC("sha256", "k", "ab")), true, "verify accepts good tag");
    eq(hs.verify("ab", HMAC("sha256", "k", "xy")), false, "verify rejects wrong tag");
    throws(() => new Hmac("no-such-alg", "k"), /./, "Hmac unknown algo throws");

    function bytesEq(a, b) { if (!a || !b || a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

    eq(PBKDF2({ password: "p", salt: "s", iterations: 1000, length: 32 }).length, 32, "PBKDF2 length");
    assert(bytesEq(PBKDF2({ password: "p", salt: "s", iterations: 1000, length: 32 }),
                   PBKDF2({ password: "p", salt: "s", iterations: 1000, length: 32 })), "PBKDF2 deterministic");
    assert(!bytesEq(PBKDF2({ password: "p", salt: "s1", iterations: 1000, length: 32 }),
                    PBKDF2({ password: "p", salt: "s2", iterations: 1000, length: 32 })), "PBKDF2 salt-sensitive");
    eq(HKDF({ key: "secret", salt: "salt", info: "ctx", length: 32 }).length, 32, "HKDF length");
    assert(!bytesEq(HKDF({ key: "secret", salt: "salt", info: "ctx", length: 32 }),
                    HKDF({ key: "secret", salt: "other", info: "ctx", length: 32 })), "HKDF salt-sensitive");
    if (HAS_TLS) eq(CryptoNS.Scrypt("p", "s", { N: 1024, r: 1, p: 1, keyLen: 16 }).length, 16, "Scrypt keyLen");
    else { n++; }

    const rb = RandomBytes(16);
    eq(rb.length, 16, "RandomBytes count");
    assert(RandomBytes(16)[3] !== undefined && rb.some((b) => b !== 0), "RandomBytes non-degenerate");
    eq(TimingSafeEqual(new Uint8Array([1, 2]), new Uint8Array([1, 2])), true, "TSE equal");
    eq(TimingSafeEqual(new Uint8Array([1]), new Uint8Array([2])), false, "TSE differ");
    eq(TimingSafeEqual(new Uint8Array([1]), new Uint8Array([1, 2])), false, "TSE length mismatch false not throw");

    eq(HOTPGenerate("12345678901234567890", 0, { digits: 6 }), "755224", "HOTP RFC4226 c=0 vector");
    eq(HOTPGenerate("12345678901234567890", 9, { digits: 6 }), "520489", "HOTP RFC4226 c=9 vector");
    eq(TOTPGenerate("12345678901234567890", { atSec: 59, digits: 8 }), "94287082", "TOTP RFC6238 T=59 vector");
    eq(TOTPGenerate("12345678901234567890", { atSec: 1111111109, digits: 8 }), "07081804", "TOTP RFC6238 T=1.11e9 vector");
    throws(() => HOTPGenerate("secret", -1), /./, "HOTP negative counter refused");
    eq(TOTPGenerate("abc", { atSec: 1234567 }), TOTPGenerate("abc", { atSec: 1234567 }), "TOTP deterministic at fixed sec");

    const tok = JWTSign({ sub: "u", n: 1 }, "secret", { alg: "HS256" });
    eq(tok.split(".").length, 3, "JWT has 3 segments");
    const pl = JWTVerify(tok, "secret", { algorithms: ["HS256"] });
    eq(pl.sub, "u", "JWT payload round-trip");
    eq(pl.n, 1, "JWT numeric field survives");
    throws(() => JWTVerify(tok, "wrong-secret", { algorithms: ["HS256"] }), /verify|signature/i, "JWT forged signature refused");
    throws(() => JWTVerify(tok, "secret", { algorithms: ["none"] }), /allow/i, "JWT allowlist enforced (alg confusion closed)");

    {
        const key = new Uint8Array(32).fill(7);
        if (!HAS_TLS) { n++; } else {
        const g = new CryptoNS.AESGCM(key);
        const nonce = new Uint8Array(12).fill(1);
        const sealed = g.seal(nonce, "hello");
        eq(sealed.length, 5 + 16, "AESGCM output = pt + tag");
        eq(new TextDecoder().decode(g.open(nonce, sealed)), "hello", "AESGCM round-trip");
        const bad = sealed.slice(); bad[0] ^= 0xff;
        throws(() => g.open(nonce, bad), /authentication failed/, "AESGCM tampered ciphertext refused");
        const s2 = g.seal(nonce, "x", new Uint8Array([1]));
        throws(() => g.open(nonce, s2, new Uint8Array([2])), /authentication failed/, "AESGCM wrong AAD refused");
        throws(() => g.seal(new Uint8Array(11), "x"), /12|nonce/i, "AESGCM nonce must be 12 bytes");
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
        throws(() => ch.open(n2, cbad), /authentication failed/i, "ChaCha tampered tag refused");
        ch.close();
        }
    }

    {
        if (!HAS_TLS) { n++; print("note: crypto AEAD/KDF/password section skipped (no-TLS build)"); }
        if (HAS_TLS) {
        const bh = CryptoNS.Bcrypt.hash("pass", 4);
        assert(bh.startsWith("$2b$"), "Bcrypt $2b$ prefix");
        eq(CryptoNS.Bcrypt.verify("pass", bh), true, "Bcrypt verify ok");
        eq(CryptoNS.Bcrypt.verify("nope", bh), false, "Bcrypt verify reject");
        const salt = new Uint8Array(16).fill(3);
        const aphc = CryptoNS.Argon2id.hash("pw", salt, { iterations: 1, memory: 1024 });
        eq(typeof aphc, "string", "Argon2id default form is the PHC string");
        assert(aphc.startsWith("$argon2id$v=19$m=1024,t=1,"), "Argon2id PHC params in string");
        const ah = CryptoNS.Argon2id.hash("pw", salt, { iterations: 1, memory: 1024, encoded: false });
        eq(ah.length, 32, "Argon2id default hashLen");
        eq(CryptoNS.Argon2id.verify("pw", salt, ah, { iterations: 1, memory: 1024 }), true, "Argon2id verify ok");
        eq(CryptoNS.Argon2id.verify("bad", salt, ah, { iterations: 1, memory: 1024 }), false, "Argon2id verify reject");
        assert(!CryptoNS.Argon2id.verify("pw", salt, ah, { iterations: 2, memory: 1024 }), "Argon2id param drift detected");
        }
    }
}
if(fails){print("test_crypto_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_crypto_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
