import { HOTPGenerate, TOTPGenerate } from "dyna:crypto";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };
const enc = (s) => new TextEncoder().encode(s);
const SECRET = enc("12345678901234567890");

const HOTP = ["755224","287082","359152","969429","338314",
              "254676","287922","162583","399871","520489"];
HOTP.forEach((want, c) => {
  const got = HOTPGenerate(SECRET, c);
  ok(got === want, "RFC4226 counter " + c + " -> " + got + " want " + want);
});

const T = [[59,"94287082"],[1111111109,"07081804"],[1111111111,"14050471"],
           [1234567890,"89005924"],[2000000000,"69279037"],[20000000000,"65353130"]];
for (const [at, want] of T) {
  const got = TOTPGenerate(SECRET, { atSec: at, digits: 8, algo: "sha1" });
  ok(got === want, "RFC6238 t=" + at + " -> " + got + " want " + want);
}

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

ok(TOTPGenerate(SECRET, {atSec: 60, digits: 8}) ===
   TOTPGenerate(SECRET, {atSec: 89, digits: 8}), "same 30s window agrees");
ok(TOTPGenerate(SECRET, {atSec: 59, digits: 8}) !==
   TOTPGenerate(SECRET, {atSec: 60, digits: 8}), "window boundary differs");

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

try { HOTPGenerate(SECRET, 0, {algo: "md5"}); ok(false, "md5 must throw"); }
catch (e) { ok(/sha1, sha256 or sha512/.test(e.message), "md5 error names the legal set: " + e.message); }


{
  const boom = { valueOf() { throw new RangeError("boom"); } };
  let caught = null;
  try { TOTPGenerate(SECRET, { atSec: boom, digits: 8 }); } catch (e) { caught = e; }
  ok(caught instanceof RangeError, "TOTP atSec getter throw propagates");
  caught = null;
  try { TOTPGenerate(SECRET, { period: boom }); } catch (e) { caught = e; }
  ok(caught instanceof RangeError, "TOTP period getter throw propagates");
  caught = null;
  try { TOTPGenerate(SECRET, { digits: boom }); } catch (e) { caught = e; }
  ok(caught instanceof RangeError, "TOTP digits getter throw propagates");
  caught = null;
  try { HOTPGenerate(SECRET, 0, { digits: boom }); } catch (e) { caught = e; }
  ok(caught instanceof RangeError, "HOTP digits getter throw propagates");
  caught = null;
  try { TOTPGenerate(SECRET, { algo: { toString() { throw new TypeError("late"); } } }); } catch (e) { caught = e; }
  ok(caught instanceof TypeError, "TOTP algo getter throw propagates");
  ok(TOTPGenerate(SECRET, { atSec: 59, digits: 8 }) === "94287082", "TOTP sane after getter throws");
}

print("test_crypto_otp: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
