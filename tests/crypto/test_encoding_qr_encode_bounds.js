// timeout: 120
// covers: dyna:encoding.QREncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_qr_encode_bounds");
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
function throwsMatch(fn, re, msg) {
    n++;
    try {
        fn();
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

throwsMatch(() => QREncode("x", { ecc: " " }),
    /ecc/i, "QREncode rejects the space ecc (ecl==4 out-of-bounds read)");

__kitS.section("lowercase and uppercase L/M/Q/H still work", () => {
ok(QREncode("x", { ecc: "l" }) !== undefined && QREncode("x", { ecc: "H" }) !== undefined,
    "lowercase and uppercase L/M/Q/H still work");
console.log("test_crypto_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_crypto_bounds: " + failed + " failures");
});
__kitS.end();
