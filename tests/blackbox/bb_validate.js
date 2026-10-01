// Black-box contract test for dyna:validate, generated from dynajs.d.ts lines 6017-6084. Engine sources not consulted.
// Table-driven: every expectation is a case row; each failure names its row (API.md pins the
// per-function grammars quoted in each table's header comment).
import { IsAlpha, IsAlphanumeric, IsAscii, IsBase64, IsBase64Url, IsCreditCard, IsDateString, IsDomain, IsE164, IsEmail, IsHex, IsHexColor, IsIBAN, IsIP, IsIPv4, IsIPv6, IsJSON, IsJWT, IsMimeType, IsNumeric, IsPort, IsRFC3339, IsSemver, IsSlug, IsStrongPassword, IsURL, IsUUID } from "dyna:validate";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg);
}

// Drives one predicate table: rows are [input, expected]; the label names the row.
function table(name, fn, rows) {
    for (const [input, expected] of rows)
        assertEq(fn(input), expected, name + "(" + JSON.stringify(input) + ")");
}

// ------------------------------------------------------------------
// Char classes — d.ts: "ASCII letters only" / "ASCII letters and digits" /
// "Every byte below 0x80"; API.md: "An empty string satisfies no class".
// ------------------------------------------------------------------

table("IsAlpha", IsAlpha, [["abc", true], ["abc1", false], ["ab c", false], ["", false]]);
table("IsAlphanumeric", IsAlphanumeric, [["abc123", true], ["abc-123", false], ["", false]]);
table("IsAscii", IsAscii, [["plain\t", true], ["café", false], ["", false]]);

// ------------------------------------------------------------------
// IsEmail — API.md: "one unquoted atext local part (at most 64 chars, no leading/trailing
// dot, no `..`), one dotted domain with a letters-only TLD of at least two chars,
// total length 3..254; quoted strings and comments are refused".
// ------------------------------------------------------------------

table("IsEmail", IsEmail, [
    ["user@example.com", true],
    ["User@Example.COM", true],
    ["a..b@example.com", false],
    ["\"quoted\"@example.com", false],
    ["a@b.12", false],
    ["a@b", false],
    ["a".repeat(64) + "@example.com", true],
    ["a".repeat(65) + "@example.com", false],
]);

// ------------------------------------------------------------------
// IsCreditCard — API.md: "Luhn check digit over 12..19 digits. Spaces and hyphens
// are ignored". IsIBAN — "length per country ... then mod-97; spaces stripped,
// letters uppercased".
// ------------------------------------------------------------------

table("IsCreditCard", IsCreditCard, [
    ["4111111111111111", true],
    ["4242 4242 4242 4242", true],
    ["4111-1111-1111-1111", true],
    ["4242424242424241", false],
    ["411111111111", false],
]);
table("IsIBAN", IsIBAN, [
    ["DE89 3704 0044 0532 0130 00", true],
    ["GB82 WEST 1234 5698 7654 32", true],
    ["DE89370400440532013001", false],
    ["GB82 WEST 1234 5698 7654 3", false],
]);

// ------------------------------------------------------------------
// IsDomain — API.md: "RFC 1035 label grammar over the whole input: at least one dot,
// each label 1..63 chars with no edge hyphens, total 3..253; an IP literal is not a
// domain". IsURL — "the dyna:url constructor accepts it, plus a non-empty host for
// the special schemes, so \"https://\" fails".
// ------------------------------------------------------------------

table("IsDomain", IsDomain, [
    ["example.com", true],
    ["a.b.c.example.com", true],
    ["xn--80ak6aa92e.com", true],
    ["-example.com", false],
    ["example-.com", false],
    ["example..com", false],
    ["localhost", false],
    ["192.168.0.1", false],
]);
table("IsURL", IsURL, [
    ["https://example.com", true],
    ["https://", false],
    ["ftp://ftp.example.com", true],
    ["not a url", false],
    ["mailto:user@example.com", true],
]);

// ------------------------------------------------------------------
// Tokens — IsSlug ("lowercase letters, digits, single hyphens, at most 64 chars"),
// IsUUID ("RFC 4122 canonical 8-4-4-4-12 form only; braces and URN prefixes refused"),
// IsJWT ("three non-empty, unpadded base64url segments whose header and claims are
// JSON objects, the header naming an alg"), IsSemver, IsE164 ("optional `+`,
// 8..15 digits").
// ------------------------------------------------------------------

