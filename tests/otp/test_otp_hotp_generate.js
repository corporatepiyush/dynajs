// covers: dyna:otp.HOTPGenerate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_otp_hotp_generate");
import { HOTPGenerate, TOTPGenerate } from "dyna:otp";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };
const enc = (s) => new TextEncoder().encode(s);
const SECRET = enc("12345678901234567890");

const HOTP = ["755224","287082","359152","969429","338314",
              "254676","287922","162583","399871","520489"];

__kitS.section("RFC4226 counter", () => {
HOTP.forEach((want, c) => {
  const got = HOTPGenerate(SECRET, c);
  ok(got === want, "RFC4226 counter " + c + " -> " + got + " want " + want);
});
});

let threw = 0;

__kitS.section("refuses digits<6, negative counter, period 0, unknown algo,", () => {
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
});

__kitS.section("md5 must throw", () => {
try { HOTPGenerate(SECRET, 0, {algo: "md5"}); ok(false, "md5 must throw"); }
catch (e) { ok(/sha1, sha256 or sha512/.test(e.message), "md5 error names the legal set: " + e.message); }
});
__kitS.end();

print("test_crypto_otp: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
