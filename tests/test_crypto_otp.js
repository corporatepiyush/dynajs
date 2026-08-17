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

/* the period actually divides: two times in the same window agree */
ok(TOTPGenerate(SECRET, {atSec: 60, digits: 8}) ===
   TOTPGenerate(SECRET, {atSec: 89, digits: 8}), "same 30s window agrees");
ok(TOTPGenerate(SECRET, {atSec: 59, digits: 8}) !==
   TOTPGenerate(SECRET, {atSec: 60, digits: 8}), "window boundary differs");

/* refusals */
let threw = 0;
for (const bad2 of [() => HOTPGenerate(SECRET, 0, {digits: 4}),
                    () => HOTPGenerate(SECRET, -1),
                    () => TOTPGenerate(SECRET, {atSec: 0, period: 0}),
                    () => TOTPGenerate(SECRET, {atSec: 0, algo: "nope"})])
  { try { bad2(); } catch (e) { threw++; } }
ok(threw === 4, "refuses digits<6, negative counter, period 0, unknown algo: " + threw);

print("test_crypto_otp: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
