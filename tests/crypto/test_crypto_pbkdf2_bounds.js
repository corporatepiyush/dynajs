// timeout: 120
// covers: dyna:crypto.PBKDF2
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_pbkdf2_bounds");
import { Argon2id, PBKDF2, HKDF, JWTSign, JWTVerify } from "dyna:crypto";
import { QREncode, QRToString, Base64URLEncode } from "dyna:encoding";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
async function throwsMatch(fn, re, msg) {
    n++;
    try {
        await fn();
    } catch (e) {
        if (re.test(String(e)))
            return;
        failed++;
        console.log("  FAIL " + msg + " (threw " + String(e).slice(0, 80) + ")");
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

await throwsMatch(() => PBKDF2({ password: "x", iterations: 1000 }),
    /salt/i, "PBKDF2 without salt throws");

await __kitS.section("PBKDF2 with salt works", async () => {
ok(typeof await PBKDF2({ password: "x", salt: "s", iterations: 1000, length: 16 }) !== "undefined",
    "PBKDF2 with salt works");
});
__kitS.end();

await throwsMatch(() => PBKDF2({ password: "x", salt: "s", iterations: 0 }),
    /iterations/i, "PBKDF2 iterations 0 refused");

await throwsMatch(() => PBKDF2({ password: "x", salt: "s", length: 1 << 21 }),
    /length/i, "PBKDF2 oversize output refused");
console.log("test_crypto_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_crypto_bounds: " + failed + " failures");
