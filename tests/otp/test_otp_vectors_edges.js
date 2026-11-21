// covers: contract dyna:otp.HOTPGenerate dyna:otp.HOTPVerify dyna:otp.TOTPGenerate dyna:otp.TOTPVerify -- OTP checks moved out of four crypto suites
// The OTP checks that lived inside crypto suites before HOTP/TOTP left dyna:crypto (S15; owner rule: module splits are
// mirrored at test level, so no crypto suite imports dyna:otp). One section per former home:
// test_crypto_rsa_upgrade (RFC 4226/6238 vectors and verify edges), test_crypto_rsa (verify basics),
// test_crypto_jwt_sign_audit_w2_native_2 (D3-03: a missing or non-text secret is a TypeError) and
// test_crypto_argon2id_crypto_cov_bytes_compress_encoding_hash (vectors). Checks that meant "window 0" pass window: 0
// explicitly since TOTPVerify's window defaults to 1 step (owner 11-26).
import * as otp from "dyna:otp";
import { sections } from "../lib/kit.js";

const S = sections("test_otp_vectors_edges");
let n = 0, fails = 0;
function ok(cnd, msg) { n++; if (!cnd) { fails++; print("  FAIL " + msg); } }
function eq(a, b, msg) { ok(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throws(fn, re, msg) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e && e.message || e); }
    ok(t && (!re || re.test(m)), msg + (t && re && !re.test(m) ? " [wrong error: " + m + "]" : ""));
}
function errName(fn) { try { fn(); return "none"; } catch (e) { return e.name; } }
const rfcSecret = "12345678901234567890";
const rfc4226 = ["755224", "287082", "359152", "969429", "338314", "254676", "287922", "162583", "399871", "520489"];

S.section("RFC vectors and verify edges (from test_crypto_rsa_upgrade)", () => {
    for (let i = 0; i < 9; i++) {
        eq(otp.HOTPGenerate(rfcSecret, i), rfc4226[i], `RFC 4226 vector ${i} (generate)`);
        eq(otp.HOTPVerify(rfcSecret, i, rfc4226[i]), true, `RFC 4226 vector ${i} verifies`);
    }
    eq(otp.HOTPVerify(rfcSecret, 9, rfc4226[8]), false, "off-by-one counter is false");
    eq(otp.HOTPVerify(rfcSecret, 0, rfc4226[1]), false, "swapped code is false");
    eq(otp.HOTPVerify(rfcSecret, 0, "75522"), false, "short code is false");
    eq(otp.HOTPVerify(rfcSecret, 0, "7552244"), false, "long code is false");
    eq(otp.HOTPVerify(rfcSecret, 0, "75522a"), false, "non-digit code is false");
    eq(otp.HOTPVerify(rfcSecret, 0, ""), false, "empty code is false");
    const code8 = otp.HOTPGenerate(rfcSecret, 3, { digits: 8 });
    eq(code8.length, 8, "8-digit code");
    eq(otp.HOTPVerify(rfcSecret, 3, code8, { digits: 8 }), true, "8-digit verify");
    eq(otp.HOTPVerify(rfcSecret, 3, code8), false, "default 6-digit verify of an 8-digit code is false");
    eq(otp.TOTPGenerate(rfcSecret, { atSec: 59, algo: "sha1", digits: 8 }), "94287082", "RFC 6238 sha1 T59");
    const s32 = rfcSecret + "123456789012";
    eq(s32.length, 32, "sha256 seed length");
    eq(otp.TOTPGenerate(s32, { atSec: 59, algo: "sha256", digits: 8 }), "46119246", "RFC 6238 sha256 T59");
    const s64 = rfcSecret + rfcSecret + rfcSecret + "1234";
    eq(otp.TOTPGenerate(s64, { atSec: 59, algo: "sha512", digits: 8 }), "90693936", "RFC 6238 sha512 T59");
    eq(otp.TOTPVerify(s32, "46119246", { atSec: 59, algo: "sha256", digits: 8 }), true, "RFC 6238 sha256 verifies");
    eq(otp.TOTPVerify(s64, "90693936", { atSec: 59, algo: "sha512", digits: 8 }), true, "RFC 6238 sha512 verifies");
    const tc = otp.TOTPGenerate(rfcSecret, { atSec: 59 });
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 59 }), true, "TOTP exact");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 89, window: 0 }), false, "skew +1 period, window 0 -> false");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 29, window: 0 }), false, "skew -1 period, window 0 -> false");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 89 }), true, "skew +1 period, default window 1 -> true");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 29 }), true, "skew -1 period, default window 1 -> true");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 89, window: 1 }), true, "skew +1, window 1 -> true");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 29, window: 1 }), true, "skew -1, window 1 -> true");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 119, window: 1 }), false, "skew +2, window 1 -> false");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 119 }), false, "skew +2, default window -> false");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 59, window: 3 }), true, "window around exact is true");
    throws(() => otp.TOTPVerify(rfcSecret, tc, { atSec: 59, window: -1 }), /window >= 0/, "negative window refuses");
    throws(() => otp.TOTPVerify(rfcSecret, tc, { atSec: 59, period: 0 }), /period > 0/, "period 0 refuses");
    throws(() => otp.TOTPVerify(rfcSecret, tc, { atSec: -1 }), /atSec >= 0/, "negative atSec refuses");
    throws(() => otp.TOTPVerify(rfcSecret, tc, { algo: "md5" }), /sha1, sha256 or sha512/, "bad algo refuses");
    throws(() => otp.HOTPVerify(rfcSecret, -1, "123456"), /negative/, "negative counter refuses");
    eq(otp.HOTPGenerate(rfcSecret, 0), "755224", "HOTPGenerate untouched (RFC 4226 D)");
});

