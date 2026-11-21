// covers: dyna:otp.HOTPVerify
// C3b: HOTPVerify's bag is {digits?, algo?}. Before C3b its option reader also read "period" (a TOTP key) through a NULL
// out-pointer, so HOTPVerify(s, c, code, {period: 30}) -- or a bag inheriting period from its prototype -- crashed the
// process (SIGSEGV). Unknown own keys are now a TypeError naming the valid set, and period is never read for HOTP.
import { suite } from "../lib/kit.js";
import { HOTPGenerate, HOTPVerify, TOTPGenerate, TOTPVerify } from "dyna:otp";

const t = suite("test_otp_hotp_verify_opts");
const SECRET = new TextEncoder().encode("12345678901234567890");

t.test("valid bags verify", (a) => {
    a.eq(HOTPVerify(SECRET, 0, "755224"), true, "RFC 4226 counter 0");
    a.eq(HOTPVerify(SECRET, 0, "755224", {}), true, "empty bag");
    a.eq(HOTPVerify(SECRET, 1, HOTPGenerate(SECRET, 1, { digits: 8, algo: "sha256" }), { digits: 8, algo: "sha256" }), true, "digits + algo");
    a.eq(HOTPVerify(SECRET, 0, "000000", { digits: 6 }), false, "wrong code");
});

t.test("a TOTP-only key is refused, never read", (a) => {
    a.throws(() => HOTPVerify(SECRET, 0, "755224", { period: 30 }), { type: TypeError, message: 'unknown option "period" (valid: digits, algo)' }, "own period");
    a.throws(() => HOTPVerify(SECRET, 0, "755224", { window: 1 }), { type: TypeError, message: 'unknown option "window"' }, "own window");
    a.eq(HOTPVerify(SECRET, 0, "755224", Object.create({ period: 30 })), true, "inherited period is ignored (no crash)");
    a.eq(HOTPVerify(SECRET, 0, "755224", Object.create({ period: "x", atSec: {} })), true, "inherited junk is ignored");
});

t.test("each OTP entry point is strict", (a) => {
    a.throws(() => HOTPGenerate(SECRET, 0, { __bogus: 1 }), { type: TypeError, message: 'unknown option "__bogus" (valid: digits, algo)' }, "HOTPGenerate");
    a.throws(() => TOTPGenerate(SECRET, { __bogus: 1 }), { type: TypeError, message: "(valid: atSec, period, digits, algo)" }, "TOTPGenerate");
    a.throws(() => TOTPVerify(SECRET, "000000", { __bogus: 1 }), { type: TypeError, message: "(valid: atSec, period, digits, algo, window)" }, "TOTPVerify");
    a.throws(() => TOTPGenerate(SECRET, { window: 1 }), { type: TypeError, message: 'unknown option "window"' }, "window is TOTPVerify-only");
    const code = TOTPGenerate(SECRET, { atSec: 59, period: 30, digits: 8, algo: "sha1" });
    a.eq(TOTPVerify(SECRET, code, { atSec: 59, period: 30, digits: 8, algo: "sha1", window: 1 }), true, "every documented key accepted");
});

await t.run();
