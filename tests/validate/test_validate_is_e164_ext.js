// covers: dyna:validate.IsE164
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_e164_ext");
import { IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164 }
    from "dyna:validate";
import { Base64URLDecode } from "dyna:encoding";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function ok(v, msg) { assert(v === true, msg + " (expected true)"); }
function no(v, msg) { assert(v === false, msg + " (expected false)"); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

__kitS.section("valid E.164 without", () => {
ok(IsE164("14155552671"), "valid E.164 without +");
});

__kitS.section("16 digits exceed the E.164 maximum of 15", () => {
no(IsE164("1234567890123456"), "16 digits exceed the E.164 maximum of 15");
no(IsE164("+1415555267a"), "a letter is refused");
no(IsE164("123 456"), "a space is refused");
no(IsE164("123-456"), "a hyphen is refused");
throws(() => IsE164(null), "a non-string is a TypeError");
});

__kitS.section("empty input is false", () => {
no(IsE164(""), "empty input is false");
no(IsE164("+"), "a lone + is refused");
ok(IsE164("123456789012345"), "15 digits are the limit");
});

__kitS.section("14 digits are fine", () => {
ok(IsE164("12345678901234"), "14 digits are fine");
});
__kitS.end();

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
