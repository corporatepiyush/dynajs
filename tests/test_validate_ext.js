/* test_validate_ext.js -- the plan 3.15 validators in dyna:validate (row 76).
 *
 * Each refusal below names the ONE underlying check it proves, so deleting
 * that check makes the test redden (the two-guards rule). The vectors were
 * verified against the live binary; spec bounds are cited beside each.
 *
 * Thinness note: IsURL/IsDomain/IsUUID/IsSemver call the exporting module's
 * JS entry points (the C parsers are static in their own files). IsJWT is
 * structural over the same base64url decoder dyna-crypto uses + the engine's
 * JSON parser, because dyna:crypto exposes no decode-without-verify.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_validate_ext.js
 */
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

/* ------------------------------------------------------------------ URL */

ok(IsURL("https://example.com/path?q=1#f"), "valid: absolute http URL");
ok(IsURL("http://192.168.0.1/x"), "valid: IP-literal URL");
ok(IsURL("mailto:a@b.co"), "valid: non-special scheme");
/* Proves the URL-parse rejection in dyna:url (dyn_url_parse failure -> the
   ctor throws -> false). Delete that check and this string is accepted. */
no(IsURL("example.com/path?q=1#f"), "no scheme is refused");
/* Proves the empty-host policy on the value the ctor RETURNED (the parser
   itself accepts "https://"). Delete the .hostname check and it passes. */
no(IsURL("https://"), "special scheme with an empty host is refused");
throws(() => IsURL(42), "a non-string is a TypeError");
no(IsURL(""), "empty input is false");

/* --------------------------------------------------------------- domain */

ok(IsDomain("example.com"), "valid domain");
ok(IsDomain("sub.example.co.uk"), "valid multi-label domain");
ok(IsDomain("xn--bcher-kva.ch"), "valid punycode domain");
/* Each refusal names its guard inside dyn_v_domain_grammar / the netip call. */
no(IsDomain("-example.com"), "leading hyphen refused (label-edge guard)");
no(IsDomain("example-.com"), "trailing hyphen refused (label-edge guard)");
no(IsDomain("example..com"), "empty label refused (label guard)");
no(IsDomain("exa_mple.com"), "underscore refused (charset guard)");
no(IsDomain("localhost"), "no dot: a bare host, not a domain (dot guard)");
/* Proves the net.isValid (dyna:net) call: the label grammar ACCEPTS an IPv4
   literal. Delete that call and 127.0.0.1 passes. */
no(IsDomain("127.0.0.1"), "an IP literal is not a domain (netip guard)");
throws(() => IsDomain({}), "a non-string is a TypeError");
no(IsDomain(""), "empty input is false");
/* RFC 1035 label bound (63) and name bound (253), at both edges. */
ok(IsDomain("a".repeat(63) + ".com"), "a 63-char label is the limit");
no(IsDomain("a".repeat(64) + ".com"), "64-char label is refused");
{
    const max253 = "a".repeat(61) + "." + "b".repeat(63) + "." +
                   "c".repeat(63) + "." + "d".repeat(63);
    assert(max253.length === 253, "the 253-char vector really is 253");
    ok(IsDomain(max253), "a 253-char domain is the limit (RFC 1035)");
    no(IsDomain(max253 + "e"), "254 chars are refused");
}

/* ------------------------------------------------------------------ slug */

ok(IsSlug("hello-world"), "valid slug");
ok(IsSlug("a"), "single char is a slug");
ok(IsSlug("x-2-y"), "digits are allowed inside a slug");
/* Four distinct guards in dyn_v_slug; each test proves its own return. */
no(IsSlug("-hello-world"), "leading hyphen refused (guard 1)");
no(IsSlug("hello-world-"), "trailing hyphen refused (guard 2)");
no(IsSlug("hello--world"), "double hyphen refused (guard 3)");
no(IsSlug("Hello-world"), "uppercase refused (charset guard)");
no(IsSlug("hello_world"), "underscore refused (charset guard)");
throws(() => IsSlug(9), "a non-string is a TypeError");
no(IsSlug(""), "empty input is false");
/* The 64-char bound, at both edges. */
ok(IsSlug("a".repeat(64)), "a 64-char slug is the limit");
no(IsSlug("a".repeat(65)), "65 chars are refused");

/* ------------------------------------------------------------------ UUID */

