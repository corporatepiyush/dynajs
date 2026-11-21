// covers: dyna:validate.IsDomain
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_domain_ext");
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

ok(IsDomain("sub.example.co.uk"), "valid multi-label domain");

ok(IsDomain("xn--bcher-kva.ch"), "valid punycode domain");

no(IsDomain("-example.com"), "leading hyphen refused (label-edge guard)");
no(IsDomain("example-.com"), "trailing hyphen refused (label-edge guard)");
no(IsDomain("example..com"), "empty label refused (label guard)");
no(IsDomain("exa_mple.com"), "underscore refused (charset guard)");
no(IsDomain("localhost"), "no dot: a bare host, not a domain (dot guard)");
no(IsDomain("127.0.0.1"), "an IP literal is not a domain (netip guard)");
throws(() => IsDomain({}), "a non-string is a TypeError");

no(IsDomain(""), "empty input is false");
ok(IsDomain("a".repeat(63) + ".com"), "a 63-char label is the limit");

no(IsDomain("a".repeat(64) + ".com"), "64-char label is refused");
__kitS.section("the 253-char vector really is 253", () => {
    const max253 = "a".repeat(61) + "." + "b".repeat(63) + "." +
                   "c".repeat(63) + "." + "d".repeat(63);
    assert(max253.length === 253, "the 253-char vector really is 253");
    ok(IsDomain(max253), "a 253-char domain is the limit (RFC 1035)");
    no(IsDomain(max253 + "e"), "254 chars are refused");
});
__kitS.end();

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
