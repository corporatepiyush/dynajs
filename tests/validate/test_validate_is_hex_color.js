// covers: dyna:validate.IsHexColor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_hex_color");
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

__kitS.section("valid hex color", () => {
for (const a of ["#fff", "#000", "#aBc123", "#ABCDEF", "#abcdef12",
                 "#FaFbFc99", "#123456", "#00000000"])
    ok(IsHexColor(a), "valid hex color " + a);
});

for (const a of ["", "fff", "#ff", "#ffff", "#fffffff", "#ffffFFFFFF",
                 "#gggggg", "#fff ", " #fff", "#abg", "rgb(0,0,0)", "#-fff"])
    no(IsHexColor(a), "invalid hex color " + JSON.stringify(a));

__kitS.section("valid numeric", () => {
for (const a of ["0", "1", "123", "-123", "+123", "1.5", "-1.5", ".5", "5.",
                 "0.0", "007", "12345678901234567890"])
    ok(IsNumeric(a), "valid numeric " + JSON.stringify(a));
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
