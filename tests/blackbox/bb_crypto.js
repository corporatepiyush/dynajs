// Black-box contract test for dyna:crypto, generated from dynajs.d.ts lines 726-1073 (plus shared types lines 1-124). Engine sources not consulted.
// CAPABILITY GATE: the d.ts module head notes that the Ed25519*/X25519*/Scrypt functions, the
// AESGCM/ChaCha20Poly1305 classes and the RSA/ECDSA/ECDH/X509 namespaces exist only in CONFIG_TLS=y
// builds — static named imports of them fail to link on a no-TLS build. So the gated names are
// pulled from a namespace import after one probe, and ONLY the gated tables are skipped (loudly)
// when the probe says absent; every unconditional table still runs and must pass.
// PARAMETRIC: every exported surface is driven as a CASE TABLE through one loop per table; rows are
// [label, args, expected] (with the table's natural operation) or [label, thunk, expected] (behavior rows),
// and every failure message names its row. Expected values come from the dynajs.d.ts contract text plus
// fixed official vectors, cited per row/table: FIPS 180-4/202, RFC 1321, RFC 2202, RFC 4231, RFC 5869,
// RFC 7914, PBKDF2-HMAC-SHA256 published vector set, RFC 4226 App. D, RFC 6238 App. B, RFC 7748 s6.1,
// RFC 8032 s7.1, RFC 8439 s2.8.2, McGrew-Viega GCM spec TC3/13/14, xxHash spec 0.8, BLAKE2 RFC 7693,
// BLAKE3 spec, crypt_blowfish bcrypt vector (OpenWall), Ethereum empty keccak-256.
import {
    Bcrypt, Argon2id,
    Hmac, HMAC, HMACHex, HKDF, PBKDF2,
    RandomBytes, TimingSafeEqual,
    HOTPGenerate, TOTPGenerate, HOTPVerify, TOTPVerify,
    JWTSign, JWTVerify,
    MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex, SHA256, SHA256Hex,
    SHA384, SHA384Hex, SHA512, SHA512Hex,
    CRC32, CRC32C, XXHash32, XXHash64, XXH3_64,
    SHA3_256, SHA3_256Hex, SHA3_512, Keccak256, Keccak256Hex,
    SHAKE128, SHAKE128Hex, SHAKE256,
    BLAKE3, BLAKE3Hex, BLAKE2b, BLAKE2bHex, BLAKE2s, BLAKE2sHex,
    Murmur3_128, Murmur3_128Hex,
} from "dyna:crypto";

// ---- CONFIG_TLS capability probe -----------------------------------------
// The gated family rides one engine flag, so probing one representative name
// (RSA) is exact. Namespace property access never throws; absent exports read
// as undefined, which is the documented no-TLS behavior (d.ts module head).
const gated = await import("dyna:crypto");
const HAS_TLS = typeof gated.RSA !== "undefined";
const RSA = gated.RSA, ECDSA = gated.ECDSA, ECDH = gated.ECDH, X509 = gated.X509,
    AESGCM = gated.AESGCM, ChaCha20Poly1305 = gated.ChaCha20Poly1305,
    Scrypt = gated.Scrypt,
    Ed25519Generate = gated.Ed25519Generate, Ed25519Sign = gated.Ed25519Sign,
    Ed25519Verify = gated.Ed25519Verify, Ed25519PemToRaw = gated.Ed25519PemToRaw,
    Ed25519PemFromRaw = gated.Ed25519PemFromRaw,
    X25519Generate = gated.X25519Generate, X25519Derive = gated.X25519Derive,
    X25519PemToRaw = gated.X25519PemToRaw, X25519PemFromRaw = gated.X25519PemFromRaw;
if (!HAS_TLS)
    print("bb_crypto: SKIP(RSA: this build has no CONFIG_TLS)");

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true, e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
// ---- table drivers --------------------------------------------------------
const THROWS = (t, p) => ({ __throws: true, t, p }); // expected-error marker for a row
// Arg table: row = [label, argsArray, expected]; fn is the table's one natural operation.
function runs(table, fn) {
    for (const [label, args, expected] of table) {
        const call = fn ? () => fn(...args) : args; // without fn, args IS the row thunk
        if (expected && expected.__throws) assertThrows(call, label, expected.t, expected.p);
        else if (typeof expected === "function") { n++; let v; try { v = call(); } catch (e) { throw new Error("assertion failed (threw " + e + "): " + label); } if (!expected(v)) throw new Error("assertion failed (predicate): " + label + " — got |" + v + "|"); }
        else assertEq(call(), expected, label);
    }
}
// ---- local helpers --------------------------------------------------------
function u8(...bytes) { return new Uint8Array(bytes); }
function byHex(hex) { const out = new Uint8Array(hex.length / 2); for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16); return out; }
function hexOf(a) { let s = ""; for (let i = 0; i < a.length; i++) s += (a[i] & 0xff).toString(16).padStart(2, "0"); return s; }
function utf8(s) { return new TextEncoder().encode(s); }
function b64url(bytes) { let s = ""; for (const b of bytes) s += String.fromCharCode(b); return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); }
function b64uToHex(s) { let b64 = s.replace(/-/g, "+").replace(/_/g, "/"); while (b64.length % 4) b64 += "="; let bin = ""; try { bin = atob(b64); } catch (e) { return ""; } let hex = ""; for (const ch of bin) hex += ch.charCodeAt(0).toString(16).padStart(2, "0"); return hex; }

/* ==========================================================================
 * TABLE 1 — unkeyed digest vectors (d.ts lines 962-1000).
 * ========================================================================== */
const FN = { MD5, SHA1, SHA224, SHA256, SHA384, SHA512, SHA3_256, SHA3_512, Keccak256 };
runs([
    ["MD5('') — RFC 1321", ["MD5", ""], "d41d8cd98f00b204e9800998ecf8427e"],
    ["MD5('abc') — RFC 1321", ["MD5", "abc"], "900150983cd24fb0d6963f7d28e17f72"],
    ["SHA1('') — FIPS 180-4", ["SHA1", ""], "da39a3ee5e6b4b0d3255bfef95601890afd80709"],
    ["SHA1('abc') — FIPS 180-4", ["SHA1", "abc"], "a9993e364706816aba3e25717850c26c9cd0d89d"],
    ["SHA224('') — FIPS 180-4", ["SHA224", ""], "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f"],
    ["SHA224('abc') — FIPS 180-4", ["SHA224", "abc"], "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"],
    ["SHA256('') — FIPS 180-4", ["SHA256", ""], "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"],
    ["SHA256('abc') — FIPS 180-4", ["SHA256", "abc"], "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"],
    ["SHA384('') — FIPS 180-4", ["SHA384", ""], "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b"],
    ["SHA384('abc') — FIPS 180-4", ["SHA384", "abc"], "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"],
    ["SHA512('') — FIPS 180-4", ["SHA512", ""], "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"],
    ["SHA512('abc') — FIPS 180-4", ["SHA512", "abc"], "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"],
    ["SHA3-256('abc') — FIPS 202", ["SHA3_256", "abc"], "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"],
    ["SHA3-512('') — FIPS 202", ["SHA3_512", ""], "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26"],
    ["Keccak256('') — Ethereum empty-hash constant", ["Keccak256", ""], "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"],
], (name, data) => hexOf(FN[name](data)));

/* ==========================================================================
 * TABLE 2 — digest sizes and XOF/variable length options (d.ts lines 962-1020).
 * ========================================================================== */
