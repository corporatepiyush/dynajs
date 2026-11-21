// covers: dyna:validate.IsMimeType
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_mime_type");
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

__kitS.section("text/html; charset=utf-8", () => {
for (const a of ["a/b", "text/plain", "application/json",
                 "text/html;charset=utf-8", "text/html; charset=utf-8",
                 "application/vnd.api+json", "image/svg+xml",
                 "multipart/form-data; boundary=abc123",
                 'text/plain; charset="utf-8"',
                 'text/plain; a="say \\"hi\\""', "text/plain;a=1;b=2",
                 "text/plain ; charset=utf-8"])
    ok(IsMimeType(a), "valid mime type " + JSON.stringify(a));
});

__kitS.section("text plain", () => {
for (const a of ["", "text", "/plain", "text/", "text plain", "text/plain ",
                 "text/plain;", "text/plain; ", "text/plain; =v", "text/plain; a=",
                 'text/plain; a="unterminated', 'text/plain; a="x\\"',
                 "text/plain; a=b=c", "text/pl@in", "text/pl ain",
                 "text/plain;charset=utf-8;charset=ascii", "text/plain;a",
                 "text\tplain", "text/plain;a=\u0000x"])
    no(IsMimeType(a), "invalid mime type " + JSON.stringify(a));
ok(IsMimeType("TEXT/PLAIN"), "upper case type/subtype is fine");
});

__kitS.section("a parameter name repeated in another case is still refused", () => {
no(IsMimeType("text/plain;charset=utf-8;CHARSET=ascii"),
   "a parameter name repeated in another case is still refused");
ok(IsMimeType("a".repeat(127) + "/b"), "a 127-char type half is the limit");
});

no(IsMimeType("a".repeat(128) + "/b"), "a 128-char type half is over it");

__kitS.section("Ab1!Ab1!", () => {
for (const a of ["Str0ng!pass", "Abcdef1!", "Password1!", "xY3!xY3!",
                 "T3st!ng1", "Ab1!Ab1! "])
    ok(IsStrongPassword(a), "strong: " + JSON.stringify(a));
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
