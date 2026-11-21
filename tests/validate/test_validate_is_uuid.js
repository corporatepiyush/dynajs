// covers: dyna:validate.IsUUID
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_uuid");
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

__kitS.section("no stray __dyna_v global is created", () => {
    try { IsUUID("123e4567-e89b-12d3-a456-426614174000"); } catch (e) {}
    try { IsSemver("1.2.3"); } catch (e) {}
    assert(globalThis.__dyna_v === undefined, "no stray __dyna_v global is created");
});

__kitS.section("a planted bridge accessor cannot forge IsUUID", () => {
    const before = IsUUID("123e4567-e89b-12d3-a456-426614174000");
    Object.defineProperty(Object.prototype, "__dyna_vx_0_1", {
        get() { return () => true; }, configurable: true,
    });
    try {
        assert(IsUUID("not-a-uuid") === false, "a planted bridge accessor cannot forge IsUUID");
        assert(IsUUID("123e4567-e89b-12d3-a456-426614174000") === before, "IsUUID still accepts a valid uuid");
    } finally {
        delete Object.prototype.__dyna_vx_0_1;
    }
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
