// covers: dyna:validate.IsAscii
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_ascii");
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

no(IsAscii("caf\u00E9"), "a Latin-1 character is not ASCII");
no(IsAscii("\u4F60"), "a CJK character is not ASCII");
no(IsAscii(""), "empty is not ascii");
no(IsAlpha("caf\u00E9"), "an accented letter is not [A-Za-z]");
no(IsAlphanumeric("\u4F60"), "CJK is not alphanumeric");

__kitS.section("8 digits is the floor", () => {
ok(IsE164("12345678"), "8 digits is the floor");
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
