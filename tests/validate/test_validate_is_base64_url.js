// covers: dyna:validate.IsBase64Url
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_base64_url");
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

for (const a of ["", "QQ==", "a+b", "a/b", "a.b", "a b", "a=b", "+/=",
                 "n", "QQR", "CAG", "kZKl2", "a-", "AB"])
    no(IsBase64Url(a), "invalid base64url " + JSON.stringify(a));

no(IsBase64("aGVsbG8"), "unpadded base64url text is not canonical base64");
no(IsBase64Url("aGVsbG8="), "padded base64 is not base64url");
no(IsBase64Url("a+b/"), "the standard alphabet is not the url alphabet");

__kitS.section("valid hex", () => {
for (const a of ["00", "ff", "FF", "0123456789abcdef", "ABCDEF", "deadBEEF",
                 "00112233"])
    ok(IsHex(a), "valid hex " + a.slice(0, 8));
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
