// covers: dyna:validate.IsSlug
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_slug_ext");
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

__kitS.section("valid slug", () => {
ok(IsSlug("hello-world"), "valid slug");
});

__kitS.section("single char is a slug", () => {
ok(IsSlug("a"), "single char is a slug");
});

__kitS.section("digits are allowed inside a slug", () => {
ok(IsSlug("x-2-y"), "digits are allowed inside a slug");
});

__kitS.section("leading hyphen refused (guard 1)", () => {
no(IsSlug("-hello-world"), "leading hyphen refused (guard 1)");
no(IsSlug("hello-world-"), "trailing hyphen refused (guard 2)");
no(IsSlug("hello--world"), "double hyphen refused (guard 3)");
no(IsSlug("Hello-world"), "uppercase refused (charset guard)");
no(IsSlug("hello_world"), "underscore refused (charset guard)");
throws(() => IsSlug(9), "a non-string is a TypeError");
});

__kitS.section("empty input is false", () => {
no(IsSlug(""), "empty input is false");
ok(IsSlug("a".repeat(64)), "a 64-char slug is the limit");
});

no(IsSlug("a".repeat(65)), "65 chars are refused");

__kitS.section("valid canonical UUID", () => {
ok(IsUUID("123e4567-e89b-12d3-a456-426614174000"), "valid canonical UUID");
});
__kitS.end();

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
