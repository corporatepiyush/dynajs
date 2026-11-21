// covers: dyna:validate.IsJWT
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_jwt");
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

no(IsE164("1234567890123456"), "16 digits exceed the 15-digit cap");

__kitS.section("structural check unchanged without options", () => {
    const H = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    const C = "eyJzdWIiOiIxMjM0NTY3ODkwIn0";
    const TOK = H + "." + C + "." + "dGVzdHNpZ25hdHVyZQ";
    ok(IsJWT(TOK), "structural check unchanged without options");
    ok(IsJWT(TOK, { algs: ["HS256"] }), "alg in the allowlist passes");
    ok(IsJWT(TOK, { algs: ["ES256", "HS256"] }), "any allowlist entry matches");
    no(IsJWT(TOK, { algs: ["RS256"] }), "alg outside the allowlist is refused");
    no(IsJWT(TOK, { algs: [] }), "an empty allowlist refuses everything");
    no(IsJWT(TOK, { algs: "HS256" }), "a non-array allowlist refuses rather than skips");
    no(IsJWT(TOK, { algs: ["hs256"] }), "the alg match is exact, not case-folded");

    no(IsJWT("eyJhbGciOiJBIn0=." + C + "." + TOK.slice(TOK.lastIndexOf(".") + 1)),
       "a padded header is refused");
    no(IsJWT(H + ".eyJhIjoxfQ==." + "dGVzdHNpZ25hdHVyZQ"), "padded claims are refused");
    no(IsJWT(H + "." + C + ".ab=="), "a padded signature is refused");
    throws(() => IsJWT(TOK, { get algs() { throw new Error("options getter"); } }),
           "a throwing algs getter propagates");
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