runs([
    ["MD5 size 16", [() => MD5("x").length], 16],
    ["SHA1 size 20", [() => SHA1("x").length], 20],
    ["SHA224 size 28", [() => SHA224("x").length], 28],
    ["SHA256 size 32", [() => SHA256("x").length], 32],
    ["SHA384 size 48", [() => SHA384("x").length], 48],
    ["SHA512 size 64", [() => SHA512("x").length], 64],
    ["SHA3_256 size 32", [() => SHA3_256("x").length], 32],
    ["Keccak256 size 32", [() => Keccak256("x").length], 32],
    ["Murmur3_128 size 16 — d.ts line 1019", [() => Murmur3_128("x").length], 16],
    ["SHAKE128 length option honored", [() => SHAKE128("abc", 32).length], 32],
    ["SHAKE256 length option honored", [() => SHAKE256("abc", 64).length], 64],
    ["SHAKE128 stream prefix property (XOF)", [() => hexOf(SHAKE128("abc", 32)).startsWith(hexOf(SHAKE128("abc", 16)))], true],
    ["SHAKE256 stream prefix property (XOF)", [() => hexOf(SHAKE256("abc", 64)).startsWith(hexOf(SHAKE256("abc", 32)))], true],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 3 — Hex door parity rows: HexFn(x) == hexOf(BytesFn(x)).
 * ========================================================================== */
runs([
    ["MD5Hex parity", [() => MD5Hex("abc"), () => MD5("abc")], true],
    ["SHA1Hex parity", [() => SHA1Hex("abc"), () => SHA1("abc")], true],
    ["SHA224Hex parity", [() => SHA224Hex("abc"), () => SHA224("abc")], true],
    ["SHA256Hex parity", [() => SHA256Hex("abc"), () => SHA256("abc")], true],
    ["SHA384Hex parity", [() => SHA384Hex("abc"), () => SHA384("abc")], true],
    ["SHA512Hex parity", [() => SHA512Hex("abc"), () => SHA512("abc")], true],
    ["SHA3_256Hex parity", [() => SHA3_256Hex("abc"), () => SHA3_256("abc")], true],
    ["Keccak256Hex parity", [() => Keccak256Hex("abc"), () => Keccak256("abc")], true],
    ["SHAKE128Hex parity", [() => SHAKE128Hex("abc", 8), () => SHAKE128("abc", 8)], true],
    ["BLAKE2bHex parity", [() => BLAKE2bHex("abc"), () => BLAKE2b("abc")], true],
    ["BLAKE2sHex parity", [() => BLAKE2sHex("abc"), () => BLAKE2s("abc")], true],
    ["BLAKE3Hex parity", [() => BLAKE3Hex(""), () => BLAKE3("")], true],
    ["Murmur3_128Hex parity", [() => Murmur3_128Hex("x"), () => Murmur3_128("x")], true],
], (hexDoor, byteDoor) => hexDoor() === hexOf(byteDoor())); // runs() spreads args, so the two thunks arrive as two parameters (not an array)

// BytesInput contract: strings are read as UTF-8 (d.ts lines 73-74).
runs([
    ["SHA256 string input is UTF-8 (é == c3 a9)", [SHA256, "\u00e9", u8(0xc3, 0xa9)], true],
    ["SHA1 string input is UTF-8 (é == c3 a9)", [SHA1, "\u00e9", u8(0xc3, 0xa9)], true],
], (f, s, bytes) => eqArr(f(s), f(bytes)));

/* ==========================================================================
 * TABLE 4 — BLAKE keyed/derive contract (d.ts lines 1001-1018).
 * Vectors: RFC 7693 ('abc' for b/s), BLAKE3 spec ('').
 * ========================================================================== */
runs([
    ["BLAKE2b-512('abc') — RFC 7693", [() => hexOf(BLAKE2b("abc"))], "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"],
    ["BLAKE2s-256('abc') — RFC 7693", [() => hexOf(BLAKE2s("abc"))], "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"],
    ["BLAKE3('') — BLAKE3 spec", [() => hexOf(BLAKE3(""))], "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"],
    ["BLAKE3 output is a stream: longer request extends the 32-byte default", [() => hexOf(BLAKE3("abc", 64)).startsWith(hexOf(BLAKE3("abc")))], true],
    ["BLAKE2b EMPTY key is legal == unkeyed (RFC 7693, d.ts 1001-1006)", [() => BLAKE2bHex("abc", { key: "" }) === hexOf(BLAKE2b("abc"))], true],
    ["BLAKE2b keyed differs from unkeyed", [() => hexOf(BLAKE2b("abc", { key: "k" })) !== hexOf(BLAKE2b("abc"))], true],
    ["BLAKE3 derive-key mode yields exactly 32 bytes", [() => BLAKE3("data", { deriveKey: "context" }).length], 32],
    ["BLAKE3 different deriveKey contexts differ", [() => hexOf(BLAKE3("d", { deriveKey: "a" })) !== hexOf(BLAKE3("d", { deriveKey: "b" }))], true],
    ["BLAKE3 length is refused in derive-key mode (d.ts 1004-1005)", [() => BLAKE3("data", { deriveKey: "c", length: 64 })], THROWS()],
    ["BLAKE3 keyed mode differs from unkeyed", [() => hexOf(BLAKE3("d", { key: new Uint8Array(32).fill(7) })) !== hexOf(BLAKE3("d"))], true],
    ["Murmur3_128 seed changes the digest", [() => hexOf(Murmur3_128("a")) !== hexOf(Murmur3_128("a", 1))], true],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 5 — numeric checksums. CRC-32/CRC-32C check values ("123456789"),
 * xxHash spec 0.8 empty-input values.
 * ========================================================================== */
runs([
    ["CRC32('') == 0", [CRC32, ""], 0],
    ["CRC32 check value 0xCBF43926", [CRC32, "123456789"], 0xcbf43926 >>> 0],
    ["CRC32 famous phrase 0x414FA339", [CRC32, "The quick brown fox jumps over the lazy dog"], 0x414fa339 >>> 0],
    ["CRC32C('') == 0", [CRC32C, ""], 0],
    ["CRC32C (iSCSI) check value 0xE3069283", [CRC32C, "123456789"], 0xe3069283 >>> 0],
    ["XXH32('', seed 0) spec 0x02CC5D05", [XXHash32, "", 0], 0x02cc5d05 >>> 0],
    ["XXH64('', 0) spec 0xEF46DB3751D8E999 (as:'bigint')", [XXHash64, "", 0, { as: "bigint" }], 0xef46db3751d8e999n],
    ["XXH3-64('', 0) spec 0x2D06800538D394C2 (as:'bigint')", [XXH3_64, "", 0, { as: "bigint" }], 0x2d06800538d394c2n],
], (f, ...a) => f(...a));
runs([
    ["XXHash64 as:'hex' returns a string", [() => XXHash64("", 0, { as: "hex" })], (v) => typeof v === "string"],
    ["XXHash64 as:'bytes' returns bytes (byte order undocumented)", [() => XXHash64("", 0, { as: "bytes" })], (v) => v instanceof Uint8Array],
    ["XXHash32 seed changes the digest", [() => XXHash32("", 0) !== XXHash32("", 1)], true],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 6 — HMAC one-shot vectors (RFC 4231 TC1/TC2; RFC 2202 TC1 for MD5/SHA1)
 * + parity and refusal rows.
 * ========================================================================== */
const TC1KEY = byHex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"); // RFC 4231 TC1 key: 0x0b x20
runs([
    ["HMAC-SHA256 RFC 4231 TC1 (binary key 0x0b x20)", ["sha256", TC1KEY, "Hi There"], "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"],
    ["HMAC-SHA256 RFC 4231 TC2 (string key 'Jefe')", ["sha256", "Jefe", "what do ya want for nothing?"], "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"],
    ["HMAC-SHA1 RFC 2202 TC1", ["sha1", TC1KEY, "Hi There"], "b617318655057264e28bc0b6fb378c8ef146be00"],
    ["HMAC-MD5 RFC 2202 TC1 (key 0x0b x16)", ["md5", byHex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"), "Hi There"], "9294727a3638bb1c13f48ef8158bfc9d"],
    ["HMACHex parity with HMAC (RFC 4231 TC2)", ["sha256", "Jefe", "what do ya want for nothing?"], "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"],
], (algo, key, data) => algo === "sha256" && typeof key === "string" ? HMACHex(algo, key, data) : hexOf(HMAC(algo, key, data)));
runs([
    ["HMAC unknown algorithm refused", [() => HMAC("nope", "k", "d")], THROWS(TypeError)],
    ["different keys -> different tags", [() => hexOf(HMAC("sha256", "key1", "d")) !== hexOf(HMAC("sha256", "key2", "d"))], true],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 7 — streaming Hmac class (d.ts lines 934-956): one instance, row per behavior.
 * ========================================================================== */
{
    const h = new Hmac("sha256", "key");
    const oneShot = HMACHex("sha256", "key", "data");
    const rawTag = h.sign("data");
    runs([
        ["algorithm property", [() => h.algorithm], "sha256"],
        ["digestSize property (sha256 -> 32)", [() => h.digestSize], 32],
        ["update returns this (d.ts line 940)", [() => h.update("")], h],
        ["streaming update/update/digestHex == one-shot", [() => { h.reset(); return h.update("d").update("ata").digestHex(); }], oneShot],
        ["sign() is a complete MAC and resets for the next message", [() => hexOf(h.sign("data"))], oneShot],
        ["verify accepts the hex tag", [() => h.verify("data", oneShot)], true],
        ["verify accepts the raw tag", [() => h.verify("data", rawTag)], true],
        ["verify rejects a tampered message", [() => h.verify("data!", oneShot)], false],
        ["verify rejects a wrong tag of the right length", [() => h.verify("data", oneShot.slice(0, 63) + (oneShot[63] === "0" ? "1" : "0"))], false], // last-char flip must actually differ (real tag ends '0', so flip to '1')
        ["reset() discards the update() prefix, key schedule kept (d.ts 945-947)", [() => { h.update("abandoned"); h.reset(); return h.update("data").digestHex(); }], oneShot],
        ["unknown algorithm refused", [() => new Hmac("sha999", "key")], THROWS(TypeError)],
        ["close() then closed", [() => { h.close(); return h.closed; }], true],
        ["use after close throws", [() => h.sign("x")], THROWS()],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 8 — HKDF (RFC 5869 TC1/TC3; STRICT bag, d.ts lines 1022-1025).
 * ========================================================================== */
const HKDF_IKM = byHex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"); // RFC 5869 TC1/TC3 IKM: 0x0b x22
runs([
    ["HKDF RFC 5869 TC1 (SHA-256, salt+info, L=42)", [() => hexOf(HKDF({ hash: "sha256", key: HKDF_IKM, salt: byHex("000102030405060708090a0b0c"), info: byHex("f0f1f2f3f4f5f6f7f8f9"), length: 42 }))],
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"],
    ["HKDF RFC 5869 TC3 (zero-length salt/info = defaults)", [() => hexOf(HKDF({ key: HKDF_IKM, length: 42 }))],
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"],
    ["HKDF length option honored", [() => HKDF({ key: "ikm", length: 64 }).length], 64],
    ["HKDF default length is 32", [() => HKDF({ key: "ikm" }).length], 32],
    ["HKDF STRICT bag refuses unknown keys", [() => HKDF({ key: "ikm", bogus: 1 })], THROWS(TypeError, "bogus")],
    ["HKDF requires {key}", [() => HKDF({ ikm: "x" })], THROWS()],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 9 — PBKDF2 (RFC 8018). Vectors: the published PBKDF2-HMAC-SHA256
 * password/salt series and PBKDF2-HMAC-SHA1 c=4096 dkLen=20. STRICT bag.
 * ========================================================================== */
const pb = (over) => ({ hash: "sha256", password: "password", salt: "salt", iterations: 1, length: 32, ...over });
runs([
    ["PBKDF2-SHA256 c=1", [() => hexOf(PBKDF2(pb({})))], "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"],
    ["PBKDF2-SHA256 c=2", [() => hexOf(PBKDF2(pb({ iterations: 2 })))], "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"],
    ["PBKDF2-SHA256 c=4096", [() => hexOf(PBKDF2(pb({ iterations: 4096 })))], "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"],
    ["PBKDF2-SHA256 c=4096 dkLen=40 (multi-block)", [() => hexOf(PBKDF2(pb({ password: "passwordPASSWORDpassword", salt: "saltSALTsaltSALTsaltSALTsaltSALTsalt", iterations: 4096, length: 40 })))],
        "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"],
    ["PBKDF2-SHA1 c=4096 dkLen=20", [() => hexOf(PBKDF2(pb({ hash: "sha1", iterations: 4096, length: 20 })))], "4b007901b765489abead49d926f721d065a429c1"],
    ["PBKDF2 STRICT bag refuses unknown keys", [() => PBKDF2(pb({ bogus: 1 }))], THROWS(TypeError, "bogus")],
    ["PBKDF2 iterations below range refused", [() => PBKDF2(pb({ iterations: 0 }))], THROWS()],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 10 — Scrypt (RFC 7914 s12 vector; STRICT bag, d.ts lines 1032-1035).
 * CONFIG_TLS-gated: skipped loudly when the probe says the name is absent.
 * ========================================================================== */
if (HAS_TLS) {
    runs([
        ["Scrypt RFC 7914 s12 vector (N=1024 r=8 p=16 dkLen=64)", [() => hexOf(Scrypt("password", "NaCl", { N: 1024, r: 8, p: 16, keyLen: 64 }))],
            "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640"],
        ["Scrypt deterministic at small N", [() => eqArr(Scrypt("pw", "salt", { N: 16, r: 1, p: 1, keyLen: 32 }), Scrypt("pw", "salt", { N: 16, r: 1, p: 1, keyLen: 32 }))], true],
        ["Scrypt keyLen honored", [() => Scrypt("pw", "salt", { N: 16, r: 1, p: 1, keyLen: 64 }).length], 64],
        ["Scrypt N must be a power of 2", [() => Scrypt("pw", "salt", { N: 100 })], THROWS()],
        ["Scrypt STRICT bag refuses unknown keys", [() => Scrypt("pw", "salt", { bogus: 1 })], THROWS(TypeError, "bogus")],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 11 — Bcrypt (d.ts lines 745-751). One cost-4 hash feeds the verify rows;
 * the $2a$/$2y$ rows use the published crypt_blowfish 'U*U' vector (OpenWall).
 * ========================================================================== */
{
    const bh = Bcrypt.hash("secret password", 4);
    const uv = "CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW";
    runs([
        ["$2b$04$ format: cost + 53 base64 chars", [() => /^\$2b\$04\$[A-Za-z0-9./]{53}$/.test(bh)], true],
        ["verify accepts its own hash", [() => Bcrypt.verify("secret password", bh)], true],
        ["verify rejects the wrong password", [() => Bcrypt.verify("wrong password", bh)], false],
        ["fresh salt: two hashes of one password differ", [() => bh !== Bcrypt.hash("secret password", 4)], true],
        ["both fresh hashes verify", [() => Bcrypt.verify("secret password", Bcrypt.hash("secret password", 4))], true],
        ["default rounds = 10 rides in the hash", [() => /^\$2b\$10\$/.test(Bcrypt.hash("rounds default"))], true],
        ["$2a$ prefix verifies (crypt_blowfish 'U*U')", [() => Bcrypt.verify("U*U", "$2a$05$" + uv)], true],
        ["$2y$ prefix verifies (same vector)", [() => Bcrypt.verify("U*U", "$2y$05$" + uv)], true],
        ["rounds below 4 refused", [() => Bcrypt.hash("x", 3)], THROWS()],
        ["rounds above 31 refused", [() => Bcrypt.hash("x", 32)], THROWS()],
        ["password over 72 bytes refused (RangeError)", [() => Bcrypt.hash("x".repeat(73), 4)], THROWS(RangeError)],
        ["exactly 72 bytes accepted", [() => Bcrypt.verify("x".repeat(72), Bcrypt.hash("x".repeat(72), 4))], true],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 12 — Argon2id (RFC 9106 v0x13; d.ts lines 753-790). Memory floor is
 * 8*parallelism KiB, so small parameters keep every row fast. STRICT bag.
 * ========================================================================== */
{
    const salt = utf8("0123456789abcdef");
    const small = { iterations: 1, memory: 8, parallelism: 1 };
    const phc = Argon2id.hash("hunter2", salt, small);
    const raw = Argon2id.hash("hunter2", salt, { ...small, encoded: false });
    const tamperedTag = raw.slice(); tamperedTag[0] ^= 1;
    const refusesOrFalse = (fn) => { try { return fn() === false || false; } catch (e) { return e instanceof TypeError; } }; // d.ts 774-776: TypeError refusal OR plain false
    runs([
        ["PHC format $argon2id$v=19$m=8,t=1,p=1$salt$tag (unpadded b64)", [() => /^\$argon2id\$v=19\$m=8,t=1,p=1\$[A-Za-z0-9+/]+\$[A-Za-z0-9+/]+$/.test(phc)], true],
        ["the salt rides in the PHC string", [() => phc.includes(b64url(salt))], true],
        ["PHC verify accepts", [() => Argon2id.verify(phc, "hunter2")], true],
        ["PHC verify rejects the wrong password", [() => Argon2id.verify(phc, "hunter3")], false],
        ["encoded:false returns the raw tag bytes", [() => raw instanceof Uint8Array], true],
        ["default hashLen is 32", [() => raw.length], 32],
        ["raw verify accepts (opts override derivation params)", [() => Argon2id.verify("hunter2", salt, raw, small)], true],
        ["raw verify rejects the wrong password", [() => Argon2id.verify("hunter3", salt, raw, small)], false],
        ["raw verify rejects a flipped tag bit", [() => Argon2id.verify("hunter2", salt, tamperedTag, small)], false],
        ["hashLen option honored", [() => Argon2id.hash("pw", salt, { ...small, hashLen: 16, encoded: false }).length], 16],
        ["tampered version v=18 refuses or mismatches (d.ts 774-776)", [() => refusesOrFalse(() => Argon2id.verify(phc.replace("v=19", "v=18"), "hunter2"))], true],
        ["tampered m refuses or mismatches", [() => refusesOrFalse(() => Argon2id.verify(phc.replace("m=8,", "m=9,"), "hunter2"))], true],
        ["corrupt tag base64 refuses or mismatches", [() => refusesOrFalse(() => Argon2id.verify(phc.slice(0, -2) + "zz", "hunter2"))], true],
        ["salt shorter than 8 bytes refused", [() => Argon2id.hash("pw", utf8("short"), small)], THROWS()],
        ["STRICT bag refuses unknown keys", [() => Argon2id.hash("pw", salt, { ...small, bogus: 1 })], THROWS(TypeError, "bogus")],
        ["iterations below 1 refused", [() => Argon2id.hash("pw", salt, { iterations: 0, memory: 8, parallelism: 1 })], THROWS()],
        ["memory below 8*parallelism refused", [() => Argon2id.hash("pw", salt, { iterations: 1, memory: 4, parallelism: 1 })], THROWS()],
        ["asyncStats key set (d.ts 788-789)", [() => Object.keys(Argon2id.asyncStats()).sort().join(",")], "inline,offloadMin,offloaded"],
        ["asyncStats.offloadMin is a number", [() => typeof Argon2id.asyncStats().offloadMin], "number"],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 13 — AEAD official vectors. GCM spec TC13/TC14/TC3 (McGrew-Viega),
 * ChaCha20-Poly1305 RFC 8439 s2.8.2. seal() = ciphertext || 16-byte tag.
 * CONFIG_TLS-gated: skipped loudly when the probe says the names are absent.
 * ========================================================================== */
if (HAS_TLS) {
    const ZERO32 = "0000000000000000000000000000000000000000000000000000000000000000";
    const ZERO12 = "000000000000000000000000";
    const VECTORS = [
        ["GCM TC13: AES-256 zero key/IV, empty PT", [AESGCM, ZERO32, ZERO12, "hex:", null, "530f8afbc74536b9a963b4f1c4cb738b"]],
        ["GCM TC14: 16 zero PT bytes", [AESGCM, ZERO32, ZERO12, "hex:00000000000000000000000000000000", null, "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"]],
        ["GCM TC3: AES-128, 64-byte PT, no AAD", [AESGCM, "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
            "hex:d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", null,
            "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f59854d5c2af327cd64a62cf35abd2ba6fab4"]],
        ["RFC 8439 s2.8.2 ChaCha20-Poly1305 (sunscreen)", [ChaCha20Poly1305, "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", "070000004041424344454647",
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.", "50515253c0c1c2c3c4c5c6c7",
            "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd0600691"]],
    ];
    for (const [label, [Alg, keyHex, nonceHex, ptSpec, aadHex, sealedHex]] of VECTORS) {
        const a = new Alg(byHex(keyHex));
        try {
            const pt = ptSpec.startsWith("hex:") ? byHex(ptSpec.slice(4)) : utf8(ptSpec);
            const aad = aadHex === null ? undefined : byHex(aadHex);
            assertEq(hexOf(a.seal(byHex(nonceHex), pt, aad)), sealedHex, label);
        } finally { a.close(); }
    }

    /* ------------------------------------------------------------------
     * TABLE 14 — key-size acceptance/refusal, parametric over lengths.
     * ------------------------------------------------------------------ */
    const SIZE_ROWS = [
        ["AESGCM key 16 accepted", [AESGCM, 16, true]], ["AESGCM key 24 accepted", [AESGCM, 24, true]], ["AESGCM key 32 accepted", [AESGCM, 32, true]],
        ["AESGCM key 0 refused", [AESGCM, 0, false]], ["AESGCM key 15 refused", [AESGCM, 15, false]], ["AESGCM key 31 refused", [AESGCM, 31, false]], ["AESGCM key 33 refused", [AESGCM, 33, false]],
        ["ChaCha key 32 accepted", [ChaCha20Poly1305, 32, true]], ["ChaCha key 31 refused", [ChaCha20Poly1305, 31, false]],
    ];
    for (const [label, [Alg, klen, ok]] of SIZE_ROWS) {
        if (ok) { const a = new Alg(new Uint8Array(klen).fill(1)); n++; if (!(a instanceof Alg)) throw new Error("assertion failed: " + label); a.close(); }
        else assertThrows(() => new Alg(new Uint8Array(klen)), label);
    }

    /* ------------------------------------------------------------------
     * TABLE 15 — AEAD open() outcome matrix: one seal per algorithm, every
     * mutation the contract names (d.ts lines 888-932).
     * ------------------------------------------------------------------ */
    const NON = "000102030405060708090a0b";
    const K1 = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const K2 = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
    const PT = "top secret", AAD = "aad";
    function aead(Alg, keyHex, mut) {
        const a = new Alg(byHex(keyHex));
        try {
            let s = a.seal(byHex(NON), PT, AAD), nn = byHex(NON), aad = AAD;
            if (mut === "ct0") { s = s.slice(); s[0] ^= 1; }
            else if (mut === "tag") { s = s.slice(); s[s.length - 1] ^= 1; }
            else if (mut === "trunc") s = s.slice(0, 8);
            else if (mut === "nonce") { nn = nn.slice(); nn[0] ^= 1; }
            else if (mut === "aadCase") aad = "Aad";
            else if (mut === "noaad") aad = undefined;
            else if (mut === "aadObj") aad = {};
            else if (mut === "key") { const b = new Alg(byHex(K2)); try { return new TextDecoder().decode(b.open(nn, s, aad)); } finally { b.close(); } }
            return new TextDecoder().decode(a.open(nn, s, aad));
        } finally { a.close(); }
    }
    runs([
        ["AESGCM happy path round trip", [AESGCM, K1, "none"], PT],
        ["AESGCM flipped ciphertext bit -> 'authentication failed'", [AESGCM, K1, "ct0"], THROWS(undefined, "authentication failed")],
        ["AESGCM flipped tag bit -> 'authentication failed'", [AESGCM, K1, "tag"], THROWS(undefined, "authentication failed")],
        ["AESGCM wrong nonce -> 'authentication failed'", [AESGCM, K1, "nonce"], THROWS(undefined, "authentication failed")],
        ["AESGCM wrong-case AAD -> 'authentication failed'", [AESGCM, K1, "aadCase"], THROWS(undefined, "authentication failed")],
        ["AESGCM missing AAD -> 'authentication failed'", [AESGCM, K1, "noaad"], THROWS(undefined, "authentication failed")],
        ["AESGCM wrong key -> 'authentication failed'", [AESGCM, K1, "key"], THROWS(undefined, "authentication failed")],
        ["AESGCM sealed shorter than its tag refused", [AESGCM, K1, "trunc"], THROWS()],
        ["AESGCM options object at the aad position refused, not stringified", [AESGCM, K1, "aadObj"], THROWS()],
        ["ChaCha happy path round trip", [ChaCha20Poly1305, K1, "none"], PT],
        ["ChaCha flipped ciphertext bit refused", [ChaCha20Poly1305, K1, "ct0"], THROWS(undefined, "authentication failed")],
        ["ChaCha flipped tag bit refused", [ChaCha20Poly1305, K1, "tag"], THROWS(undefined, "authentication failed")],
        ["ChaCha missing AAD refused", [ChaCha20Poly1305, K1, "noaad"], THROWS(undefined, "authentication failed")],
        ["ChaCha wrong key refused", [ChaCha20Poly1305, K1, "key"], THROWS(undefined, "authentication failed")],
    ], (Alg, keyHex, mut) => aead(Alg, keyHex, mut));

    // Nonce length is contractual: exactly 12 bytes.
    runs([
        ["AESGCM 11-byte nonce refused", [AESGCM, K1, 11], THROWS()],
        ["AESGCM 13-byte nonce refused", [AESGCM, K1, 13], THROWS()],
        ["ChaCha 11-byte nonce refused", [ChaCha20Poly1305, K1, 11], THROWS()],
        ["ChaCha 12-byte nonce accepted", [ChaCha20Poly1305, K1, 12], true],
    ], (Alg, keyHex, nlen) => {
        const a = new Alg(byHex(keyHex));
        try { a.seal(new Uint8Array(nlen), "x"); return true; } finally { a.close(); }
    });

    // sealRandom: fresh 12-byte CSPRNG nonce returned beside the sealed blob.
    runs([
        ["AESGCM sealRandom nonce is 12 bytes", [() => { const a = new AESGCM(byHex(K1)); try { return a.sealRandom(PT, AAD).nonce.length; } finally { a.close(); } }], 12],
        ["AESGCM sealRandom nonces differ across calls", [() => { const a = new AESGCM(byHex(K1)); try { return hexOf(a.sealRandom("x").nonce) !== hexOf(a.sealRandom("x").nonce); } finally { a.close(); } }], true],
        ["AESGCM sealRandom/open round trip", [() => { const a = new AESGCM(byHex(K1)); try { const r = a.sealRandom(PT, AAD); return new TextDecoder().decode(a.open(r.nonce, r.sealed, AAD)); } finally { a.close(); } }], PT],
        ["ChaCha sealRandom nonce is 12 bytes", [() => { const c = new ChaCha20Poly1305(byHex(K1)); try { return c.sealRandom("x").nonce.length; } finally { c.close(); } }], 12],
    ], (thunk) => thunk());

    // Into forms (d.ts lines 895-907): caller-owned out, detached tag, one-blob layout.
    runs([
        ["sealInto returns bytes written (pt+16)", [() => { const a = new AESGCM(byHex(K1)); try { return a.sealInto(new Uint8Array(64), byHex(NON), "into form", AAD); } finally { a.close(); } }], 9 + 16],
        ["sealInto output == seal output", [() => { const a = new AESGCM(byHex(K1)); try { const o = new Uint8Array(64); const w = a.sealInto(o, byHex(NON), "into form", AAD); return hexOf(o.subarray(0, w)) === hexOf(a.seal(byHex(NON), "into form", AAD)); } finally { a.close(); } }], true],
        ["openInto round trip through a subarray slice", [() => { const a = new AESGCM(byHex(K1)); try { const big = new Uint8Array(64); const w = a.sealInto(big.subarray(8), byHex(NON), "into form", AAD); const b = new Uint8Array(16); const g = a.openInto(b, byHex(NON), big.subarray(8, 8 + w), AAD); return new TextDecoder().decode(b.subarray(0, g)); } finally { a.close(); } }], "into form"],
        ["detached tagOut: out holds pure ciphertext + 16-byte tag buffer", [() => { const a = new AESGCM(byHex(K1)); try { const ct = new Uint8Array(32), tg = new Uint8Array(16); const w = a.sealInto(ct, byHex(NON), "detached", undefined, { tagOut: tg }); return w === 8 && tg.length === 16; } finally { a.close(); } }], true],
        ["openInto {tag} reassembles the detached layout", [() => { const a = new AESGCM(byHex(K1)); try { const ct = new Uint8Array(8), tg = new Uint8Array(16); a.sealInto(ct, byHex(NON), "detached", undefined, { tagOut: tg }); const b = new Uint8Array(8); const g = a.openInto(b, byHex(NON), ct, undefined, { tag: tg }); return new TextDecoder().decode(b.subarray(0, g)); } finally { a.close(); } }], "detached"],
        ["prefixNonce layout leads out with the nonce", [() => { const a = new AESGCM(byHex(K1)); try { const blob = new Uint8Array(12 + 5 + 16); a.sealInto(blob, byHex(NON), "12345", undefined, { prefixNonce: true }); return hexOf(blob.subarray(0, 12)); } finally { a.close(); } }], NON],
        ["openInto {prefixNonce} reads the leading nonce from the blob", [() => { const a = new AESGCM(byHex(K1)); try { const blob = new Uint8Array(12 + 5 + 16); a.sealInto(blob, byHex(NON), "12345", undefined, { prefixNonce: true }); const b = new Uint8Array(5); const g = a.openInto(b, undefined, blob, undefined, { prefixNonce: true }); return new TextDecoder().decode(b.subarray(0, g)); } finally { a.close(); } }], "12345"],
        ["short out throws RangeError before any byte is written", [() => { const a = new AESGCM(byHex(K1)); try { a.sealInto(new Uint8Array(10), byHex(NON), "too long"); } finally { a.close(); } }], THROWS(RangeError)],
        // DOC NOTE: d.ts (AESGCM.openInto, ~line 901-907) only pins "a forged tag throws exactly like open()";
        // it never promises out is zeroed on failure. Original row expected zeroing = misderived.
        ["openInto forged tag throws exactly like open()", [() => { const a = new AESGCM(byHex(K1)); try { const tam = a.seal(byHex(NON), PT, AAD); tam[0] ^= 1; a.openInto(new Uint8Array(26).fill(0xaa), byHex(NON), tam, AAD); return "opened?!"; } finally { a.close(); } }], THROWS(undefined, "authentication failed")],
        ["DynResource surface: [Symbol.dispose] present", [() => { const a = new AESGCM(byHex(K1)); try { return typeof a[Symbol.dispose]; } finally { a.close(); } }], "function"],
    ], (thunk) => thunk());
    {
        const a = new AESGCM(byHex(K1));
        a.dispose();
        runs([
            ["dispose() marks closed", [() => a.closed], true],
            ["use after dispose throws", [() => a.seal(byHex(NON), "x")], THROWS()],
        ], (thunk) => thunk());
    }
}

/* ==========================================================================
 * TABLE 16 — Ed25519 (RFC 8032 s7.1) and X25519 (RFC 7748 s6.1), raw keys.
 * CONFIG_TLS-gated: skipped loudly when the probe says the names are absent.
 * ========================================================================== */
if (HAS_TLS) {
    const T1SEED = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
    const T1PUB = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    const T2SEED = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
    const T2PUB = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
    const APRIV = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
    const APUB = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    const BPRIV = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
    const BPUB = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
    runs([
        ["RFC 8032 TEST 1: sign(seed, '') verifies under the published public key", [() => Ed25519Verify(byHex(T1PUB), "", Ed25519Sign(byHex(T1SEED), ""))], true],
        ["RFC 8032 TEST 2: sign(seed, 0x72) verifies under the published public key", [() => Ed25519Verify(byHex(T2PUB), u8(0x72), Ed25519Sign(byHex(T2SEED), u8(0x72)))], true],
        ["Ed25519 signing is deterministic", [() => hexOf(Ed25519Sign(byHex(T1SEED), "")) === hexOf(Ed25519Sign(byHex(T1SEED), ""))], true],
        ["Ed25519 signature is 64 bytes", [() => Ed25519Sign(byHex(T1SEED), "").length], 64],
        ["tampered signature -> false", [() => { const s = Ed25519Sign(byHex(T1SEED), ""); s[0] ^= 1; return Ed25519Verify(byHex(T1PUB), "", s); }], false],
        ["wrong message -> false", [() => Ed25519Verify(byHex(T1PUB), "x", Ed25519Sign(byHex(T1SEED), ""))], false],
        ["63-byte signature -> false, not a throw", [() => Ed25519Verify(byHex(T1PUB), "", Ed25519Sign(byHex(T1SEED), "").slice(0, 63))], false],
        ["31-byte private key refused by Sign", [() => Ed25519Sign(byHex(T1SEED).slice(0, 31), "")], THROWS()],
        ["31-byte public key is a caller error in Verify (throws)", [() => Ed25519Verify(byHex(T1PUB).slice(0, 31), "", new Uint8Array(64))], THROWS()],
        ["RFC 7748 6.1 shared secret", [() => hexOf(X25519Derive(byHex(APRIV), byHex(BPUB)))], "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"],
        ["X25519 agreement is symmetric (RFC pair, both directions)", [() => eqArr(X25519Derive(byHex(APRIV), byHex(BPUB)), X25519Derive(byHex(BPRIV), byHex(APUB)))], true],
        ["X25519 shared secret is 32 bytes", [() => X25519Derive(byHex(APRIV), byHex(BPUB)).length], 32],
        ["31-byte private key refused by Derive", [() => X25519Derive(byHex(APRIV).slice(0, 31), byHex(BPUB))], THROWS()],
        ["Ed25519 keygen: 32/32 bytes, fresh per call", [() => { const a = Ed25519Generate(), b = Ed25519Generate(); return a.privateKey.length === 32 && a.publicKey.length === 32 && hexOf(a.privateKey) !== hexOf(b.privateKey); }], true],
        ["X25519 keygen: 32/32 bytes", [() => { const a = X25519Generate(); return a.privateKey.length === 32 && a.publicKey.length === 32; }], true],
        ["generated Ed25519 pair round trip", [() => { const k = Ed25519Generate(); return Ed25519Verify(k.publicKey, "hello", Ed25519Sign(k.privateKey, "hello")); }], true],
        ["generated X25519 pairs agree", [() => { const a = X25519Generate(), b = X25519Generate(); return eqArr(X25519Derive(a.privateKey, b.publicKey), X25519Derive(b.privateKey, a.publicKey)); }], true],
    ], (thunk) => thunk());

    // PEM bridge (d.ts lines 878-886): parametric over BOTH curves, one contract.
    for (const [name, gen, fromRaw, toRaw] of [["Ed25519", Ed25519Generate, Ed25519PemFromRaw, Ed25519PemToRaw], ["X25519", X25519Generate, X25519PemFromRaw, X25519PemToRaw]]) {
        const k = gen();
        const pems = fromRaw({ privateKey: k.privateKey, publicKey: k.publicKey });
        runs([
            [name + " private PEM is PKCS#8", [() => pems.privateKey.startsWith("-----BEGIN PRIVATE KEY")], true],
            [name + " public PEM is SPKI", [() => pems.publicKey.includes("-----BEGIN PUBLIC KEY")], true],
            [name + " PemToRaw recovers the seed", [() => eqArr(toRaw(pems.privateKey).privateKey, k.privateKey)], true],
            [name + " private PEM yields both raw halves", [() => eqArr(toRaw(pems.privateKey).publicKey, k.publicKey)], true],
            [name + " public PEM yields publicKey only", [() => toRaw(pems.publicKey).privateKey === undefined], true],
            [name + " SPKI round trips the public key", [() => eqArr(toRaw(pems.publicKey).publicKey, k.publicKey)], true],
        ], (thunk) => thunk());
    }
}

/* ==========================================================================
 * TABLE 17 — ECDSA / ECDH (d.ts lines 854-867). CONFIG_TLS-gated.
 * ========================================================================== */
if (HAS_TLS) {
    const ec = ECDSA.generate("P-256");
    const other = ECDSA.generate("P-256");
    const sig = ECDSA.sign("sha256", ec.privateKey, "msg");
    const der = ECDSA.sign("sha256", ec.privateKey, "msg", { format: "der" });
    runs([
        ["P-256 raw signature is 64 bytes (R||S)", [() => sig.length], 64],
        ["ECDSA raw verify", [() => ECDSA.verify("sha256", ec.publicKey, "msg", sig)], true],
        ["wrong message rejected", [() => ECDSA.verify("sha256", ec.publicKey, "msg!", sig)], false],
        ["wrong public key rejected", [() => ECDSA.verify("sha256", other.publicKey, "msg", sig)], false],
        ["wrong-length raw signature is a plain false", [() => ECDSA.verify("sha256", ec.publicKey, "msg", sig.slice(0, 63))], false],
        ["DER signature is a SEQUENCE", [() => der[0]], 0x30],
        ["DER verify", [() => ECDSA.verify("sha256", ec.publicKey, "msg", der, { format: "der" })], true],
        ["DER bytes read as raw fail on length", [() => ECDSA.verify("sha256", ec.publicKey, "msg", der, { format: "raw" })], false],
        ["unknown curve refused", [() => ECDSA.generate("P-999")], THROWS()],
        ["ECDH P-256 shared secret is 32 bytes", [() => { const a = ECDH.generate("P-256"), b = ECDH.generate("P-256"); return ECDH.derive(a.privateKey, b.publicKey).length; }], 32],
        ["ECDH agreement is symmetric", [() => { const a = ECDH.generate("P-256"), b = ECDH.generate("P-256"); return eqArr(ECDH.derive(a.privateKey, b.publicKey), ECDH.derive(b.privateKey, a.publicKey)); }], true],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 18 — RSA (d.ts lines 792-818). THE one expensive keygen: 2048 bits,
 * the smallest documented modulus, generated once and shared by all rows.
 * CONFIG_TLS-gated.
 * ========================================================================== */
if (HAS_TLS) {
    const rk = RSA.generate(2048);
    const rsig = RSA.sign("sha256", rk.privateKey, "payload");
    const pss0 = RSA.signPSS("sha256", rk.privateKey, "payload", { saltLen: 0 });
    const pssMax = RSA.signPSS("sha256", rk.privateKey, "payload", { saltLen: "max" });
    const pss32 = RSA.signPSS("sha256", rk.privateKey, "payload", { saltLen: 32 });
    const sealedOaep = RSA.OAEP.seal(rk.publicKey, "secret payload", { label: "context" });
    const pssTam = pssMax.slice(); pssTam[10] ^= 1;
    runs([
        ["PEM key pair returned", [() => rk.privateKey.startsWith("-----BEGIN") && rk.publicKey.startsWith("-----BEGIN")], true],
        ["2048-bit modulus -> 256-byte signature", [() => rsig.length], 256],
        ["PKCS#1 v1.5 verify", [() => RSA.verify("sha256", rk.publicKey, "payload", rsig)], true],
        ["wrong message rejected", [() => RSA.verify("sha256", rk.publicKey, "payload!", rsig)], false],
        ["different digest rejected", [() => RSA.verify("sha1", rk.publicKey, "payload", rsig)], false],
        ["md case-insensitive with 'sha-256' alias (d.ts line 795)", [() => RSA.verify("SHA256", rk.publicKey, "payload", RSA.sign("sha-256", rk.privateKey, "payload"))], true],
        ["bits outside {2048,3072,4096} refused", [() => RSA.generate(1024)], THROWS()],
        ["md outside the digest table refused", [() => RSA.sign("md5", rk.privateKey, "x")], THROWS()],
        ["PSS saltLen 0 is deterministic (d.ts line 798)", [() => eqArr(pss0, RSA.signPSS("sha256", rk.privateKey, "payload", { saltLen: 0 }))], true],
        ["PSS verify auto-recovers the salt length", [() => RSA.verifyPSS("sha256", rk.publicKey, "payload", pss0)], true],
        ["PSS 'max' round trip", [() => RSA.verifyPSS("sha256", rk.publicKey, "payload", pssMax, { saltLen: "max" })], true],
        ["PSS fixed saltLen 32 round trip", [() => RSA.verifyPSS("sha256", rk.publicKey, "payload", pss32, { saltLen: 32 })], true],
        ["PSS tampered signature is false", [() => RSA.verifyPSS("sha256", rk.publicKey, "payload", pssTam)], false],
        ["PSS wrong message is false", [() => RSA.verifyPSS("sha256", rk.publicKey, "other", pss32, { saltLen: 32 })], false],
        ["OAEP output length is the modulus size", [() => sealedOaep.length], 256],
        ["OAEP label round trips exactly", [() => new TextDecoder().decode(RSA.OAEP.open(rk.privateKey, sealedOaep, { label: "context" }))], "secret payload"],
        ["OAEP without label round trips", [() => new TextDecoder().decode(RSA.OAEP.open(rk.privateKey, RSA.OAEP.seal(rk.publicKey, "plain")))], "plain"],
        ["OAEP label mismatch refuses", [() => RSA.OAEP.open(rk.privateKey, sealedOaep, { label: "contexT" })], THROWS()],
        ["truncated OAEP blob refuses", [() => RSA.OAEP.open(rk.privateKey, sealedOaep.slice(1))], THROWS()],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 19 — X509 (d.ts lines 820-852). One cheap EC self-signed cert feeds the rows.
 * CONFIG_TLS-gated.
 * ========================================================================== */
if (HAS_TLS) {
    const ec = ECDSA.generate("P-256");
    const certPem = X509.generateSelfSigned({ key: ec.privateKey, subject: "localhost", days: 30, sans: ["myapp.test", "127.0.0.1", "admin@example.test"] });
    const info = X509.parse(certPem);
    runs([
        ["self-signed output is PEM", [() => certPem.startsWith("-----BEGIN CERTIFICATE")], true],
        ["version is 3 (v3 with SAN)", [() => info.version], 3],
        ["serialNumber is hex", [() => /^[0-9a-fA-F]+$/.test(info.serialNumber)], true],
        ["fingerprint is 64 hex chars (SHA-256)", [() => /^[0-9a-f]{64}$/.test(info.fingerprint)], true],
        ["DNS SAN classified by shape", [() => info.sans.dns.includes("myapp.test")], true],
        ["IP SAN classified by shape (inet_pton)", [() => info.sans.ip.includes("127.0.0.1")], true],
        ["'@' classifies as email SAN", [() => info.sans.email.includes("admin@example.test")], true],
        ["validity strings present", [() => typeof info.notBefore === "string" && typeof info.notAfter === "string"], true],
        ["malformed cert refused", [() => X509.parse("definitely not a certificate")], THROWS()],
        ["generateSelfSigned is a strict option bag", [() => X509.generateSelfSigned({ key: ec.privateKey, junk: 1 })], THROWS(TypeError, "junk")],
        ["subjectAltName extension alongside sans refused as ambiguous", [() => X509.generateSelfSigned({ key: ec.privateKey, sans: ["a.test"], extensions: [{ name: "subjectAltName", value: "DNS:b.test" }] })], THROWS()],
        ["unsupported extension name refused by name, never ignored", [() => X509.generateSelfSigned({ key: ec.privateKey, extensions: [{ name: "notARealExtension", value: "x" }] })], THROWS()],
        // days feeds notAfter = now + days*86400s; a value whose product cannot
        // exist as an ASN.1 time is refused, never wrapped (d.ts: days?: number)
        ["absurd days refused instead of wrapping notAfter", [() => X509.generateSelfSigned({ key: ec.privateKey, days: 1e15 })], THROWS(RangeError)],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 20 — HOTP (RFC 4226 App. D Table 1) and TOTP (RFC 6238 App. B).
 * ========================================================================== */
{
    const SEC = "12345678901234567890";
    const RFC4226 = ["755224", "287082", "359152", "969429", "338314", "254676", "287922", "162583", "399871", "520489"];
    for (let c = 0; c < RFC4226.length; c++) {
        assertEq(HOTPGenerate(SEC, c), RFC4226[c], "HOTP counter " + c + " — RFC 4226 Table 1");
        assert(HOTPVerify(SEC, c, RFC4226[c]), "HOTPVerify accepts counter " + c + " — RFC 4226");
    }
    runs([
        ["TOTP T=59 — RFC 6238 (SHA-1 seed, 8 digits)", [{ atSec: 59, digits: 8 }], "94287082"],
        ["TOTP T=1111111109 — RFC 6238", [{ atSec: 1111111109, digits: 8 }], "07081804"],
        ["TOTP T=1234567890 — RFC 6238", [{ atSec: 1234567890, digits: 8 }], "89005924"],
        ["TOTP T=20000000000 — RFC 6238", [{ atSec: 20000000000, digits: 8 }], "65353130"],
    ], (o) => TOTPGenerate(SEC, o));

    runs([
        ["HOTPVerify rejects the wrong counter", [() => HOTPVerify(SEC, 0, RFC4226[1])], false],
        ["malformed (non-digit) code is false, never a throw", [() => HOTPVerify(SEC, 0, "12ab34")], false],
        ["wrong-length code is false", [() => HOTPVerify(SEC, 0, "12345")], false],
        ["empty code is false", [() => HOTPVerify(SEC, 0, "")], false],
        ["negative counter refused", [() => HOTPVerify(SEC, -1, "755224")], THROWS()],
        ["digits below 6 refused", [() => HOTPGenerate(SEC, 0, { digits: 5 })], THROWS()],
        ["digits above 8 refused", [() => HOTPGenerate(SEC, 0, { digits: 9 })], THROWS()],
        ["digits option sizes the code", [() => HOTPGenerate(SEC, 0, { digits: 8 }).length], 8],
        ["secret accepts string or UTF-8 bytes (parity)", [() => HOTPGenerate(utf8(SEC), 0) === HOTPGenerate(SEC, 0)], true],
        // DOC-TENSION: d.ts types counter as number, API.md documents number|bigint; the more specific documented reading wins.
        ["counter accepts a bigint (API.md)", [() => HOTPGenerate(SEC, 0n) === HOTPGenerate(SEC, 0)], true],
        ["default digits is 6", [() => HOTPGenerate(SEC, 0).length], 6],
        ["atSec >= 0 enforced", [() => TOTPGenerate(SEC, { atSec: -1 })], THROWS()],
        ["period > 0 enforced", [() => TOTPGenerate(SEC, { atSec: 0, period: 0 })], THROWS()],
        ["counter steps by period (30 and 31 are one counter)", [() => TOTPGenerate(SEC, { atSec: 30 }) === TOTPGenerate(SEC, { atSec: 31 })], true],
        ["default period 30, default digits 6", [() => TOTPGenerate(SEC, { atSec: 45 }).length], 6],
        ["TOTPVerify at the exact time", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1000 })], true],
        ["one period later with window 0 -> false", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1030 })], false],
        ["window 1 accepts the neighbouring period", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1030, window: 1 })], true],
        ["window includes the exact period too", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1000, window: 1 })], true],
        ["garbage code is false, never a throw", [() => TOTPVerify("secret", "12ab", { atSec: 1000 })], false],
    ], (thunk) => thunk());
}

/* ==========================================================================
 * TABLE 21 — JWT (d.ts lines 1066-1072). HS rows are unconditional; the
 * RS/ES rows need RSA/ECDSA (CONFIG_TLS) and are skipped loudly without it.
 * ========================================================================== */
{
    const secret = "shared secret";
    const token = JWTSign({ sub: "123" }, secret);
    const parts = token.split(".");
    const forgedPayload = b64url(utf8(JSON.stringify({ sub: "124" })));
    const noneToken = b64url(utf8(JSON.stringify({ alg: "none", typ: "JWT" }))) + "." + forgedPayload + ".";
    runs([
        ["JWT is a three-segment JWS", [() => parts.length], 3],
        ["default header alg is HS256", [() => JSON.parse(new TextDecoder().decode(byHex(b64uToHex(parts[0])))).alg], "HS256"],
        ["payload carries the claims", [() => JSON.parse(new TextDecoder().decode(byHex(b64uToHex(parts[1])))).sub], "123"],
        ["HS256 sig == b64url(HMAC-SHA256(signing input)) — RFC 7515", [() => parts[2] === b64url(HMAC("sha256", secret, parts[0] + "." + parts[1]))], true],
        ["HS256 round trip", [() => JWTVerify(token, secret, { algorithms: ["HS256"] }).sub], "123"],
        ["wrong key throws (bad signature)", [() => JWTVerify(token, "another secret", { algorithms: ["HS256"] })], THROWS(TypeError)],
        ["tampered payload throws", [() => JWTVerify(parts[0] + "." + forgedPayload + "." + parts[2], secret, { algorithms: ["HS256"] })], THROWS(TypeError)],
        ["alg:none refused even when allowlisted", [() => JWTVerify(noneToken, secret, { algorithms: ["none", "HS256"] })], THROWS()],
        ["unlisted alg refused", [() => JWTVerify(token, secret, { algorithms: ["HS512"] })], THROWS(TypeError)],
        ["algorithms allowlist is required", [() => JWTVerify(token, secret)], THROWS()],
        ["malformed token throws", [() => JWTVerify("garbage.token", secret, { algorithms: ["HS256"] })], THROWS(TypeError)],
    ], (thunk) => thunk());

    if (HAS_TLS) {
        const rk = RSA.generate(2048);
        const ec = ECDSA.generate("P-256");
        const rsToken = JWTSign({ sub: "rs" }, rk.privateKey, { alg: "RS256" });
        const esToken = JWTSign({ sub: "es" }, ec.privateKey, { alg: "ES256" });
        runs([
            ["RS256 round trip", [() => JWTVerify(rsToken, rk.publicKey, { algorithms: ["RS256"] }).sub], "rs"],
            ["RS256 token refused under an HS-only allowlist", [() => JWTVerify(rsToken, rk.publicKey, { algorithms: ["HS256"] })], THROWS()],
            ["ES256 round trip (raw R||S, never DER)", [() => JWTVerify(esToken, ec.publicKey, { algorithms: ["ES256"] }).sub], "es"],
        ], (thunk) => thunk());
    }
}

/* ==========================================================================
 * TABLE 22 — entropy and constant-time compare (d.ts lines 1037-1048).
 * ========================================================================== */
runs([
    ["RandomBytes length honored", [() => RandomBytes(12).length], 12],
    ["RandomBytes default is 32", [() => RandomBytes().length], 32],
    ["count 0 is inside the documented range", [() => RandomBytes(0).length], 0],
    ["OS entropy: two draws differ", [() => hexOf(RandomBytes(16)) !== hexOf(RandomBytes(16))], true],
    ["TimingSafeEqual true when equal", [() => TimingSafeEqual(u8(1, 2, 3), u8(1, 2, 3))], true],
    ["false on a differing last byte", [() => TimingSafeEqual(u8(1, 2, 3), u8(1, 2, 4))], false],
    ["different lengths return false (d.ts line 1044)", [() => TimingSafeEqual(u8(1, 2), u8(1, 2, 3))], false],
    ["two empty views are equal", [() => TimingSafeEqual(u8(), u8())], true],
    ["ByteView accepts an ArrayBuffer (d.ts line 71)", [() => TimingSafeEqual(u8(1, 2), u8(1, 2).buffer)], true],
], (thunk) => thunk());

print("bb_crypto: all tests passed (" + n + " assertions)");
