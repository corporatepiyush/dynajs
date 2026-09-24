/* test_crypto_surface.js -- the dyna:crypto alias surface, from the crypto
 * side (audit X-15).
 *
 * Since P14, dyna:crypto re-exports dyna:hash's unkeyed one-shots under the
 * same names. test_hash_split.js pins a SAMPLE of those aliases; the
 * alias-qualified coverage analysis still found 39 of dyna:crypto's 73
 * exports unreferenced from dyna:crypto-importing tests: the 38 hash-alias
 * names plus ECDH's derive. The behavior is tested via the dyna:hash twins,
 * so nothing pinned the dyna:crypto registrations themselves from this side.
 * This file pins, permanently:
 *
 *   1. every one of the 38 alias names exists on dyna:crypto and agrees
 *      with its dyna:hash counterpart over a fixed corpus (the alias set
 *      cannot drift by construction -- one shared C table -- but the two
 *      MODULE registrations can: this catches a dropped or renamed
 *      registration on either side);
 *   2. dyna:crypto's TOTAL export count equals the number of runtime-level
 *      declarations in its dynajs.d.ts block (both drift directions);
 *   3. the ECDH derive path works from dyna:crypto (P-256/P-384 symmetry,
 *      determinism, and X25519 cross-check).
 *
 * The alias functions are DISTINCT function objects per module (one shared C
 * entry point, separate JSFunction wrappers -- X-13), so "identity" here is
 * BEHAVIORAL: same arguments, same bytes/hex/number out.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_crypto_surface.js
 * Prints "test_crypto_surface: all N tests passed"; throws on failure. */

import * as crypto from "dyna:crypto";
import * as hash from "dyna:hash";
import { Path, readFile } from "dyna:file";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function sameOut(a, b, msg) {
    if (a instanceof Uint8Array && b instanceof Uint8Array) {
        n++;
        if (a.length !== b.length)
            throw new Error("assertion failed: " + msg + " (length "
                            + a.length + " vs " + b.length + ")");
        for (let i = 0; i < a.length; i++)
            if (a[i] !== b[i])
                throw new Error("assertion failed: " + msg + " (byte " + i + ")");
        return;
    }
    n++;
    if (a !== b)
        throw new Error("assertion failed: " + msg + "\n  got:  " + a
                        + "\n  want: " + b);
}

/* -------------------------------------------------- 1. the 38-name table */
const ALIASES = [
    "MD5", "MD5Hex", "SHA1", "SHA1Hex", "SHA224", "SHA224Hex",
    "SHA256", "SHA256Hex", "SHA384", "SHA384Hex", "SHA512", "SHA512Hex",
    "SHA3_224", "SHA3_224Hex", "SHA3_256", "SHA3_256Hex",
    "SHA3_384", "SHA3_384Hex", "SHA3_512", "SHA3_512Hex",
    "Keccak256", "Keccak256Hex",
    "SHAKE128", "SHAKE128Hex", "SHAKE256", "SHAKE256Hex",
    "BLAKE3", "BLAKE3Hex", "BLAKE2b", "BLAKE2bHex", "BLAKE2s", "BLAKE2sHex",
    "Murmur3_128", "Murmur3_128Hex", "XXHash32", "XXHash64",
    "CRC32", "CRC32C",
];
assert(ALIASES.length === 38, "the alias table has 38 names");

/* One corpus entry per input class: empty, short text, a longer message,
 * raw bytes (0..255), and a seed-able second argument for the seeded pair. */
const CORPUS = [
    "",
    "abc",
    "The quick brown fox jumps over the lazy dog",
    new Uint8Array(256).map((_, i) => i),
];

for (const name of ALIASES) {
    assert(typeof crypto[name] === "function",
           "dyna:crypto exports " + name);
    assert(typeof hash[name] === "function",
           "dyna:hash still exports " + name + " (the alias source)");
    for (const input of CORPUS) {
        sameOut(crypto[name](input), hash[name](input),
                name + " agrees with dyna:hash on "
                + (typeof input === "string" ? JSON.stringify(input.slice(0, 12))
                                             : "bytes 0..255"));
    }
    /* seeded/length variants take the same second argument on both sides */
    if (["XXHash32", "XXHash64", "Murmur3_128", "Murmur3_128Hex"].indexOf(name) >= 0)
        sameOut(crypto[name]("seeded", 7), hash[name]("seeded", 7),
                name + " agrees on a seeded call");
    if (["SHAKE128", "SHAKE128Hex", "SHAKE256", "SHAKE256Hex",
         "BLAKE3", "BLAKE3Hex", "BLAKE2b", "BLAKE2bHex",
         "BLAKE2s", "BLAKE2sHex"].indexOf(name) >= 0)
        sameOut(crypto[name]("abc", 32), hash[name]("abc", 32),
                name + " agrees on an explicit length");
}

