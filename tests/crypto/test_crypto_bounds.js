// flags: --std
// timeout: 120
import { Argon2id, PBKDF2, HKDF, TOTPVerify, JWTSign, JWTVerify } from "dyna:crypto";
import { QREncode, QRToString, Base64URLEncode } from "dyna:encoding";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
function throwsMatch(fn, re, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        if (re.test(String(e)))
            return;
        failed++;
        console.log("  FAIL " + msg + " (threw " + String(e).slice(0, 80) + ")");
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

throwsMatch(() => PBKDF2({ password: "x", iterations: 1000 }),
    /salt/i, "PBKDF2 without salt throws");
ok(typeof PBKDF2({ password: "x", salt: "s", iterations: 1000, length: 16 }) !== "undefined",
    "PBKDF2 with salt works");
ok(typeof HKDF({ key: "k", length: 16 }) !== "undefined",
    "HKDF keeps its RFC 5869 optional salt");
throwsMatch(() => PBKDF2({ password: "x", salt: "s", iterations: 0 }),
    /iterations/i, "PBKDF2 iterations 0 refused");
throwsMatch(() => PBKDF2({ password: "x", salt: "s", length: 1 << 21 }),
    /length/i, "PBKDF2 oversize output refused");

const bigPhc = "$argon2id$v=19$m=4194304,t=16,p=16$MDEyMzQ1Njc4OWFiY2RlZg$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
throwsMatch(() => Argon2id.verify(bigPhc, "pw"),
    /cap|memory/i, "Argon2id.verify refuses a 4 GiB PHC memory parameter");
const smallPhc = "$argon2id$v=19$m=8192,t=1,p=1$MDEyMzQ1Njc4OWFiY2RlZg$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
try {
    Argon2id.verify(smallPhc, "pw");
    ok(true, "small PHC verifies (returns false, no throw)");
} catch (e) {
    ok(false, "small PHC must not throw: " + e);
}

throwsMatch(() => TOTPVerify("c2VjcmV0", "000000", { window: 500000 }),
    /window/i, "TOTPVerify refuses an impractical window");

throwsMatch(() => QREncode("x", { ecc: " " }),
    /ecc/i, "QREncode rejects the space ecc (ecl==4 out-of-bounds read)");
throwsMatch(() => QRToString("x", { ecc: " " }),
    /ecc/i, "QRToString rejects the space ecc");
ok(QREncode("x", { ecc: "l" }) !== undefined && QREncode("x", { ecc: "H" }) !== undefined,
    "lowercase and uppercase L/M/Q/H still work");

{
    const key = "0123456789abcdef";
    const tok = JWTSign({ sub: "u", exp: Math.floor(Date.now() / 1000) + 600 }, key, { alg: "HS256" });
    const v = JWTVerify(tok, key, { algorithms: ["HS256"] });
    ok(v && v.sub === "u", "JWTVerify round trip");
    const parts = tok.split(".");
    const nulAlg = Base64URLEncode(JSON.stringify({ alg: "HS256\u0000evil", typ: "JWT" }));
    let threw = false;
    try {
        JWTVerify(nulAlg + "." + parts[1] + "." + parts[2], key, { algorithms: ["HS256"] });
    } catch (e) {
        threw = /alg/i.test(String(e));
    }
    ok(threw, "an alg with an embedded NUL is refused, not silently truncated to HS256");
    const longAlg = Base64URLEncode(JSON.stringify({ alg: "a".repeat(40), typ: "JWT" }));
    threw = false;
    try {
        JWTVerify(longAlg + "." + parts[1] + "." + parts[2], key, { algorithms: ["HS256"] });
    } catch (e) {
        threw = true;
    }
    ok(threw, "an over-long alg is refused");
}
console.log("test_crypto_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_crypto_bounds: " + failed + " failures");
