// covers: dyna:crypto.Scrypt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_scrypt_strict_opts");
import { Argon2id, Scrypt, HKDF, PBKDF2 } from "dyna:crypto";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const throws = async (fn, rx, m) => {
    try { await fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
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

const base = await Argon2id.hash(pwd, salt, { iterations: 2, memory: 8192, parallelism: 1, hashLen: 32, encoded: false });

await __kitS.section("scrypt typo'd key refuses with the full valid set", async () => {
await throws(() => Scrypt(pwd, salt, { N: 1024, r: 8, p: 1, keyLen: 32, iterat: 1 }), /unknown option "iterat" \(valid: N, r, p, keyLen, dyna\)/,
       "scrypt typo'd key refuses with the full valid set");
});
__kitS.end();

eqb(await Scrypt(pwd, salt, { N: 1024, r: 8, p: 1, keyLen: 32 }),
    await Scrypt(pwd, salt, { N: 1024, r: 8, p: 1, keyLen: 32 }),
    "scrypt deterministic with the strict check in place");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_crypto_strict_opts: " + fail + " failures");
