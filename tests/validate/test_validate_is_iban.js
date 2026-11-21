// covers: dyna:validate.IsIBAN
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_validate_is_iban");
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

no(IsEmail("a@" + "b".repeat(60) + "." + "c".repeat(64)), "a 64-char TLD is refused");

const IBANS = [
    "GB82WEST12345698765432", "DE89370400440532013000", "FR1420041010050500013M02606",
    "NL91ABNA0417164300", "CH9300762011623852957", "ES9121000418450200051332",
    "IT60X0542811101000000123456", "BE68539007547034", "AT611904300234573201",
    "PL61109010140000071219812874", "NO9386011117947", "SE4550000000058398257466",
];

for (const v of IBANS) ok(IsIBAN(v), "valid IBAN " + v.slice(0, 6));

ok(IsIBAN("GB82 WEST 1234 5698 7654 32"), "spaces are tolerated");

ok(IsIBAN("gb82west12345698765432"), "lowercase is tolerated");

__kitS.section("every one-character corruption is rejected", () => {
    let caught = 0, tried = 0;
    for (const v of IBANS) {
        const i = 6;
        const c = v[i];
        const alt = c === "0" ? "1" : (c >= "0" && c <= "9" ? "0" : (c === "A" ? "B" : "A"));
        const bad = v.slice(0, i) + alt + v.slice(i + 1);
        tried++;
        if (IsIBAN(bad) === false) caught++;
    }
    assert(caught === tried,
           "every one-character corruption is rejected (" + caught + "/" + tried + ")");
    assert(tried === IBANS.length, "the corruption sweep ran on every specimen");
});

no(IsIBAN("DE89370400440532013800"), "a transposed pair is rejected");

for (const v of ["", "GB82", "XX82WEST12345698765432", "GB82WEST1234569876543",
                 "GB82WEST123456987654321", "GB8!WEST12345698765432", "1B82WEST12345698765432"])
    no(IsIBAN(v), "invalid IBAN " + JSON.stringify(v));
no(IsIBAN("NL91ABNA04171643001"), "correct shape but wrong length for NL");

const NEW_IBANS = [
    "BI3710000123456789000000000", "DJ7210000000000123456780000",
    "IQ98NBIQ850123456789012", "IR200170000000000123456789",
    "LY15002048000000020010353", "MN360012345678000012",
    "OM100101234567010000000", "ST73000100020000009876543",
    "SV22CAGR00000000000000000007", "RU3604452522540817810500000001234",
    "SC72BAE1006100001234567890USN00", "VA03001123000012826157",
    "TL380080012345678910157",
];

for (const v of NEW_IBANS) ok(IsIBAN(v), "valid IBAN " + v.slice(0, 2) + " (" + v.length + ")");

__kitS.section("a 32-char RU IBAN is the wrong length", () => {
    const ru = "RU3604452522540817810500000001234";
    no(IsIBAN(ru.slice(0, 32)), "a 32-char RU IBAN is the wrong length");
    const bad = ru.slice(0, 6) + (ru[6] === "8" ? "9" : "8") + ru.slice(7);
    no(IsIBAN(bad), "a corrupted Russian IBAN fails mod-97");
});
__kitS.end();

ok(IsIBAN("GB82-WEST-1234-5698-7654-32"), "dashes are tolerated");

ok(IsIBAN("RU36-0445-2522-5408-1781-0500-0000-0123-4"), "dashed Russian IBAN tolerated");

const INTAKE_IBANS = [
    "AF15000000000000000000019046", "NI45BAPR00000013000003558124",
    "SD1700000012345678", "SO080000000000000000444",
];

for (const v of INTAKE_IBANS) ok(IsIBAN(v), "valid IBAN " + v.slice(0, 2));

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
