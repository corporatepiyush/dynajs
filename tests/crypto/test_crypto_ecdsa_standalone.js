// covers: dyna:crypto.ECDSA
import { sections as __kitSections } from "../lib/kit.js";
import {
    Bcrypt, Argon2id, Scrypt,
    RSA, ECDSA, ECDH,
    TimingSafeEqual,
} from "dyna:crypto";
const __kitS = __kitSections("test_crypto_ecdsa_standalone", { chain: true });

let n = 0;
function assert(c, msg) {
    n++;
    if (!c) throw new Error("assertion failed: " + msg);
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}
function toHex(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i++) s += u8[i].toString(16).padStart(2, "0");
    return s;
}
function u8(...bytes) { return new Uint8Array(bytes); }

function derEcdsaSigCheck(sig, qBytes) {
    if (!(sig instanceof Uint8Array)) return "not a Uint8Array";
    if (sig.length < 2) return "too short for any TLV";
    if (sig[0] !== 0x30) return "not a DER SEQUENCE";
    let off = 1, len = sig[off++];
    if (len & 0x80) {
        const nlen = len & 0x7f;
        if (nlen === 0 || nlen > 2) return "bad long-form length size";
        if (off + nlen > sig.length) return "length octets past the end";
        if (sig[off] === 0x00) return "non-minimal length encoding";
        len = 0;
        for (let k = 0; k < nlen; k++) len = (len << 8) | sig[off++];
        if (nlen === 1 && len < 0x80) return "long form used for a short length";
        if (nlen === 2 && len <= 0xff) return "long form padded to two octets";
    }
    if (off + len !== sig.length) return "SEQUENCE length does not cover the buffer";
    for (const name of ["r", "s"]) {
        if (off + 2 > sig.length) return "missing INTEGER " + name;
        if (sig[off] !== 0x02) return name + " is not a DER INTEGER";
        const ilen = sig[off + 1];
        if (ilen & 0x80) return name + " has a long-form length";
        off += 2;
        if (ilen === 0) return name + " is empty";
        if (off + ilen > sig.length) return name + " runs past the SEQUENCE";
        const first = sig[off];
        if (first & 0x80) return name + " is negative (high bit set)";
        if (ilen >= 2 && first === 0x00 && !(sig[off + 1] & 0x80))
            return name + " has a redundant leading zero";
        if (ilen > qBytes + 1) return name + " is wider than the field";
        if (ilen === qBytes + 1 && first !== 0x00) return name + " overflows the field";
        off += ilen;
    }
    if (off !== sig.length) return "trailing bytes after the INTEGER pair";
    return null;
}

