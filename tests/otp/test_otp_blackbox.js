// covers: blackbox dyna:otp.HOTPGenerate dyna:otp.HOTPVerify dyna:otp.TOTPGenerate dyna:otp.TOTPVerify -- black-box contract table moved from bb_crypto TABLE 20
// Black-box contract table for dyna:otp, moved out of tests/blackbox/bb_crypto.js TABLE 20 when HOTP/TOTP left dyna:crypto
// (S15; owner rule: module splits are mirrored at test level, so no crypto suite imports dyna:otp). Rows are
// [label, args, expected] driven through one loop per table, every failure names its row. Vectors: RFC 4226 App. D
// Table 1 and RFC 6238 App. B. The rows that pinned "window 0" pass window: 0 explicitly since TOTPVerify's window
// defaults to 1 step (owner 11-26); one row pins that default.
import { HOTPGenerate, TOTPGenerate, HOTPVerify, TOTPVerify } from "dyna:otp";
import { sections } from "../lib/kit.js";

const S = sections("test_otp_blackbox");
let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; if (!Object.is(actual, expected)) throw new Error("assertion failed: " + msg + " (got " + String(actual) + ", want " + String(expected) + ")"); }
function assertThrows(fn, msg) { n++; try { fn(); } catch (e) { return; } throw new Error("expected throw: " + msg); }
const THROWS = () => ({ __throws: true });
function runs(table, fn) {
    for (const [label, args, expected] of table) {
        const call = fn ? () => fn(...args) : args;
        if (expected && expected.__throws) assertThrows(call, label);
        else assertEq(call(), expected, label);
    }
}
const utf8 = (s) => new TextEncoder().encode(s);
const SEC = "12345678901234567890";
const RFC4226 = ["755224", "287082", "359152", "969429", "338314", "254676", "287922", "162583", "399871", "520489"];

S.section("HOTP RFC 4226 Table 1", () => {
    for (let c = 0; c < RFC4226.length; c++) {
        assertEq(HOTPGenerate(SEC, c), RFC4226[c], "HOTP counter " + c + " — RFC 4226 Table 1");
        assert(HOTPVerify(SEC, c, RFC4226[c]), "HOTPVerify accepts counter " + c + " — RFC 4226");
    }
});

S.section("TOTP RFC 6238 App. B", () => {
    runs([
        ["TOTP T=59 — RFC 6238 (SHA-1 seed, 8 digits)", [{ atSec: 59, digits: 8 }], "94287082"],
        ["TOTP T=1111111109 — RFC 6238", [{ atSec: 1111111109, digits: 8 }], "07081804"],
        ["TOTP T=1234567890 — RFC 6238", [{ atSec: 1234567890, digits: 8 }], "89005924"],
        ["TOTP T=20000000000 — RFC 6238", [{ atSec: 20000000000, digits: 8 }], "65353130"],
    ], (o) => TOTPGenerate(SEC, o));
});

S.section("behaviour rows", () => {
    runs([
        ["HOTPVerify rejects the wrong counter", [() => HOTPVerify(SEC, 0, RFC4226[1])], false],
        ["malformed (non-digit) code is false, never a throw", [() => HOTPVerify(SEC, 0, "12ab34")], false],
        ["wrong-length code is false", [() => HOTPVerify(SEC, 0, "12345")], false],
        ["empty code is false", [() => HOTPVerify(SEC, 0, "")], false],
        ["negative counter refused", [() => HOTPVerify(SEC, -1, "755224")], THROWS()],
        ["digits below 6 refused", [() => HOTPGenerate(SEC, 0, { digits: 5 })], THROWS()],
        ["digits above 8 refused", [() => HOTPGenerate(SEC, 0, { digits: 9 })], THROWS()],
        ["digits option sizes the code", [() => HOTPGenerate(SEC, 0, { digits: 8 }).length], 8],
        ["secret accepts string or UTF-8 bytes (parity)", [() => HOTPGenerate(utf8(SEC), 0) === HOTPGenerate(SEC, 0)], true],
        ["counter accepts a bigint (dynajs.d.ts)", [() => HOTPGenerate(SEC, 0n) === HOTPGenerate(SEC, 0)], true],
        ["default digits is 6", [() => HOTPGenerate(SEC, 0).length], 6],
        ["atSec >= 0 enforced", [() => TOTPGenerate(SEC, { atSec: -1 })], THROWS()],
        ["period > 0 enforced", [() => TOTPGenerate(SEC, { atSec: 0, period: 0 })], THROWS()],
        ["counter steps by period (30 and 31 are one counter)", [() => TOTPGenerate(SEC, { atSec: 30 }) === TOTPGenerate(SEC, { atSec: 31 })], true],
        ["default period 30, default digits 6", [() => TOTPGenerate(SEC, { atSec: 45 }).length], 6],
        ["TOTPVerify at the exact time", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1000 })], true],
        ["one period later with window 0 -> false", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1030, window: 0 })], false],
        ["default window 1 accepts the neighbouring period", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1030 })], true],
        ["two periods later with the default window -> false", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1060 })], false],
        ["window 1 accepts the neighbouring period", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1030, window: 1 })], true],
        ["window includes the exact period too", [() => TOTPVerify("secret", TOTPGenerate("secret", { atSec: 1000 }), { atSec: 1000, window: 1 })], true],
        ["garbage code is false, never a throw", [() => TOTPVerify("secret", "12ab", { atSec: 1000 })], false],
    ], (thunk) => thunk());
});

S.end();
print("test_otp_blackbox: all tests passed (" + n + " assertions)");
