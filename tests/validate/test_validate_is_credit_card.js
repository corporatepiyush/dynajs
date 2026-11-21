// covers: dyna:validate.IsCreditCard
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_credit_card");
import { cpuNow } from "../lib/cpuclock.js";
import { IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii,
         IsUUID, IsSemver, IsJWT, IsE164,
         IsIPv4, IsIPv6, IsIP, IsPort, IsBase64, IsBase64Url, IsHex,
         IsHexColor, IsNumeric, IsDateString, IsRFC3339, IsJSON,
         IsMimeType, IsStrongPassword }
    from "dyna:validate";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function ok(v, msg) { assert(v === true, msg + " (expected true)"); }
function no(v, msg) { assert(v === false, msg + " (expected false)"); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

const INTAKE_IBANS = [
    "AF15000000000000000000019046", "NI45BAPR00000013000003558124",
    "SD1700000012345678", "SO080000000000000000444",
];

for (const v of INTAKE_IBANS) {
    const bad = v.slice(0, 8) + (v[8] === "0" ? "1" : "0") + v.slice(9);
    no(IsIBAN(bad), "a corrupted " + v.slice(0, 2) + " IBAN fails mod-97");
}

const CARDS = ["4242424242424242", "4111111111111111", "5555555555554444",
               "5105105105105100", "378282246310005", "371449635398431",
               "6011111111111117", "3530111333300000", "6200000000000005"];

for (const c of CARDS) ok(IsCreditCard(c), "valid card " + c.slice(0, 4) + "...");

ok(IsCreditCard("4242 4242 4242 4242"), "spaces are tolerated");

ok(IsCreditCard("4242-4242-4242-4242"), "dashes are tolerated");

__kitS.section("a single mistyped digit is rejected on every card", () => {
    let caught = 0;
    for (const c of CARDS) {
        const d = c[3];
        const bad = c.slice(0, 3) + (d === "0" ? "1" : "0") + c.slice(4);
        if (bad !== c && IsCreditCard(bad) === false) caught++;
    }
    assert(caught === CARDS.length,
           "a single mistyped digit is rejected on every card (" + caught + "/" + CARDS.length + ")");
});
__kitS.end();

no(IsCreditCard("4242424242424224"), "a transposed pair is rejected");

for (const c of ["", "1234", "4242424242424241", "42424242424242429999999",
                 "abcd424242424242", "4242 4242 4242 424a"])
    no(IsCreditCard(c), "invalid card " + JSON.stringify(c));
no(IsCreditCard("41111111111"), "11 digits is too short");
no(IsCreditCard("4111111111111111111111"), "22 digits is too long");

ok(IsAlpha("abcXYZ"), "letters");

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
