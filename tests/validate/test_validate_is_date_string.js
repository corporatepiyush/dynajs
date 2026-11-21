// covers: dyna:validate.IsDateString
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_date_string");
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

for (const a of ["", "2023-02-29", "1900-02-29", "2100-02-29", "2023-13-01",
                 "2023-00-10", "2023-01-00", "2023-04-31", "2023-1-01",
                 "2023-01-1", "20230101", "2023/01/01", "2023-01-31 ",
                 " 2023-01-31", "2023-1b-01", "23-01-01"])
    no(IsDateString(a), "invalid date " + JSON.stringify(a));

__kitS.section("valid RFC3339", () => {
for (const a of ["2023-01-15T10:30:00Z", "2023-01-15t10:30:00z",
                 "2023-12-31T23:59:60Z", "2020-02-29T00:00:00Z",
                 "2023-01-15T10:30:00+05:30", "2023-01-15T10:30:00-08:00",
                 "2023-01-15T10:30:00.123Z", "2023-01-15T10:30:00.123456789Z",
                 "2023-01-15T10:30:00-00:00", "0000-01-01T00:00:00Z",
                 "2023-01-15T10:30:00+00:00"])
    ok(IsRFC3339(a), "valid RFC3339 " + a);
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