if (typeof ECDSA !== "undefined") {
let ec256, sigRaw, sigDer, badDer, shapes, runs, ec384, sig384Raw, sig384Der, bad384;
    const msg = "ECDSA message test";
__kitS.section("ECDSA P-256 raw signature is 64 bytes", () => {
    ec256 = ECDSA.generate("P-256");

    sigRaw = ECDSA.sign("SHA256", ec256.privateKey, msg);
    assert(sigRaw instanceof Uint8Array && sigRaw.length === 64, "ECDSA P-256 raw signature is 64 bytes");
    assert(ECDSA.verify("SHA256", ec256.publicKey, msg, sigRaw) === true, "ECDSA P-256 raw signature verifies");
    assert(ECDSA.verify("SHA256", ec256.publicKey, "tampered", sigRaw) === false, "ECDSA P-256 raw verify rejects tampered");
});

__kitS.section("ECDSA DER signature is bytes", () => {
    sigDer = ECDSA.sign("SHA256", ec256.privateKey, msg, { format: "der" });
    assert(sigDer instanceof Uint8Array, "ECDSA DER signature is bytes");
});

    const derParts = (rLead, rExtra, sLead, sExtra) => {
        const r = [rLead].concat(new Array(rExtra).fill(0x91));
        const s = [sLead].concat(new Array(sExtra).fill(0xA2));
        const body = [0x02, r.length].concat(r, [0x02, s.length], s);
        return new Uint8Array([0x30, body.length].concat(body));
    };
__kitS.section("ECDSA DER signature has valid TLV structure", () => {
    badDer = derEcdsaSigCheck(sigDer, 32);
    assert(badDer === null, "ECDSA DER signature has valid TLV structure (got: " + badDer + ")");
    assert(ECDSA.verify("SHA256", ec256.publicKey, msg, sigDer, { format: "der" }) === true, "ECDSA P-256 DER signature verifies");
});
__kitS.section("invalid DER shape refused", () => {
    shapes = [
        [derParts(0x01, 31, 0x01, 31), null, "70: both INTEGERs 32 bytes"],
        [derParts(0x00, 32, 0x00, 32), null, "72: both padded to 33"],
        [derParts(0x00, 32, 0x01, 31), null, "71: only r padded"],
        [derParts(0x01, 31, 0x01, 30), null, "69: s is a 31-byte magnitude"],
        [derParts(0x01, 30, 0x01, 30), null, "68: both magnitudes 31 bytes"],
        [derParts(0x01, 0, 0x01, 0), null, "minimal: both INTEGERs one byte"],
        [new Uint8Array([0x30, 6, 0x02, 1, 0x01, 0x02, 1, 0x01]), null,
         "8: minimal short-form frame"],
        [new Uint8Array([0x31, 6, 0x02, 1, 0x01, 0x02, 1, 0x01]), "SEQUENCE",
         "wrong outer tag"],
        [new Uint8Array([0x30, 6, 0x02, 1, 0x01, 0x02, 1, 0x80]), "negative",
         "s has the sign bit set"],
        [new Uint8Array([0x30, 7, 0x02, 2, 0x00, 0x01, 0x02, 1, 0x01]), "redundant",
         "r has a redundant leading zero"],
        [new Uint8Array([0x30, 5, 0x02, 1, 0x01, 0x02, 2]), "past",
         "s runs past the SEQUENCE"],
        [new Uint8Array([0x30, 4, 0x02, 1, 0x01, 0x02, 1, 0x01]), "cover",
         "SEQUENCE length short of the buffer"],
        [new Uint8Array([0x30, 0x81, 6, 0x02, 1, 0x01, 0x02, 1, 0x01]), "long form",
         "non-minimal long-form length"],
        [new Uint8Array([0x30, 7, 0x02, 0x81, 1, 0x01, 0x02, 1, 0x01]), "long-form",
         "long-form INTEGER length"],
    ];
    for (const [buf, wantBad, why] of shapes) {
        const got = derEcdsaSigCheck(buf, 32);
        if (wantBad === null)
            assert(got === null, "valid DER shape accepted: " + why + " (got: " + got + ")");
        else
            assert(got !== null && got.indexOf(wantBad) >= 0,
                   "invalid DER shape refused for " + wantBad + ": " + why + " (got: " + got + ")");
    }
});
    let minDer = 1 << 30, maxDer = 0, shortSeen = 0;

__kitS.section("DER signature structure over many runs", () => {
    runs = (function () {
        const env = (typeof std !== "undefined" && std.getenv)
            ? std.getenv("DYNAJS_DER_SIG_RUNS") : null;
        const n = env ? parseInt(env, 10) : 5000;
        return (n > 0 && n < 1000000) ? n : 5000;
    })();
    for (let i = 0; i < runs; i++) {
        const s = ECDSA.sign("SHA256", ec256.privateKey, msg + i, { format: "der" });
        const bad = derEcdsaSigCheck(s, 32);
        assert(bad === null, "sig #" + i + " structure (got: " + bad + ")");
        assert(ECDSA.verify("SHA256", ec256.publicKey, msg + i, s, { format: "der" }) === true,
               "sig #" + i + " verifies");
        if (s.length < minDer) minDer = s.length;
        if (s.length > maxDer) maxDer = s.length;
        if (s.length < 70) shortSeen++;
    }
});
__kitS.section("DER length distribution within the structural bounds", () => {
    assert(minDer >= 8 && maxDer <= 72,
           "DER length distribution within the structural bounds [" + minDer + ".." + maxDer + "]");
});
__kitS.section("ECDSA P-384 raw signature is 96 bytes", () => {
    if (shortSeen > 0)
        print("  note: " + shortSeen + "/" + runs + " DER signatures shorter than 70 bytes (legal minimal INTEGERs)");
    else
        print("  note: no sub-70-byte DER in " + runs + " signatures (p~" +
              (Math.pow(0.9985, runs)).toFixed(4) + " under the ~0.15% rate)");

    ec384 = ECDSA.generate("P-384");
    sig384Raw = ECDSA.sign("SHA384", ec384.privateKey, msg);
    assert(sig384Raw instanceof Uint8Array && sig384Raw.length === 96, "ECDSA P-384 raw signature is 96 bytes");
});
__kitS.section("ECDSA P-384 raw signature verifies", () => {
    assert(ECDSA.verify("SHA384", ec384.publicKey, msg, sig384Raw) === true, "ECDSA P-384 raw signature verifies");
});
__kitS.section("ECDSA P-384 DER signature has valid TLV structure", () => {
    sig384Der = ECDSA.sign("SHA384", ec384.privateKey, msg, { format: "der" });
    bad384 = derEcdsaSigCheck(sig384Der, 48);
    assert(bad384 === null, "ECDSA P-384 DER signature has valid TLV structure (got: " + bad384 + ")");
    assert(ECDSA.verify("SHA384", ec384.publicKey, msg, sig384Der, { format: "der" }) === true,
           "ECDSA P-384 DER signature verifies");
});
}
__kitS.end();
if (!__kitS.listing) {

print("test_crypto_standalone: all tests passed (" + n + " assertions)");
}
