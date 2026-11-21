// timeout: 120
// covers: dyna:otp.TOTPVerify
import { TOTPVerify } from "dyna:otp";
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

throwsMatch(() => TOTPVerify("c2VjcmV0", "000000", { window: 500000 }),
    /window/i, "TOTPVerify refuses an impractical window");
console.log("test_otp_totp_verify_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_otp_totp_verify_bounds: " + failed + " failures");
