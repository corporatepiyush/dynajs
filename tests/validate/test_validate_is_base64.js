// covers: dyna:validate.IsBase64
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_base64");
import { cpuNow } from "../lib/cpuclock.js";
import { IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii,
         IsUUID, IsSemver, IsJWT, IsE164,
         IsIPv4, IsIPv6, IsIP, IsPort, IsBase64, IsBase64Url, IsHex,
         IsHexColor, IsNumeric, IsDateString, IsRFC3339, IsJSON,
         IsMimeType, IsStrongPassword }
    from "dyna:validate";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function ok(v, msg) { assert(v === true, msg + " (expected true)"); }
function no(v, msg) { assert(v === false, msg + " (expected false)"); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

__kitS.section("a b c=", () => {
for (const a of ["", "QQ=", "QQ===", "=QQ=", "====", "A", "AB=", "ABC",
                 "AR==", "AB==", "a-b_", "a b c=", "aGVs bG8=", "TQ== ",
                 "a\u0000b="])
    no(IsBase64(a), "invalid base64 " + JSON.stringify(a));
for (const a of ["QQ", "QQRJ", "aGVsbG8", "dGVzdA", "a-_1", "_-_-",
                 "eyJhbGciOiJIUzI1NiJ9"])
    ok(IsBase64Url(a), "valid base64url " + a.slice(0, 10));
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
