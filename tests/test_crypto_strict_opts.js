// flags: --std
/* test_crypto_strict_opts.js -- straggler: dyna:crypto's pre-existing
 * option bags go strict.
 *
 * Before: a typo'd key ({hashLen2}, {Nn: 16384}, {iteration: ...}) was
 * silently dropped and the KDF ran at the DEFAULT for that parameter -- the
 * call "worked" and derived the wrong (weak, or merely different) key.
 * That is the worst failure mode strict options exist to kill.
 *
 * Bags covered (the phase-1 enumeration):
 *   Argon2id.hash/verify raw form  { iterations, memory, parallelism, hashLen, encoded }
 *   Scrypt(password, salt, opts)   { N, r, p, keyLen }
 *   HKDF({ key, hash, salt, info, length })
 *   PBKDF2({ password, hash, salt, length, iterations })
 * The BLAKE opts bags went strict in the same milestone
 * (tests/test_hash_keyed.js pins those).
 */
import { Argon2id, Scrypt, HKDF, PBKDF2 } from "dyna:crypto";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const throws = (fn, rx, m) => {
    try { fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
    catch (e) {
        if (!(e instanceof TypeError)) { fail++; print("  FAIL: " + m + " wrong kind " + (e && e.constructor.name) + " [" + e.message + "]"); return; }
        if (rx && !rx.test(e.message)) { fail++; print("  FAIL: " + m + " message [" + e.message + "]"); return; }
        pass++;
    }
};
const eqb = (a, b, m) => {
    if (a.length !== b.length) { fail++; print("  FAIL: " + m); return; }
    for (let i = 0; i < a.length; i++)
        if (a[i] !== b[i]) { fail++; print("  FAIL: " + m); return; }
    pass++;
};

const pwd = "correct horse battery staple";
const salt = "some salt";

/* ---- 1. the honest keys still work, and work IDENTICALLY ---- */
const base = Argon2id.hash(pwd, salt, { iterations: 2, memory: 8192, parallelism: 1, hashLen: 32, encoded: false });
eqb(Argon2id.hash(pwd, salt, { iterations: 2, memory: 8192, parallelism: 1, hashLen: 32, encoded: false }),
    base, "argon2 with the full bag is deterministic");
eqb(Argon2id.hash(pwd, salt, { iterations: 2, memory: 8192, parallelism: 1, hashLen: 32, encoded: false }),
    base, "`encoded: false` spelled identically is deterministic");
ok(Argon2id.hash(pwd, salt, {}) === Argon2id.hash(pwd, salt),
   "an empty bag equals the default form (both PHC strings)");

/* ---- 2. the bogus-key matrix ---- */
throws(() => Argon2id.hash(pwd, salt, { iterations: 2, mem: 8192 }), /unknown option "mem" \(valid: /,
       "argon2 typo'd `mem` refuses by name");
throws(() => Argon2id.hash(pwd, salt, { hashLen2: 32 }), /unknown option "hashLen2"/,
       "argon2 `hashLen2` refuses");
throws(() => Argon2id.verify(pwd, salt, base, { encoded: true, extra: 1 }), /unknown option "extra"/,
       "argon2 verify-raw form refuses unknown keys too");
throws(() => Scrypt(pwd, salt, { N: 1024, r: 8, p: 1, keyLen: 32, iterat: 1 }), /unknown option "iterat" \(valid: N, r, p, keyLen\)/,
       "scrypt typo'd key refuses with the full valid set");
throws(() => HKDF({ key: "k", hash: "sha256", salt: "s", info: "i", len: 32 }), /unknown option "len"/,
       "hkdf `len` refuses (the real key is `length`)");
throws(() => PBKDF2({ password: "p", hash: "sha256", salt: "s", length: 32, iter: 1000 }), /unknown option "iter"/,
       "pbkdf2 `iter` refuses (the real key is `iterations`)");

/* ---- 3. the values did not move: a strict check is not a semantic change ---- */
eqb(Scrypt(pwd, salt, { N: 1024, r: 8, p: 1, keyLen: 32 }),
    Scrypt(pwd, salt, { N: 1024, r: 8, p: 1, keyLen: 32 }),
    "scrypt deterministic with the strict check in place");
eqb(HKDF({ key: "k", hash: "sha256", info: "i", length: 32 }),
    HKDF({ key: "k", hash: "sha256", info: "i", length: 32 }),
    "hkdf with salt omitted is deterministic (optional keys stay optional)");
eqb(PBKDF2({ password: "p", hash: "sha256", salt: "s", length: 32, iterations: 1000 }),
    PBKDF2({ password: "p", hash: "sha256", salt: "s", length: 32, iterations: 1000 }),
    "pbkdf2 deterministic");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_crypto_strict_opts: " + fail + " failures");
