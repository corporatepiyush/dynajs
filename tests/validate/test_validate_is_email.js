// covers: dyna:validate.IsEmail
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_email");
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

__kitS.section("valid:", () => {
for (const a of ["a@b.co", "user@example.com", "first.last@example.com",
                 "user+tag@example.co.uk", "u_n-a.m'e@sub.domain.org",
                 "x!#$%&'*+-/=?^_`{|}~@example.com", "a@b-c.de"])
    ok(IsEmail(a), "valid: " + a);
});

for (const a of ["", "a", "@", "a@", "@b.co", "a@b", "a@@b.co", "a@b@c.co",
                 "a b@c.co", "a@b .co", ".a@b.co", "a.@b.co", "a..b@c.co",
                 "a@.b.co", "a@b..co", "a@b.co.", "a@-b.co", "a@b-.co",
                 "a@b.c", "a@b.c0m", "a@b,co", "a\u0000b@c.co"])
    no(IsEmail(a), "invalid: " + JSON.stringify(a));

__kitS.section("a label edge hyphen after a dot:", () => {
for (const a of ["a@b.-cd.com", "ab@mail.-example.com", "a@b.c.-d.com",
                 "a@b.-cd.-ef.com", "a@b.cd-.com"])
    no(IsEmail(a), "a label edge hyphen after a dot: " + a);
ok(IsEmail("a@b.c--d.com"), "a doubled hyphen inside a label is legal");
});

__kitS.section("a 64-char local part is the limit", () => {
ok(IsEmail("a".repeat(64) + "@b.co"), "a 64-char local part is the limit");
});

no(IsEmail("a".repeat(65) + "@b.co"), "65 is over it");

__kitS.section("a 63-char domain label is the limit", () => {
ok(IsEmail("a@" + "b".repeat(63) + ".com"), "a 63-char domain label is the limit");
});

__kitS.section("a 64-char domain label is over it", () => {
no(IsEmail("a@" + "b".repeat(64) + ".com"), "a 64-char domain label is over it");
ok(IsEmail("a@" + "b".repeat(60) + "." + "c".repeat(63)), "the TLD is capped as a label too");
});

__kitS.section("a validator refuses", () => {
for (const fn of [IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii,
                  IsIPv4, IsIPv6, IsIP, IsPort, IsBase64, IsBase64Url, IsHex,
                  IsHexColor, IsNumeric, IsDateString, IsRFC3339, IsJSON,
                  IsMimeType, IsStrongPassword]) {
    for (const bad of [null, undefined, 42, {}, []])
        throws(() => fn(bad), "a validator refuses " + JSON.stringify(bad));
    throws(() => fn("x".repeat(5000)), "and refuses an over-long input");
}
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
