// covers: dyna:validate.IsIPv4
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_i_pv4");
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

__kitS.section("valid IPv4:", () => {
for (const a of ["0.0.0.0", "255.255.255.255", "127.0.0.1", "8.8.8.8",
                 "192.168.1.100", "1.2.3.4", "10.0.0.1", "100.200.250.255",
                 "9.9.9.9", "203.0.113.7", "1.22.233.44"])
    ok(IsIPv4(a), "valid IPv4: " + a);
});

for (const a of ["", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.256",
                 "999.999.999.999", "127.000.000.001", "01.2.3.4", "1.02.3.4",
                 "1.2.3.", ".1.2.3.4", "1..2.3", "1.2.3.4 ", " 1.2.3.4",
                 "1.2.3.-4", "+1.2.3.4", "0x1.2.3.4", "1.2.3.4\n", "a.b.c.d",
                 "2130706433", "1.2.3.4:80", "1.2.3.4/24", "1.2.3.\u0001"])
    no(IsIPv4(a), "invalid IPv4: " + JSON.stringify(a));

__kitS.section("valid IPv6:", () => {
for (const a of ["::", "::1", "1::", "fe80::1", "1:2:3:4:5:6:7:8",
                 "2001:db8::8a2e:370:7334", "::ffff:192.168.1.1",
                 "64:ff9b::1.2.3.4", "0:0:0:0:0:0:0:1",
                 "0001:0000:0000:0000:0000:0000:0000:0001",
                 "2001:0DB8:0:0:0:0:2:1", "1:2:3:4:5:6:7::",
                 "::1:2:3:4:5:6:7", "1:2:3:4:5:6:1.2.3.4", "::ffff:1.2.3.4"])
    ok(IsIPv6(a), "valid IPv6: " + a);
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
