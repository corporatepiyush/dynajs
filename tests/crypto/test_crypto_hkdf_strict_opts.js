// covers: dyna:crypto.HKDF
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hkdf_strict_opts");
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

const base = await Argon2id.hash(pwd, salt, { iterations: 2, memory: 8192, parallelism: 1, hashLen: 32, encoded: false });

await __kitS.section("hkdf `len` refuses (the real key is `length`)", async () => {
throws(() => HKDF({ key: "k", hash: "sha256", salt: "s", info: "i", len: 32 }), /unknown option "len"/,
       "hkdf `len` refuses (the real key is `length`)");
});
__kitS.end();

eqb(HKDF({ key: "k", hash: "sha256", info: "i", length: 32 }),
    HKDF({ key: "k", hash: "sha256", info: "i", length: 32 }),
    "hkdf with salt omitted is deterministic (optional keys stay optional)");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_crypto_strict_opts: " + fail + " failures");