/* --------------------------------------- 2. export count matches the d.ts */
/* The runner's cwd is the repo root; Path resolves relative names against the
 * SCRIPT directory, so anchor the d.ts at the process cwd explicitly. */
let dtsPath = Path.cwd().join("dynajs.d.ts");
try {
    readFile(dtsPath);
} catch (e) {
    dtsPath = Path.cwd().join("../dynajs.d.ts");
}
const dts = readFile(dtsPath);

function stripComments(s) {
    return s.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
}
function moduleBody(src, name) {
    let i = src.indexOf('declare module "' + name + '"');
    assert(i >= 0, "d.ts declares " + name);
    i = src.indexOf("{", i);
    let depth = 1, j = i + 1;
    while (depth && j < src.length) {
        if (src[j] === "{") depth++;
        else if (src[j] === "}") depth--;
        j++;
    }
    return src.slice(i + 1, j - 1);
}
function declaredRuntimeNames(body) {
    const names = new Set();
    const re = /^ {4}(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?(?:class|function|const|var|enum|namespace)\s+([A-Za-z_$][\w$]*)/gm;
    let m;
    while ((m = re.exec(body)) !== null) names.add(m[1]);
    return names;
}
const cryptoBlock = stripComments(moduleBody(dts, "dyna:crypto"));
const declared = declaredRuntimeNames(cryptoBlock);
/* Names behind CONFIG_TLS are absent from a no-TLS build; here the build has
 * TLS (ECDH asserted below), so the full count must match. */
const liveNames = Object.getOwnPropertyNames(crypto);
assert(liveNames.length === declared.size,
       "dyna:crypto export count (" + liveNames.length + ") matches d.ts ("
       + declared.size + ")");
for (const name of declared)
    assert(liveNames.indexOf(name) >= 0,
           "declared dyna:crypto name is live: " + name);
for (const name of liveNames)
    assert(declared.has(name),
           "live dyna:crypto export is declared: " + name);

/* ------------------------------------------------ 3. the ECDH derive path */
/* dyna:crypto's own derive entry points, exercised from THIS import: ECDH
 * (P-256/P-384 over X9.63) and the X25519 one-shot. */
assert(typeof crypto.ECDH.derive === "function", "crypto.ECDH.derive exists");

function bytesEq(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
for (const curve of ["P-256", "P-384"]) {
    const A = crypto.ECDH.generate(curve);
    const B = crypto.ECDH.generate(curve);
    const sAB = crypto.ECDH.derive(A.privateKey, B.publicKey);
    const sBA = crypto.ECDH.derive(B.privateKey, A.publicKey);
    assert(sAB instanceof Uint8Array && sAB.length > 0,
           curve + ": derive returns a non-empty secret");
    assert(bytesEq(sAB, sBA), curve + ": ECDH is symmetric");
    const sAA = crypto.ECDH.derive(A.privateKey, A.publicKey);
    assert(!bytesEq(sAB, sAA), curve + ": a different pair derives a different secret");
    const sAB2 = crypto.ECDH.derive(A.privateKey, B.publicKey);
    assert(bytesEq(sAB, sAB2), curve + ": derive is deterministic");
}

/* X25519Derive: raw 32-byte keys, same symmetric property. */
const xA = crypto.X25519Generate();
const xB = crypto.X25519Generate();
const xAB = crypto.X25519Derive(xA.privateKey, xB.publicKey);
assert(bytesEq(xAB, crypto.X25519Derive(xB.privateKey, xA.publicKey)),
       "X25519: ECDH is symmetric");
assert(xAB.length === 32, "X25519 secret is 32 bytes");

print("test_crypto_surface: all " + n + " tests passed");
