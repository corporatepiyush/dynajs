// covers: dyna:validate.IsIP
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_ip");
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

__kitS.section("IsIP accepts v4", () => {
ok(IsIP("1.2.3.4"), "IsIP accepts v4");
});

__kitS.section("IsIP accepts v6", () => {
ok(IsIP("::1"), "IsIP accepts v6");
});

__kitS.section("IsIP accepts the v4-mapped form", () => {
ok(IsIP("::ffff:1.2.3.4"), "IsIP accepts the v4-mapped form");
});

for (const a of ["", "1.2.3", ":::", "example.com", "1.2.3.4.5"])
    no(IsIP(a), "IsIP refuses " + JSON.stringify(a));

__kitS.section("valid port", () => {
for (const a of ["0", "1", "80", "443", "8080", "65534", "65535"])
    ok(IsPort(a), "valid port " + a);
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