/* RFC 4122 sec.3 canonical vector. */
ok(IsUUID("123e4567-e89b-12d3-a456-426614174000"), "valid canonical UUID");
/* Proves the 36-char canonical gate: uuid.validate ACCEPTS urn:/braced/raw
   forms, the gate refuses them. Delete the gate and this passes. */
no(IsUUID("urn:uuid:123e4567-e89b-12d3-a456-426614174000"),
   "urn:uuid: form is refused (canonical-36 gate)");
no(IsUUID("123e4567e89b12d3a456426614174000"), "raw hex is refused (gate)");
/* Proves the hex-pair check in dyna-uuid.c (dyn_uuid_parse_canonical). Delete
   it and this 36-char string passes. */
no(IsUUID("123e4567-e89b-12d3-a456-42661417400z"), "a non-hex char is refused");
throws(() => IsUUID(true), "a non-string is a TypeError");
no(IsUUID(""), "empty input is false");
/* RFC 4122 length boundary: 35/36/37. */
no(IsUUID("123e4567-e89b-12d3-a456-42661417400"), "35 chars are refused");
no(IsUUID("123e4567-e89b-12d3-a456-4266141740000"), "37 chars are refused");

/* ------------------------------------------------------------------- JWT */

/* RFC 7515 sec.3.1 vector: BASE64URL(hdr).BASE64URL(claims).BASE64URL(sig).
   Its segments decode to {"alg":"HS256","typ":"JWT"} / {"sub":"1234567890"}. */
const H = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
const C = "eyJzdWIiOiIxMjM0NTY3ODkwIn0";
const S = "dGVzdHNpZ25hdHVyZQ";
const TOK = H + "." + C + "." + S;
ok(IsJWT(TOK), "valid three-segment JWT");
/* The positive vector's header is proven to say what it must. */
{
    const d = Base64URLDecode(H);
    let s = "";
    for (const k in d) s += String.fromCharCode(d[k]);
    assert(s === '{"alg":"HS256","typ":"JWT"}', "header decodes to the JWT header");
}
/* Every refusal maps to a distinct check in dyn_v_jwt (delete it -> accepted). */
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

/* ---------------------------------------------------------------- semver */

ok(IsSemver("1.2.3"), "valid semver");
ok(IsSemver("1.2.3-rc.1+build.2"), "prerelease and build are allowed");
/* Proves semver_parse's three-part requirement in dyna-semver.c. Delete it and
   "1.2" passes. */
no(IsSemver("1.2"), "missing patch is refused");
no(IsSemver("v1.2.3"), "a v prefix is refused");
no(IsSemver("01.2.3"), "a leading zero is refused");
no(IsSemver("1.2.3-"), "an empty prerelease is refused");
throws(() => IsSemver({}), "a non-string is a TypeError");
no(IsSemver(""), "empty input is false");

/* ------------------------------------------------------------------ E164 */

ok(IsE164("+14155552671"), "valid E.164 with +");
ok(IsE164("14155552671"), "valid E.164 without +");
/* Proves the 15-digit cap in dyn_v_e164 (ITU-T E.164 sec.3.1). Delete it and
   the 16-digit number passes. */
no(IsE164("1234567890123456"), "16 digits exceed the E.164 maximum of 15");
/* Proves the digit check in dyn_v_e164. Delete it and the letter passes. */
no(IsE164("+1415555267a"), "a letter is refused");
no(IsE164("123 456"), "a space is refused");
no(IsE164("123-456"), "a hyphen is refused");
throws(() => IsE164(null), "a non-string is a TypeError");
no(IsE164(""), "empty input is false");
no(IsE164("+"), "a lone + is refused");
/* The 15-digit boundary, both edges. */
ok(IsE164("123456789012345"), "15 digits are the limit");
ok(IsE164("12345678901234"), "14 digits are fine");

/* ------------------------------------------------- shared length guard */

/* DYN_V_MAX is 4096; at and below it the string is handed to the validator
   (returns false), one over is a RangeError. Same guard for every validator. */
for (const fn of [IsURL, IsDomain, IsSlug, IsUUID, IsJWT, IsSemver, IsE164]) {
    no(fn("a".repeat(4095)), "4095 bytes is not over the cap");
    no(fn("a".repeat(4096)), "4096 bytes is exactly the cap");
    throws(() => fn("a".repeat(4097)), "4097 bytes is a RangeError");
}

if (fails) {
    print("test_validate_ext: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate_ext failed");
}
print("test_validate_ext: " + n + " assertions, 0 failures");
