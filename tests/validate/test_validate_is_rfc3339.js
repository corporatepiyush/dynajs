// covers: dyna:validate.IsRFC3339
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_rfc3339");
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

for (const a of ["", "2023-01-15T10:30:00", "2023-01-15 10:30:00Z",
                 "2023-13-01T10:30:00Z", "2023-02-29T00:00:00Z",
                 "2023-01-15T24:00:00Z", "2023-01-15T10:60:00Z",
                 "2023-01-15T10:30:61Z", "2023-01-15T10:30:00+24:00",
                 "2023-01-15T10:30:00+05:60", "2023-1-15T10:30:00Z",
                 "20230115T103000Z", "2023-01-15T10:30:00.Z",
                 "2023-01-15T10:30:00Z ", "2023-01-15T10:30:00ZZ",
                 "2023-01-15T10:30:00+z:30", "2023-01-15"])
    no(IsRFC3339(a), "invalid RFC3339 " + JSON.stringify(a));

__kitS.section("valid JSON", () => {
for (const a of ['{}', '[]', 'null', 'true', 'false', '"str"', '42', '-1.5e3',
                 '{"a":[1,2,{"b":null}]}', '""', '[1,2,3]'])
    ok(IsJSON(a), "valid JSON " + a);
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