table("IsSlug", IsSlug, [
    ["hello-world", true],
    ["a", true],
    ["Hello", false],
    ["-lead", false],
    ["dou--ble", false],
    ["has space", false],
    ["a".repeat(64), true],
    ["a".repeat(65), false],
]);
table("IsUUID", IsUUID, [
    ["123e4567-e89b-12d3-a456-426614174000", true],
    ["123e4567-e89b-12d3-a456-42661417400", false],
    ["123e4567-e89b-12d3-a456-4266141740000", false],
    ["{123e4567-e89b-12d3-a456-426614174000}", false],
    ["urn:uuid:123e4567-e89b-12d3-a456-426614174000", false],
    ["123e4567e89b12d3a456426614174000", false],
    ["123e4567-e89b-12d3-a456-42661417400g", false],
]);
table("IsJWT", IsJWT, [
    ["eyJhbGciOiJub25lIn0.eyJzdWIiOiIxIn0.sig", true],
    ["eyJhbGciOiJub25lIn0.eyJzdWIiOiIxIn0", false],
    ["eyJhbGciOiJub25lIn0=.eyJzdWIiOiIxIn0.sig", false],
    ["eyJzdWIiOiIxIn0.eyJzdWIiOiIxIn0.sig", false],
]);
table("IsSemver", IsSemver, [
    ["1.2.3", true],
    ["v1.2.3", true],
    ["1.02.3", false],
    ["1.2", false],
]);
table("IsE164", IsE164, [
    ["+14155552671", true],
    ["14155552671", true],
    ["12345678", true],
    ["1234567", false],
    ["123456789012345", true],
    ["1234567890123456", false],
]);

// ------------------------------------------------------------------
// Addresses — IsIPv4 ("strict dotted-quad ... no leading zeros — 127.000.000.001 is
// refused outright, no whitespace, no sign"), IsIPv6 ("RFC 4291 ... `::` at most once,
// embedded IPv4 tail only at the end, zone ids refused; group leading zeros are
// grammar-legal"), IsPort ("0..65535, no sign, no leading zeros; \"0\" is allowed").
// ------------------------------------------------------------------

table("IsIPv4", IsIPv4, [
    ["127.0.0.1", true],
    ["0.0.0.0", true],
    ["255.255.255.255", true],
    ["127.000.000.001", false],
    ["256.0.0.1", false],
    ["1.2.3", false],
    [" 1.2.3.4", false],
    ["+1.2.3.4", false],
    ["01.2.3.4", false],
]);
table("IsIPv6", IsIPv6, [
    ["2001:db8::8a2e:370:7334", true],
    ["::1", true],
    ["::", true],
    ["::ffff:1.2.3.4", true],
    ["1:2:3:4:5:6:7:8", true],
    ["0001:0002:0003:0004:0005:0006:0007:0008", true],
    ["fe80::1%eth0", false],
    ["1::2::3", false],
]);
table("IsIP", IsIP, [["1.2.3.4", true], ["::ffff:1.2.3.4", true], ["abc", false]]);
table("IsPort", IsPort, [
    ["443", true],
    ["0", true],
    ["65535", true],
    ["65536", false],
    ["080", false],
    ["-1", false],
]);

// ------------------------------------------------------------------
// Encodings — IsBase64 ("RFC 4648 canonical: `+/`, `=` only as the final one or two,
// zero pad bits — \"AR==\" is refused; unpadded is refused; empty is false"),
// IsBase64Url ("the `-_` alphabet, unpadded and canonical; \"QQR\", \"a-\", a lone
// final character are refused"), IsHex ("even-length run; empty is false"),
// IsHexColor ("#RGB, #RRGGBB or #RRGGBBAA; # required; the 4-digit #RGBA form is
// NOT accepted"), IsJSON ("JSON.parse accepts it; over 4096 bytes throws a RangeError").
// ------------------------------------------------------------------

table("IsBase64", IsBase64, [
    ["aGVsbG8=", true],
    ["YWJj", true],
    ["AQ==", true],
    ["aGVsbG8", false],
    ["AR==", false],
]);
table("IsBase64Url", IsBase64Url, [
    ["eyJhbGciOiJIUzI1NiJ9", true],
    ["aA", true],
    ["hi", false],
    ["QQR", false],
    ["a-", false],
    ["n", false],
]);
table("IsHex", IsHex, [
    ["deadBEEF", true],
    ["0123456789abcdef", true],
    ["abc", false],
    ["", false],
    ["0x12", false],
]);
table("IsHexColor", IsHexColor, [
    ["#ff8800", true],
    ["#FF8800", true],
    ["#f80", true],
    ["#ff8800ff", true],
    ["#ff88", false],
    ["ff8800", false],
]);

// IsJSON: 4096-byte cap rows are computed (boundary), the throw row pins RangeError.
{
    const JSON_ROWS = [
        ['{"ok":true}', true],
        ["null", true],
        ["123", true],
        ["[1,2,3]", true],
        ["{bad", false],
        ["NaN", false],
        ["", false],
        ['"' + "a".repeat(4094) + '"', true],
    ];
    table("IsJSON", IsJSON, JSON_ROWS);
    assertThrows(() => IsJSON('"' + "a".repeat(4095) + '"'),
        "IsJSON: an input over 4096 bytes throws a RangeError rather than parse (doc pins)", RangeError);
}

