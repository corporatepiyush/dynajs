// covers: dyna:validate.IsUUID
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_uuid_ext");
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

__kitS.section("urn:uuid: form is refused (canonical-36 gate)", () => {
no(IsUUID("urn:uuid:123e4567-e89b-12d3-a456-426614174000"),
   "urn:uuid: form is refused (canonical-36 gate)");
no(IsUUID("123e4567e89b12d3a456426614174000"), "raw hex is refused (gate)");
no(IsUUID("123e4567-e89b-12d3-a456-42661417400z"), "a non-hex char is refused");
throws(() => IsUUID(true), "a non-string is a TypeError");
});
__kitS.end();

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
