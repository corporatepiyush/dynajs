// covers: dyna:validate.IsPort
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_port");
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

for (const a of ["", "65536", "99999", "123456", "-1", "+80", "007", "00",
                 "080", "80 ", " 80", "8 0", "0x50", "4e2", "1.5",
                 "\u066080"  ])
    no(IsPort(a), "invalid port " + JSON.stringify(a));

__kitS.section("valid base64", () => {
for (const a of ["QQ==", "aGVsbG8=", "aGVsbG8h", "TWFu", "Zm9vYmFy",
                 "SGVsbG8sIHdvcmxkIQ==", "AAAA", "/+//", "//52", "a+b/",
                 "Zm9vYmE=", "TQ=="])
    ok(IsBase64(a), "valid base64 " + a.slice(0, 8));
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
