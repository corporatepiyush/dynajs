// covers: dyna:validate.IsNumeric
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_numeric");
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

for (const a of ["", "-", "+", ".", "-.", "+.", "1.2.3", "1e5", "1E5", "abc",
                 "1 ", " 1", "1,5", "--1", "1-", "0x10", "\u00BD",
                 "\u22125"  ])
    no(IsNumeric(a), "invalid numeric " + JSON.stringify(a));

__kitS.section("valid date", () => {
for (const a of ["2023-01-31", "2023-02-28", "2020-02-29", "2024-02-29",
                 "2000-02-29", "1999-12-31", "0000-01-01", "1970-01-01",
                 "0004-02-29"])
    ok(IsDateString(a), "valid date " + a);
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