// ------------------------------------------------------------------
// Media types & dates — IsMimeType ("token type/subtype; optional `; name=value`
// parameters, never repeated case-insensitively; quoted values ok; linear whitespace
// around the `;` only"), IsRFC3339 ("`T` separator, lowercase t/z are RFC-legal,
// `:60` allowed, offset `Z` or `+/-HH:MM`; a space separator is refused"),
// IsDateString ("strict YYYY-MM-DD with real month/day, 400-year rule included"),
// IsNumeric ("optional sign, at most one decimal point, exponent refused").
// ------------------------------------------------------------------

table("IsMimeType", IsMimeType, [
    ["text/html; charset=utf-8", true],
    ["text/html", true],
    ['text/html; charset="utf-8"', true],
    ["text", false],
    ["text / html", false],
    ["text/html; charset=a; CHARSET=b", false],
]);
table("IsRFC3339", IsRFC3339, [
    ["2023-01-15T10:30:00Z", true],
    ["2023-01-15t10:30:00z", true],
    ["2023-01-15T10:30:00+05:30", true],
    ["2023-01-15T10:30:00.5Z", true],
    ["2023-01-15T10:30:60Z", true],
    ["2023-01-15 10:30:00Z", false],
    ["2023-02-30T00:00:00Z", false],
    ["2023-01-15T10:30:00", false],
]);
table("IsDateString", IsDateString, [
    ["2024-02-29", true],
    ["2023-02-29", false],
    ["2000-02-29", true],
    ["1900-02-29", false],
    ["2100-02-29", false],
    ["2023-04-31", false],
    ["2023-13-01", false],
    ["2024-2-9", false],
]);
table("IsNumeric", IsNumeric, [
    ["-1.5", true],
    [".5", true],
    ["5.", true],
    ["+42", true],
    ["-", false],
    ["1e5", false],
    ["1.2.3", false],
    ["", false],
]);

// ------------------------------------------------------------------
// IsStrongPassword — API.md: "printable ASCII with class minimums; defaults 8/1/1/1;
// symbols are printable non-alphanumeric ASCII; a space counts toward the length only;
// non-ASCII refuses the password; a non-number opt throws TypeError, a negative,
// fractional or over-4096 value throws RangeError; a second argument that is not an
// object is ignored". Rows: [text, opts (null = omitted), expected].
// ------------------------------------------------------------------

{
    const PASSWORD = [
        ["doc example", "Str0ng!pass", null, true],
        ["relaxed doc example", "aaaaaaaa", { minLower: 8, minUpper: 0, minDigits: 0, minSymbols: 0 }, true],
        ["meets the 8/1/1/1 defaults", "Abcdefghi1!", null, true],
        ["default minimums unmet (no upper/digit/symbol)", "aaaaaaaa", null, false],
        ["no symbol under the default minimum", "Abcdefghi1", null, false],
        ["a space is not a symbol", "Abcdefghi1 ", { minSymbols: 1 }, false],
        ["a space counts toward the length only", "Abcdefghij1 !", null, true],
        ["a non-ASCII byte refuses the password", "Str0ng!passé", null, false],
        ["an empty string is never strong", "", null, false],
    ];
    for (const [label, text, opts, expected] of PASSWORD)
        assertEq(opts === null ? IsStrongPassword(text) : IsStrongPassword(text, opts), expected,
            "IsStrongPassword(" + JSON.stringify(text) + "): " + label);

    const PASSWORD_THROW = [
        ["a non-number opt throws TypeError", () => IsStrongPassword("x", { minLength: "8" }), TypeError],
        ["a negative opt throws RangeError", () => IsStrongPassword("x", { minLength: -1 }), RangeError],
        ["a fractional opt throws RangeError", () => IsStrongPassword("x", { minLength: 2.5 }), RangeError],
        ["an over-4096 opt throws RangeError", () => IsStrongPassword("x", { minLength: 5000 }), RangeError],
    ];
    for (const [label, fn, ErrType] of PASSWORD_THROW)
        assertThrows(fn, "IsStrongPassword: " + label, ErrType);

    assertEq(IsStrongPassword("Str0ng!pass", 42), true,
        "IsStrongPassword: a second argument that is not an object is ignored (doc pins)");
}

// ------------------------------------------------------------------
// The blanket contract: "Every function takes one string and returns a boolean;
// a non-string argument throws" (API.md header) — swept across every export.
// ------------------------------------------------------------------

{
    const FNS = [IsAlpha, IsAlphanumeric, IsAscii, IsBase64, IsBase64Url, IsCreditCard, IsDateString, IsDomain, IsE164, IsEmail, IsHex, IsHexColor, IsIBAN, IsIP, IsIPv4, IsIPv6, IsJSON, IsJWT, IsMimeType, IsNumeric, IsPort, IsRFC3339, IsSemver, IsSlug, IsStrongPassword, IsURL, IsUUID];
    for (const fn of FNS)
        assertThrows(() => fn(42), (fn.name || "fn#" + FNS.indexOf(fn)) + ": a non-string argument (42) throws (API.md header)");
}

print("bb_validate: all tests passed (" + n + " assertions)");
