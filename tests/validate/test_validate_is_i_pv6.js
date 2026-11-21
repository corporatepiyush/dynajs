// covers: dyna:validate.IsIPv6
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_i_pv6");
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

__kitS.section("invalid IPv6:", () => {
for (const a of ["", ":", ":::", "1:2:3:4:5:6:7:8:9", "1::2::3", "1:::2",
                 "1:2:3:4:5:6:7", "12345::", "1:2:3:4:5:6:7:8:",
                 ":1:2:3:4:5:6:7:8", "fe80::1%eth0", "::ffff:192.168.1.256",
                 "::ffff:01.2.3.4", "1.2.3.4", "1:2:3:4:5:1.2.3.4",
                 "1:2:3:4:5:6:7:1.2.3.4", "::1:2:3:4:5:6:7:8", "g::1",
                 "1.2.3.4::", "1:2:3:4:5:6:1.2.3.4:8", "::-1",
                 "1:2:3:4:5:6:7:8:", "0:0:0:0:0:0:0:0:0",
                 "1:2:3:4:5:6::1.2.3.4", "1:2:3:4:5:6:7::8"])
    no(IsIPv6(a), "invalid IPv6: " + JSON.stringify(a));
ok(IsIPv6("0:0:0:0:0:0:0:0"), "the all-zero address, written out");
});

__kitS.section("the all-zero address in the v4-tail form", () => {
ok(IsIPv6("::0.0.0.0"), "the all-zero address in the v4-tail form");
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
