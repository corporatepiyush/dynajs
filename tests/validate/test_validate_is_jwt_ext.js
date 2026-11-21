// covers: dyna:validate.IsJWT
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_jwt_ext");
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

no(IsUUID(""), "empty input is false");
no(IsUUID("123e4567-e89b-12d3-a456-42661417400"), "35 chars are refused");
no(IsUUID("123e4567-e89b-12d3-a456-4266141740000"), "37 chars are refused");

const H = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

const C = "eyJzdWIiOiIxMjM0NTY3ODkwIn0";

const S = "dGVzdHNpZ25hdHVyZQ";

const TOK = H + "." + C + "." + S;

ok(IsJWT(TOK), "valid three-segment JWT");

__kitS.section("header decodes to the JWT header", () => {
    const d = Base64URLDecode(H);
    let s = "";
    for (const k in d) s += String.fromCharCode(d[k]);
    assert(s === '{"alg":"HS256","typ":"JWT"}', "header decodes to the JWT header");
});
__kitS.end();

no(IsJWT("aaa.bbb"), "two segments refused (two-dot guard, J1)");
no(IsJWT("aaa.bbb.ccc.ddd"), "four segments refused (third-dot guard, J1)");
no(IsJWT(H + ".." + S), "empty claims refused (non-empty guard, J2)");
no(IsJWT(H + "." + C + "."), "empty signature refused (non-empty guard, J2)");
no(IsJWT("A." + C + "." + S), "non-base64 header refused (J3)");
no(IsJWT("YWJj." + C + "." + S), "header that is not JSON refused (J4)");
no(IsJWT("e30." + C + "." + S), "header without alg refused (J5)");
no(IsJWT(H + ".A." + S), "non-base64 claims refused (J6)");
no(IsJWT(H + ".WzEsMl0." + S), "claims that are an array refused (J7)");
no(IsJWT(H + "." + C + ".%%%"), "non-base64 signature refused (J8)");
throws(() => IsJWT([]), "a non-string is a TypeError");

no(IsJWT(""), "empty input is false");

ok(IsSemver("1.2.3"), "valid semver");

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
