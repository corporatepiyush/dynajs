// 27 · Two-factor authentication — TOTP enrollment, verification with clock skew, replay protection, backup codes.
//
// WHAT IT SHOWS
//   - dyna:crypto TOTPGenerate/TOTPVerify (RFC 6238), the codes authenticator apps show
//   - dyna:encoding Base32Encode + QRToString: the otpauth:// URI as a scannable QR code
//   - the two things a naive implementation forgets: a code must not be accepted twice,
//     and repeated wrong guesses must be throttled
//   - single-use backup codes stored only as hashes
//
// RUN      dynajs examples/apps/27-two-factor-authentication.js

import { TOTPGenerate, TOTPVerify, RandomBytes, TimingSafeEqual } from "dyna:crypto";
import { Base32Encode, QRToString } from "dyna:encoding";
import { SHA256 } from "dyna:hash";
import { NanoIDAlphabet } from "dyna:uuid";

const PERIOD = 30;                 // seconds per code
const WINDOW = 1;                  // accept the previous and next period: phones drift
const MAX_FAILURES = 5;

const accounts = new Map();        // user -> { secret, lastCounter, failures, backup: Set<hex> }
const hex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");

// Step 1: the server creates a secret and shows it to the user ONCE.
function enroll(user, issuer = "ExampleCorp") {
    const secret = RandomBytes(20);                       // 160 bits, the RFC's recommendation
    const backupCodes = Array.from({ length: 8 }, () => NanoIDAlphabet("23456789abcdefghjkmnpqrstuvwxyz", 10));
    accounts.set(user, {
        secret, lastCounter: -1, failures: 0,
        // Store hashes: someone who reads the database cannot use the codes.
        backup: new Set(backupCodes.map((c) => hex(SHA256(c)))),
    });
    const uri = `otpauth://totp/${encodeURIComponent(issuer)}:${encodeURIComponent(user)}` +
                `?secret=${Base32Encode(secret).replace(/=+$/, "")}&issuer=${encodeURIComponent(issuer)}&period=${PERIOD}&digits=6`;
    return { uri, backupCodes };
}

// Step 2: every login. `atSec` is injectable so the logic is testable.
function verify(user, code, atSec = Math.floor(Date.now() / 1000)) {
    const acct = accounts.get(user);
    if (!acct) return { ok: false, reason: "not enrolled" };
    if (acct.failures >= MAX_FAILURES) return { ok: false, reason: "locked" };

    // Find WHICH period matched, so that exact counter can be burned.
    const current = Math.floor(atSec / PERIOD);
    for (let counter = current - WINDOW; counter <= current + WINDOW; counter++) {
        if (!TOTPVerify(acct.secret, code, { atSec: counter * PERIOD, period: PERIOD })) continue;
        // Replay protection: a code (and every older one) works at most once.
        // Without this, someone who watches you type a code has 30 seconds to reuse it.
        if (counter <= acct.lastCounter) return { ok: false, reason: "code already used" };
        acct.lastCounter = counter;
        acct.failures = 0;
        return { ok: true };
    }
    acct.failures++;
    return { ok: false, reason: acct.failures >= MAX_FAILURES ? "locked" : "invalid code" };
}

// Lost phone: a backup code works once and also clears a lockout.
function redeemBackupCode(user, code) {
    const acct = accounts.get(user);
    if (!acct) return false;
    const digest = hex(SHA256(code.trim().toLowerCase()));
    if (!acct.backup.delete(digest)) return false;
    acct.failures = 0;
    return true;
}

// ---- demo ------------------------------------------------------------------
const { uri, backupCodes } = enroll("ada@example.com");
console.log("scan this in an authenticator app:\n");
console.log(QRToString(uri));
console.log(uri.replace(/secret=[A-Z2-7]+/, "secret=<hidden>"));

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const secret = accounts.get("ada@example.com").secret;
const T = 1767225600;                                           // a fixed "now"
const codeAt = (t) => TOTPGenerate(secret, { atSec: t, period: PERIOD });

check(/^\d{6}$/.test(codeAt(T)), "codes are six digits");
check(verify("ada@example.com", codeAt(T), T).ok, "the current code is accepted");
check(verify("ada@example.com", codeAt(T), T + 5).reason === "code already used", "the same code is refused a second time");
check(verify("ada@example.com", codeAt(T + 30), T + 30).ok, "the next period's code works");
check(verify("ada@example.com", codeAt(T + 60), T + 85).ok, "a code from one period ago is tolerated");
check(!verify("ada@example.com", codeAt(T + 300), T + 600).ok, "an old code outside the window is refused");

// Five wrong guesses lock the account; even the right code is then refused.
enroll("bob@example.com");
const bob = accounts.get("bob@example.com");
for (let i = 0; i < MAX_FAILURES; i++) verify("bob@example.com", "000000", T);
const right = TOTPGenerate(bob.secret, { atSec: T, period: PERIOD });
check(verify("bob@example.com", right, T).reason === "locked", "the account locks after repeated failures");

const bobCodes = [...enroll("bob@example.com").backupCodes];
for (let i = 0; i < MAX_FAILURES; i++) verify("bob@example.com", "000000", T);
check(redeemBackupCode("bob@example.com", bobCodes[0]), "a backup code is accepted");
check(!redeemBackupCode("bob@example.com", bobCodes[0]), "but only once");
check(verify("bob@example.com", TOTPGenerate(accounts.get("bob@example.com").secret, { atSec: T, period: PERIOD }), T).ok,
      "redeeming a backup code lifts the lockout");
check(backupCodes.length === 8 && new Set(backupCodes).size === 8, "eight distinct backup codes are issued");
console.log("\nself-test passed: verify, replay refusal, skew window, lockout, backup codes");
