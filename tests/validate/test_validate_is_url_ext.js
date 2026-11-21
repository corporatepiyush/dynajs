// covers: dyna:validate.IsURL
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_url_ext");
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

__kitS.section("valid: absolute http URL", () => {
ok(IsURL("https://example.com/path?q=1#f"), "valid: absolute http URL");
});

__kitS.section("valid: IP-literal URL", () => {
ok(IsURL("http://192.168.0.1/x"), "valid: IP-literal URL");
});

__kitS.section("valid: non-special scheme", () => {
ok(IsURL("mailto:a@b.co"), "valid: non-special scheme");
});

__kitS.section("no scheme is refused", () => {
no(IsURL("example.com/path?q=1#f"), "no scheme is refused");
no(IsURL("https://"), "special scheme with an empty host is refused");
throws(() => IsURL(42), "a non-string is a TypeError");
});

no(IsURL(""), "empty input is false");

__kitS.section("valid domain", () => {
ok(IsDomain("example.com"), "valid domain");
});

__kitS.section("4095 bytes is not over the cap", () => {
for (const fn of [IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164]) {
    no(fn("a".repeat(4095)), "4095 bytes is not over the cap");
    no(fn("a".repeat(4096)), "4096 bytes is exactly the cap");
    throws(() => fn("a".repeat(4097)), "4097 bytes is a RangeError");
}
});
__kitS.end();

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
