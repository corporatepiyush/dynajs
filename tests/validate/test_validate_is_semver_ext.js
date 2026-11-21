// covers: dyna:validate.IsSemver
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_semver_ext");
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

__kitS.section("prerelease and build are allowed", () => {
ok(IsSemver("1.2.3-rc.1+build.2"), "prerelease and build are allowed");
});

__kitS.section("missing patch is refused", () => {
no(IsSemver("1.2"), "missing patch is refused");
ok(IsSemver("v1.2.3"), "a v prefix is allowed (parse contract)");
});

__kitS.section("a leading zero is refused", () => {
no(IsSemver("01.2.3"), "a leading zero is refused");
no(IsSemver("1.2.3-"), "an empty prerelease is refused");
throws(() => IsSemver({}), "a non-string is a TypeError");
});

no(IsSemver(""), "empty input is false");

__kitS.section("valid E.164 with", () => {
ok(IsE164("+14155552671"), "valid E.164 with +");
});
__kitS.end();

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
