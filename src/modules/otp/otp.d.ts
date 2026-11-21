/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:otp
/** dyna:otp — one-time passwords: RFC 4226 HOTP and RFC 6238 TOTP (moved out of dyna:crypto, which no longer exports them),
 *  otpauth:// key URIs (Google Authenticator Key URI format), enrolment with a QR code (dyna:encoding QREncode), look-ahead and
 *  drift windows, replay protection through the last used counter, and a stateless throttle for failed attempts.
 *  Secrets are raw bytes: a string is taken as its UTF-8 bytes (RFC 4226 Appendix D uses the ASCII seed "12345678901234567890"),
 *  never base32-decoded; KeyURIParse returns the decoded bytes. Codes are compared in constant time and every counter of a
 *  window is computed and compared, so the time spent does not depend on which counter matched. */
declare module "dyna:otp" {
    type OTPAlgorithm = "sha1" | "sha256" | "sha512";

    /** RFC 4226 HOTP: HMAC over the 8-byte big-endian counter, dynamic truncation, modulo 10^digits. digits 6..8 (default 6), algo default "sha1". */
    function HOTPGenerate(secret: BytesInput, counter: number, opts?: { digits?: number; algo?: OTPAlgorithm }): string;

    /** RFC 6238 TOTP: HOTP with counter floor(atSec / period) (T0 = 0). atSec defaults to the current Unix time (pass it for reproducible results); period default 30. */
    function TOTPGenerate(secret: BytesInput, opts?: { atSec?: number; period?: number; digits?: number; algo?: OTPAlgorithm }): string;

    /** HOTP verification of exactly one counter in constant time; a malformed code is FALSE like a wrong guess, never a throw. For a look-ahead window or the matched counter use HOTPMatch. */
    function HOTPVerify(secret: BytesInput, counter: number, code: string, opts?: { digits?: number; algo?: OTPAlgorithm }): boolean;

    /** TOTP verification; window (default 1, at most 4096 periods: RFC 6238 section 5.2 recommends one step of network delay) accepts counters within +/- window of floor(atSec / period); atSec defaults to the current Unix time. It has no replay protection: use TOTPMatch with lastCounter. */
    function TOTPVerify(secret: BytesInput, code: string, opts?: { atSec?: number; period?: number; digits?: number; algo?: OTPAlgorithm; window?: number }): boolean;

    /**
     * RFC 4226 section 7.4 resynchronisation: tries counters counter .. counter + lookAhead (lookAhead an integer in [0, 4096],
     * default 0) and returns the lowest matching counter with next = counter + 1, the value to store as the next expected
     * counter (a code is then never accepted twice), or null. counter is an integer in [0, 2^53-1]. A malformed code is null.
     */
    function HOTPMatch(secret: BytesInput, counter: number, code: string, opts?: { digits?: number; algo?: OTPAlgorithm; lookAhead?: number }): { counter: number; next: number } | null;

    /**
     * RFC 6238 verification with drift and replay protection. With step s = floor(atSec / period) (atSec defaults to the
     * current Unix time), it tries counters s + drift - window .. s + drift + window (window an integer in [0, 4096], default 0;
     * drift an integer, default 0), skips counters at or below lastCounter (RFC 6238 section 5.2: an accepted OTP is never
     * accepted again) and returns { counter, drift: counter - s } or null. When several counters of the window produce the code,
     * the one nearest s + drift wins, the lower on a tie. Store counter as the next lastCounter and drift as the next drift
     * (section 6 resynchronisation). A malformed code is null.
     */
    function TOTPMatch(secret: BytesInput, code: string, opts?: { atSec?: number; period?: number; digits?: number; algo?: OTPAlgorithm; window?: number; drift?: number; lastCounter?: number }): { counter: number; drift: number } | null;

    /** A key from an otpauth:// URI. issuer is absent when the URI has neither an issuer parameter nor a label prefix; period is set for totp, counter for hotp. */
    interface OTPKey {
        type: "totp" | "hotp";
        issuer?: string;
        account: string;
        secret: Uint8Array;
        algo: OTPAlgorithm;
        digits: number;
        period?: number;
        counter?: number;
    }

