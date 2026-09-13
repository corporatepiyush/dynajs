/* HOTP/TOTP against the RFC's OWN published vectors. A wrong implementation
   returns six plausible digits; only a published vector separates them. */
import { HOTPGenerate, TOTPGenerate } from "dyna:crypto";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };
const enc = (s) => new TextEncoder().encode(s);
const SECRET = enc("12345678901234567890");          /* RFC 4226 Appendix D */

/* RFC 4226 Appendix D: counters 0..9 */
const HOTP = ["755224","287082","359152","969429","338314",
              "254676","287922","162583","399871","520489"];
HOTP.forEach((want, c) => {
  const got = HOTPGenerate(SECRET, c);
  ok(got === want, "RFC4226 counter " + c + " -> " + got + " want " + want);
});

/* RFC 6238 Appendix B: SHA-1, 8 digits, 30s period, fixed times */
const T = [[59,"94287082"],[1111111109,"07081804"],[1111111111,"14050471"],
           [1234567890,"89005924"],[2000000000,"69279037"],[20000000000,"65353130"]];
for (const [at, want] of T) {
  const got = TOTPGenerate(SECRET, { atSec: at, digits: 8, algo: "sha1" });
  ok(got === want, "RFC6238 t=" + at + " -> " + got + " want " + want);
}

/* RFC 6238 Appendix B: SHA-256 (32-byte ASCII secret) and SHA-512 (64-byte
   ASCII secret) columns. Only sha1|sha256|sha512 are defined for OTP, so
   these two columns plus the SHA-1 table above are the WHOLE legal algo set. */
const SECRET32 = enc("12345678901234567890123456789012");
const SECRET64 = enc("1234567890123456789012345678901234567890123456789012345678901234");
const T256 = [[59,"46119246"],[1111111109,"68084774"],[1111111111,"67062674"],
              [1234567890,"91819424"],[2000000000,"90698825"],[20000000000,"77737706"]];
for (const [at, want] of T256) {
  const got = TOTPGenerate(SECRET32, { atSec: at, digits: 8, algo: "sha256" });
  ok(got === want, "RFC6238 sha256 t=" + at + " -> " + got + " want " + want);
}
const T512 = [[59,"90693936"],[1111111109,"25091201"],[1111111111,"99943326"],
              [1234567890,"93441116"],[2000000000,"38618901"],[20000000000,"47863826"]];
for (const [at, want] of T512) {
  const got = TOTPGenerate(SECRET64, { atSec: at, digits: 8, algo: "sha512" });
  ok(got === want, "RFC6238 sha512 t=" + at + " -> " + got + " want " + want);
}

/* the period actually divides: two times in the same window agree */
ok(TOTPGenerate(SECRET, {atSec: 60, digits: 8}) ===
   TOTPGenerate(SECRET, {atSec: 89, digits: 8}), "same 30s window agrees");
ok(TOTPGenerate(SECRET, {atSec: 59, digits: 8}) !==
   TOTPGenerate(SECRET, {atSec: 60, digits: 8}), "window boundary differs");

/* refusals. md5 and every non-RFC digest MUST throw: HMAC wrote only
   digest_size bytes, so a short digest made the truncation read bytes that
   were never initialized (the reason the algo set is restricted). */
let threw = 0;
for (const bad2 of [() => HOTPGenerate(SECRET, 0, {digits: 4}),
                    () => HOTPGenerate(SECRET, -1),
                    () => TOTPGenerate(SECRET, {atSec: 0, period: 0}),
                    () => TOTPGenerate(SECRET, {atSec: 0, algo: "nope"}),
                    () => HOTPGenerate(SECRET, 0, {algo: "md5"}),
                    () => TOTPGenerate(SECRET, {atSec: 0, algo: "md5"}),
                    () => HOTPGenerate(SECRET, 0, {algo: "sha384"}),
                    () => TOTPGenerate(SECRET, {atSec: 0, algo: "sha224"})])
  { try { bad2(); } catch (e) { threw++; } }
ok(threw === 8, "refuses digits<6, negative counter, period 0, unknown algo, " +
                "and every non-RFC digest (md5/sha384/sha224): " + threw);

/* the refusal is the DOCUMENTED one, not an accident of arg parsing */
try { HOTPGenerate(SECRET, 0, {algo: "md5"}); ok(false, "md5 must throw"); }
catch (e) { ok(/sha1, sha256 or sha512/.test(e.message), "md5 error names the legal set: " + e.message); }

print("test_crypto_otp: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
