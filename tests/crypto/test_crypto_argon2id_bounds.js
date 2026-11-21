// timeout: 120
// covers: dyna:crypto.Argon2id
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_argon2id_bounds");
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

const bigPhc = "$argon2id$v=19$m=4194304,t=16,p=16$MDEyMzQ1Njc4OWFiY2RlZg$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

await throwsMatch(() => Argon2id.verify(bigPhc, "pw"),
    /cap|memory/i, "Argon2id.verify refuses a 4 GiB PHC memory parameter");

const smallPhc = "$argon2id$v=19$m=8192,t=1,p=1$MDEyMzQ1Njc4OWFiY2RlZg$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

await __kitS.section("small PHC verifies (returns false, no throw)", async () => {
try {
    await Argon2id.verify(smallPhc, "pw");
    ok(true, "small PHC verifies (returns false, no throw)");
} catch (e) {
    ok(false, "small PHC must not throw: " + e);
}
console.log("test_crypto_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_crypto_bounds: " + failed + " failures");
});
__kitS.end();