    /**
     * Parses otpauth://TYPE/LABEL?PARAMETERS (Google Authenticator Key URI format). The scheme, the type (totp or hotp),
     * the algorithm value and the base32 secret are case-insensitive. LABEL is percent-decoded and is "account" or
     * "issuer:account" (a literal or %3A colon; spaces after it are dropped; more than one colon is refused). Parameter
     * values are percent-decoded with "+" read as a space. secret is required (RFC 4648 base32, padding optional, any
     * length of at least one byte; trailing bits must be zero). issuer must equal the label prefix when both are present.
     * algorithm SHA1 (default), SHA256 or SHA512; digits 6 (default) or 8; counter is required for hotp, an integer in
     * [0, 2^53-1]; period (totp, default 30) an integer >= 1. Unknown parameters are ignored, a repeated known parameter is
     * refused. Decoded text must be valid UTF-8 without control characters. @throws {TypeError} when uri is not a string.
     * @throws {SyntaxError} on any other violation.
     */
    function KeyURIParse(uri: string): OTPKey;

    /** Key fields for KeyURIBuild and Enroll. type default "totp"; account (required) and issuer are non-empty text without a colon; digits 6 or 8; period only with totp (default 30), counter only with hotp (default 0). */
    interface OTPKeyInput {
        type?: "totp" | "hotp";
        account: string;
        issuer?: string;
        algo?: OTPAlgorithm;
        digits?: number;
        period?: number;
        counter?: number;
    }

    /**
     * Builds otpauth://TYPE/[issuer:]account?secret=...&issuer=...&algorithm=...&digits=...&period=... (or &counter=...). Every
     * byte of the label and issuer outside A-Z a-z 0-9 - . _ ~ is percent-encoded (a space is %20); the secret is unpadded
     * upper-case base32 of the given bytes; algorithm, digits and period/counter are always written. KeyURIParse(KeyURIBuild(k))
     * gives back k. @throws {TypeError} for a missing or mistyped field or an unknown key. @throws {RangeError} for an empty
     * secret, digits 7, or a name that is empty or holds a colon or control characters.
     */
    function KeyURIBuild(key: OTPKeyInput & { secret: BytesInput }): string;

    /** Enrolment options: the key fields, a secret of 16..64 bytes or secretBytes (16..64, default 20 random bytes from the OS CSPRNG; RFC 4226 asks for at least 128 bits and recommends 160), and QR options for dyna:encoding QREncode. */
    interface OTPEnrollOptions extends OTPKeyInput {
        secret?: BytesInput;
        secretBytes?: number;
        qr?: import("dyna:encoding").QROptions;
        /** dyna-specific meta configuration. qr: false skips both QR renderings (the costly part; uri and secret only). */
        dyna?: { qr?: boolean };
    }

    /**
     * Creates a key for a user: { uri, secret, secretBase32, qr, qrText }. qr is exactly QREncode(uri, opts.qr) and qrText
     * exactly QRToString(uri, opts.qr). Store secret server-side (sealed); show the QR code or secretBase32 to the user once.
     * @throws {TypeError} when both secret and secretBytes are given, a field is mistyped or a key is unknown, or when the build lacks dyna:encoding and dyna.qr is not false.
     */
    function Enroll(opts: OTPEnrollOptions): { uri: string; secret: Uint8Array; secretBase32: string; qr?: { version: number; size: number; modules: Uint8Array }; qrText?: string };

    /**
     * RFC 4226 section 7.3 throttling over caller-held state (no table is kept here): failures is the count of consecutive
     * failed attempts and lastFailureSec the Unix time of the last one (required when failures > 0). The next attempt is
     * allowed from lastFailureSec + stepSec * failures (stepSec an integer in [0, 86400], default 5: a linearly growing delay);
     * at lockoutAfter failures (default 10; 0 disables the lockout) the key is locked until the caller resets it, and
     * retryAtSec is Infinity. atSec defaults to the current Unix time. Reset failures to 0 after a successful match.
     */
    function Throttle(failures: number, lastFailureSec?: number, opts?: { atSec?: number; stepSec?: number; lockoutAfter?: number }): { allowed: boolean; locked: boolean; retryAtSec: number };
}
