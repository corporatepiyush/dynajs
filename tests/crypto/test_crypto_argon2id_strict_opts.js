// covers: dyna:crypto.Argon2id
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_argon2id_strict_opts");
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

eqb(await Argon2id.hash(pwd, salt, { iterations: 2, memory: 8192, parallelism: 1, hashLen: 32, encoded: false }),
    base, "argon2 with the full bag is deterministic");

eqb(await Argon2id.hash(pwd, salt, { iterations: 2, memory: 8192, parallelism: 1, hashLen: 32, encoded: false }),
    base, "`encoded: false` spelled identically is deterministic");

await __kitS.section("an empty bag equals the default form (both PHC strings)", async () => {
ok(await Argon2id.hash(pwd, salt, {}) === await Argon2id.hash(pwd, salt),
   "an empty bag equals the default form (both PHC strings)");
});

await __kitS.section("argon2 typo'd `mem` refuses by name", async () => {
await throws(() => Argon2id.hash(pwd, salt, { iterations: 2, mem: 8192 }), /unknown option "mem" \(valid: /,
       "argon2 typo'd `mem` refuses by name");
});

await __kitS.section("argon2 `hashLen2` refuses", async () => {
await throws(() => Argon2id.hash(pwd, salt, { hashLen2: 32 }), /unknown option "hashLen2"/,
       "argon2 `hashLen2` refuses");
});

await __kitS.section("argon2 verify-raw form refuses unknown keys too", async () => {
await throws(() => Argon2id.verify(pwd, salt, base, { encoded: true, extra: 1 }), /unknown option "extra"/,
       "argon2 verify-raw form refuses unknown keys too");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_crypto_strict_opts: " + fail + " failures");
