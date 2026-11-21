// covers: dyna:validate.IsHex
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_hex");
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

__kitS.section("invalid hex", () => {
for (const a of ["", "abc", "0x12", "0X12", "gg", "12 34", "1.2", "ff ", " ff",
                 "ff;ff", "\uFF11" + "2"  ])
    no(IsHex(a), "invalid hex " + JSON.stringify(a));
ok(IsHex("a".repeat(4096)), "4096 bytes is the input cap, and hex fits inside it");
});

__kitS.section("one byte over the cap is a RangeError", () => {
throws(() => IsHex("a".repeat(4097)), "one byte over the cap is a RangeError");
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
