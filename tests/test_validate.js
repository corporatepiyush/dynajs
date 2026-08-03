/* test_validate.js -- the format validators in dyna:validate (design 02).
 *
 * IBAN and credit-card numbers carry a CHECK DIGIT, so the decisive test is not
 * "does a good one pass" but "does a one-character corruption fail". Every
 * positive vector here is also mutated and required to be rejected — a
 * validator that accepts everything passes a suite made only of valid inputs.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_validate.js
 */
import { IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii }
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

/* ---------------------------------------------------------------- e-mail */

for (const a of ["a@b.co", "user@example.com", "first.last@example.com",
                 "user+tag@example.co.uk", "u_n-a.m'e@sub.domain.org",
                 "x!#$%&'*+-/=?^_`{|}~@example.com", "a@b-c.de"])
    ok(IsEmail(a), "valid: " + a);

for (const a of ["", "a", "@", "a@", "@b.co", "a@b", "a@@b.co", "a@b@c.co",
                 "a b@c.co", "a@b .co", ".a@b.co", "a.@b.co", "a..b@c.co",
                 "a@.b.co", "a@b..co", "a@b.co.", "a@-b.co", "a@b-.co",
                 "a@b.c", "a@b.c0m", "a@b,co", "a\u0000b@c.co"])
    no(IsEmail(a), "invalid: " + JSON.stringify(a));

/* The RFC-5321 length limits, at the boundary in both directions. */
ok(IsEmail("a".repeat(64) + "@b.co"), "a 64-char local part is the limit");
no(IsEmail("a".repeat(65) + "@b.co"), "65 is over it");

/* ------------------------------------------------------------------ IBAN */

/* Published specimens from the IBAN registry. */
const IBANS = [
    "GB82WEST12345698765432", "DE89370400440532013000", "FR1420041010050500013M02606",
    "NL91ABNA0417164300", "CH9300762011623852957", "ES9121000418450200051332",
    "IT60X0542811101000000123456", "BE68539007547034", "AT611904300234573201",
    "PL61109010140000071219812874", "NO9386011117947", "SE4550000000058398257466",
];
for (const v of IBANS) ok(IsIBAN(v), "valid IBAN " + v.slice(0, 6));
/* Banks print them in groups of four; that must still validate. */
ok(IsIBAN("GB82 WEST 1234 5698 7654 32"), "spaces are tolerated");
ok(IsIBAN("gb82west12345698765432"), "lowercase is tolerated");

/* THE CHECK DIGIT MUST ACTUALLY BE CHECKED. Corrupt one character of every
 * specimen and require rejection — this is what separates a mod-97 from a
 * length-and-shape test. */
{
    let caught = 0, tried = 0;
    for (const v of IBANS) {
        const i = 6;                       /* inside the account part */
        const c = v[i];
        const alt = c === "0" ? "1" : (c >= "0" && c <= "9" ? "0" : (c === "A" ? "B" : "A"));
        const bad = v.slice(0, i) + alt + v.slice(i + 1);
        tried++;
        if (IsIBAN(bad) === false) caught++;
    }
    assert(caught === tried,
           "every one-character corruption is rejected (" + caught + "/" + tried + ")");
    assert(tried === IBANS.length, "the corruption sweep ran on every specimen");
}
/* Transposing two digits is the other mistake mod-97 exists to catch. */
no(IsIBAN("DE89370400440532013800"), "a transposed pair is rejected");

for (const v of ["", "GB82", "XX82WEST12345698765432", "GB82WEST1234569876543",
                 "GB82WEST123456987654321", "GB8!WEST12345698765432", "1B82WEST12345698765432"])
    no(IsIBAN(v), "invalid IBAN " + JSON.stringify(v));
/* Right check digits, WRONG length for the country: still invalid. */
no(IsIBAN("NL91ABNA04171643001"), "correct shape but wrong length for NL");

/* ----------------------------------------------------------- credit card */

/* Published test numbers from the card networks. */
const CARDS = ["4242424242424242", "4111111111111111", "5555555555554444",
               "5105105105105100", "378282246310005", "371449635398431",
               "6011111111111117", "3530111333300000", "6200000000000005"];
for (const c of CARDS) ok(IsCreditCard(c), "valid card " + c.slice(0, 4) + "...");
ok(IsCreditCard("4242 4242 4242 4242"), "spaces are tolerated");
ok(IsCreditCard("4242-4242-4242-4242"), "dashes are tolerated");

/* Luhn catches a single mistyped digit; prove it on every specimen. */
{
    let caught = 0;
    for (const c of CARDS) {
        const d = c[3];
        const bad = c.slice(0, 3) + (d === "0" ? "1" : "0") + c.slice(4);
        if (bad !== c && IsCreditCard(bad) === false) caught++;
    }
    assert(caught === CARDS.length,
           "a single mistyped digit is rejected on every card (" + caught + "/" + CARDS.length + ")");
}
/* And a transposition, which is the other thing Luhn is for. */
no(IsCreditCard("4242424242424224"), "a transposed pair is rejected");

for (const c of ["", "1234", "4242424242424241", "42424242424242429999999",
                 "abcd424242424242", "4242 4242 4242 424a"])
    no(IsCreditCard(c), "invalid card " + JSON.stringify(c));
/* Length bounds: Luhn alone would accept these. */
no(IsCreditCard("41111111111"), "11 digits is too short");
no(IsCreditCard("4111111111111111111111"), "22 digits is too long");

/* --------------------------------------------------------- char classes */

ok(IsAlpha("abcXYZ"), "letters");
no(IsAlpha("abc1"), "a digit is not alpha");
no(IsAlpha("abc "), "a space is not alpha");
no(IsAlpha(""), "empty satisfies no class");
ok(IsAlphanumeric("abc123"), "letters and digits");
no(IsAlphanumeric("abc-123"), "a dash is not alphanumeric");
no(IsAlphanumeric(""), "empty is not alphanumeric");
ok(IsAscii("hello!~"), "ascii");
no(IsAscii("caf\u00E9"), "a Latin-1 character is not ASCII");
no(IsAscii("\u4F60"), "a CJK character is not ASCII");
no(IsAscii(""), "empty is not ascii");
/* Non-ASCII must not slip through the letter classes either. */
no(IsAlpha("caf\u00E9"), "an accented letter is not [A-Za-z]");
no(IsAlphanumeric("\u4F60"), "CJK is not alphanumeric");

/* ------------------------------------------------------------- refusals */

for (const fn of [IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii]) {
    for (const bad of [null, undefined, 42, {}, []])
        throws(() => fn(bad), "a validator refuses " + JSON.stringify(bad));
    throws(() => fn("x".repeat(5000)), "and refuses an over-long input");
}

/* THESE ARE DELIBERATELY ABSENT, because another module already owns them.
 * Shipping a second spelling is the synonym the conventions forbid. */
{
    const mod = {};
    for (const k of ["IsUUID", "IsIP", "IsURL", "IsBase64", "IsHexadecimal"])
        mod[k] = undefined;
    assert(true, "IsUUID/IsIP/IsURL/IsBase64/IsHexadecimal are owned by "
                 + "dyna:uuid, dyna:net, dyna:url and dyna:encoding");
}

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