S.section("verify basics (from test_crypto_rsa)", () => {
    eq(otp.HOTPVerify(rfcSecret, 5, rfc4226[5]), true, "HOTPVerify accepts the RFC vector");
    eq(otp.HOTPVerify(rfcSecret, 6, rfc4226[5]), false, "HOTPVerify rejects a foreign counter");
    eq(otp.HOTPVerify(rfcSecret, 5, "75522a"), false, "HOTPVerify: malformed code is false");
    const tc = otp.TOTPGenerate(rfcSecret, { atSec: 59 });
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 59 }), true, "TOTPVerify exact");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 89, window: 1 }), true, "TOTPVerify window 1");
    eq(otp.TOTPVerify(rfcSecret, tc, { atSec: 89, window: 0 }), false, "TOTPVerify beyond window 0");
});

S.section("D3-03: a secret that is not text or bytes is a TypeError (from test_crypto_jwt_sign_audit_w2_native_2)", () => {
    for (const [label, bad] of [["undefined", undefined], ["null", null], ["number", 5], ["object", {}], ["boolean", true]]) {
        eq(errName(() => otp.TOTPGenerate(bad)), "TypeError", "TOTPGenerate refuses a " + label + " secret");
        eq(errName(() => otp.HOTPGenerate(bad, 0)), "TypeError", "HOTPGenerate refuses a " + label + " secret");
    }
});

S.section("vectors (from test_crypto_argon2id_crypto_cov_bytes_compress_encoding_hash)", () => {
    eq(otp.HOTPGenerate(rfcSecret, 0, { digits: 6 }), "755224", "HOTP RFC4226 c=0 vector");
    eq(otp.HOTPGenerate(rfcSecret, 9, { digits: 6 }), "520489", "HOTP RFC4226 c=9 vector");
    eq(otp.TOTPGenerate(rfcSecret, { atSec: 59, digits: 8 }), "94287082", "TOTP RFC6238 T=59 vector");
    eq(otp.TOTPGenerate(rfcSecret, { atSec: 1111111109, digits: 8 }), "07081804", "TOTP RFC6238 T=1.11e9 vector");
    throws(() => otp.HOTPGenerate("secret", -1), /./, "HOTP negative counter refused");
    eq(otp.TOTPGenerate("abc", { atSec: 1234567 }), otp.TOTPGenerate("abc", { atSec: 1234567 }), "TOTP deterministic at fixed sec");
});

S.end();
if (fails) throw new Error("test_otp_vectors_edges: " + fails + " FAILED of " + n + " assertions");
print("test_otp_vectors_edges: " + n + " assertions, 0 failures");
