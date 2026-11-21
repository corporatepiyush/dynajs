// covers: dyna:validate.IsStrongPassword
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_strong_password");
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

__kitS.section("a b c d e f 1", () => {
for (const a of ["", "Ab1!", "Ab1!Ab1", "abcdefgh", "ABCDEFGH", "Abcdefg1",
                 "Abcdefg!", "Password1", "aaaaaaaa1!", "AAAAAAAA1!",
                 "P@ssw\u00F6rd1"  , "a b c d e f 1"])
    no(IsStrongPassword(a), "weak: " + JSON.stringify(a));
ok(IsStrongPassword("Aa1!Aa1!  "), "a space is length, not a class");
});

__kitS.section("all-zero class minimums relax the classes", () => {
ok(IsStrongPassword("aaaa", { minLength: 4, minLower: 4, minUpper: 0,
                              minDigits: 0, minSymbols: 0 }),
   "all-zero class minimums relax the classes");
});

__kitS.section("8.0 is an integer for these purposes", () => {
ok(IsStrongPassword("Abcdef1!", { minLength: 8.0 }),
   "8.0 is an integer for these purposes");
});

__kitS.section("a partial options object only overrides what it names", () => {
ok(IsStrongPassword("aaaaaaaa", { minLower: 0, minUpper: 0, minDigits: 0,
                                  minSymbols: 0 }),
   "a partial options object only overrides what it names");
});

__kitS.section("minLength is honored", () => {
no(IsStrongPassword("aaaa", { minLength: 5, minLower: 0, minUpper: 0,
                              minDigits: 0, minSymbols: 0 }),
   "minLength is honored");
no(IsStrongPassword("abc", { minLower: 4, minLength: 3, minUpper: 0,
                             minDigits: 0, minSymbols: 0 }),
   "minLower is honored");
no(IsStrongPassword("abcdef1!", {}), "an empty options object keeps the defaults");
ok(IsStrongPassword("Abcdef1!", "not an object"), "a non-object opts is ignored");
});

__kitS.section("undefined opts keeps defaults", () => {
ok(IsStrongPassword("Abcdef1!", undefined), "undefined opts keeps defaults");
});

__kitS.section("every minimum at zero still accepts non-empty input", () => {
ok(IsStrongPassword("abc", { minLength: 0, minLower: 0, minUpper: 0,
                             minDigits: 0, minSymbols: 0 }),
   "every minimum at zero still accepts non-empty input");
});

__kitS.section("an empty string is never a strong password", () => {
no(IsStrongPassword("", { minLength: 0, minLower: 0, minUpper: 0,
                          minDigits: 0, minSymbols: 0 }),
   "an empty string is never a strong password");
throws(() => IsStrongPassword("Abcdef1!", { minLength: NaN }),
       "a NaN minLength is a RangeError");
});

__kitS.section("an Infinity minLength is a RangeError", () => {
throws(() => IsStrongPassword("Abcdef1!", { minLength: Infinity }),
       "an Infinity minLength is a RangeError");
});

__kitS.section("a string minLength is a TypeError", () => {
throws(() => IsStrongPassword("Abcdef1!", { minLength: "8" }),
       "a string minLength is a TypeError");
});

__kitS.section("a negative minLength is a RangeError", () => {
throws(() => IsStrongPassword("Abcdef1!", { minLength: -1 }),
       "a negative minLength is a RangeError");
});

__kitS.section("a fractional minLength is a RangeError", () => {
throws(() => IsStrongPassword("Abcdef1!", { minLength: 1.5 }),
       "a fractional minLength is a RangeError");
});

__kitS.section("an unsatisfiable minLength is a RangeError", () => {
throws(() => IsStrongPassword("Abcdef1!", { minLength: 5000 }),
       "an unsatisfiable minLength is a RangeError");
});

__kitS.section("a non-string text throws", () => {
throws(() => IsStrongPassword(null), "a non-string text throws");
});

__kitS.section("a throwing option getter propagates", () => {
throws(() => IsStrongPassword("Abcdef1!", { get minLength() { throw new Error("x"); } }),
       "a throwing option getter propagates");
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
