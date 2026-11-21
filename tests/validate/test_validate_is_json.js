// covers: dyna:validate.IsJSON
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_json");
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

__kitS.section("invalid JSON", () => {
for (const a of ['', '{', '}', "{'a':1}", 'undefined', '{"a":1,}', 'NaN',
                 '"unterminated', '01', '+1', '[1,]', "{'a':1}", '"\x01"',
                 '{a:1}', 'True'])
    no(IsJSON(a), "invalid JSON " + JSON.stringify(a));
ok(IsJSON("[".repeat(2048) + "]".repeat(2048)),
   "2048-deep nesting at the 4096-byte cap parses");
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
