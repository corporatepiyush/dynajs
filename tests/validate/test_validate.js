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

for (const a of ["a@b.co", "user@example.com", "first.last@example.com",
                 "user+tag@example.co.uk", "u_n-a.m'e@sub.domain.org",
                 "x!#$%&'*+-/=?^_`{|}~@example.com", "a@b-c.de"])
    ok(IsEmail(a), "valid: " + a);

for (const a of ["", "a", "@", "a@", "@b.co", "a@b", "a@@b.co", "a@b@c.co",
                 "a b@c.co", "a@b .co", ".a@b.co", "a.@b.co", "a..b@c.co",
                 "a@.b.co", "a@b..co", "a@b.co.", "a@-b.co", "a@b-.co",
                 "a@b.c", "a@b.c0m", "a@b,co", "a\u0000b@c.co"])
    no(IsEmail(a), "invalid: " + JSON.stringify(a));

for (const a of ["a@b.-cd.com", "ab@mail.-example.com", "a@b.c.-d.com",
                 "a@b.-cd.-ef.com", "a@b.cd-.com"])
    no(IsEmail(a), "a label edge hyphen after a dot: " + a);
ok(IsEmail("a@b.c--d.com"), "a doubled hyphen inside a label is legal");

ok(IsEmail("a".repeat(64) + "@b.co"), "a 64-char local part is the limit");
no(IsEmail("a".repeat(65) + "@b.co"), "65 is over it");

ok(IsEmail("a@" + "b".repeat(63) + ".com"), "a 63-char domain label is the limit");
no(IsEmail("a@" + "b".repeat(64) + ".com"), "a 64-char domain label is over it");
ok(IsEmail("a@" + "b".repeat(60) + "." + "c".repeat(63)), "the TLD is capped as a label too");
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

{
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
}
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
{
    const ru = "RU3604452522540817810500000001234";
    no(IsIBAN(ru.slice(0, 32)), "a 32-char RU IBAN is the wrong length");
    const bad = ru.slice(0, 6) + (ru[6] === "8" ? "9" : "8") + ru.slice(7);
    no(IsIBAN(bad), "a corrupted Russian IBAN fails mod-97");
}
ok(IsIBAN("GB82-WEST-1234-5698-7654-32"), "dashes are tolerated");
ok(IsIBAN("RU36-0445-2522-5408-1781-0500-0000-0123-4"), "dashed Russian IBAN tolerated");

const INTAKE_IBANS = [
    "AF15000000000000000000019046", "NI45BAPR00000013000003558124",
    "SD1700000012345678", "SO080000000000000000444",
];
for (const v of INTAKE_IBANS) ok(IsIBAN(v), "valid IBAN " + v.slice(0, 2));
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
no(IsCreditCard("4242424242424224"), "a transposed pair is rejected");

for (const c of ["", "1234", "4242424242424241", "42424242424242429999999",
                 "abcd424242424242", "4242 4242 4242 424a"])
    no(IsCreditCard(c), "invalid card " + JSON.stringify(c));
no(IsCreditCard("41111111111"), "11 digits is too short");
no(IsCreditCard("4111111111111111111111"), "22 digits is too long");

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
no(IsAlpha("caf\u00E9"), "an accented letter is not [A-Za-z]");
no(IsAlphanumeric("\u4F60"), "CJK is not alphanumeric");

ok(IsE164("12345678"), "8 digits is the floor");
no(IsE164("1234567"), "7 digits is too short to be a number");
no(IsE164("1"), "a lone digit is not a number");
ok(IsE164("+14155552671"), "a real US number passes");
no(IsE164("1234567890123456"), "16 digits exceed the 15-digit cap");

{
    const H = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    const C = "eyJzdWIiOiIxMjM0NTY3ODkwIn0";
    const TOK = H + "." + C + "." + "dGVzdHNpZ25hdHVyZQ";
    ok(IsJWT(TOK), "structural check unchanged without options");
    ok(IsJWT(TOK, { algs: ["HS256"] }), "alg in the allowlist passes");
    ok(IsJWT(TOK, { algs: ["ES256", "HS256"] }), "any allowlist entry matches");
    no(IsJWT(TOK, { algs: ["RS256"] }), "alg outside the allowlist is refused");
    no(IsJWT(TOK, { algs: [] }), "an empty allowlist refuses everything");
    no(IsJWT(TOK, { algs: "HS256" }), "a non-array allowlist refuses rather than skips");
    no(IsJWT(TOK, { algs: ["hs256"] }), "the alg match is exact, not case-folded");

    no(IsJWT("eyJhbGciOiJBIn0=." + C + "." + TOK.slice(TOK.lastIndexOf(".") + 1)),
       "a padded header is refused");
    no(IsJWT(H + ".eyJhIjoxfQ==." + "dGVzdHNpZ25hdHVyZQ"), "padded claims are refused");
    no(IsJWT(H + "." + C + ".ab=="), "a padded signature is refused");
    throws(() => IsJWT(TOK, { get algs() { throw new Error("options getter"); } }),
           "a throwing algs getter propagates");
}

for (const a of ["0.0.0.0", "255.255.255.255", "127.0.0.1", "8.8.8.8",
                 "192.168.1.100", "1.2.3.4", "10.0.0.1", "100.200.250.255",
                 "9.9.9.9", "203.0.113.7", "1.22.233.44"])
    ok(IsIPv4(a), "valid IPv4: " + a);

for (const a of ["", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.256",
                 "999.999.999.999", "127.000.000.001", "01.2.3.4", "1.02.3.4",
                 "1.2.3.", ".1.2.3.4", "1..2.3", "1.2.3.4 ", " 1.2.3.4",
                 "1.2.3.-4", "+1.2.3.4", "0x1.2.3.4", "1.2.3.4\n", "a.b.c.d",
                 "2130706433", "1.2.3.4:80", "1.2.3.4/24", "1.2.3.\u0001"])
    no(IsIPv4(a), "invalid IPv4: " + JSON.stringify(a));

for (const a of ["::", "::1", "1::", "fe80::1", "1:2:3:4:5:6:7:8",
                 "2001:db8::8a2e:370:7334", "::ffff:192.168.1.1",
                 "64:ff9b::1.2.3.4", "0:0:0:0:0:0:0:1",
                 "0001:0000:0000:0000:0000:0000:0000:0001",
                 "2001:0DB8:0:0:0:0:2:1", "1:2:3:4:5:6:7::",
                 "::1:2:3:4:5:6:7", "1:2:3:4:5:6:1.2.3.4", "::ffff:1.2.3.4"])
    ok(IsIPv6(a), "valid IPv6: " + a);

for (const a of ["", ":", ":::", "1:2:3:4:5:6:7:8:9", "1::2::3", "1:::2",
                 "1:2:3:4:5:6:7", "12345::", "1:2:3:4:5:6:7:8:",
                 ":1:2:3:4:5:6:7:8", "fe80::1%eth0", "::ffff:192.168.1.256",
                 "::ffff:01.2.3.4", "1.2.3.4", "1:2:3:4:5:1.2.3.4",
                 "1:2:3:4:5:6:7:1.2.3.4", "::1:2:3:4:5:6:7:8", "g::1",
                 "1.2.3.4::", "1:2:3:4:5:6:1.2.3.4:8", "::-1",
                 "1:2:3:4:5:6:7:8:", "0:0:0:0:0:0:0:0:0",
                 "1:2:3:4:5:6::1.2.3.4", "1:2:3:4:5:6:7::8"])
    no(IsIPv6(a), "invalid IPv6: " + JSON.stringify(a));
ok(IsIPv6("0:0:0:0:0:0:0:0"), "the all-zero address, written out");
ok(IsIPv6("::0.0.0.0"), "the all-zero address in the v4-tail form");

ok(IsIP("1.2.3.4"), "IsIP accepts v4");
ok(IsIP("::1"), "IsIP accepts v6");
ok(IsIP("::ffff:1.2.3.4"), "IsIP accepts the v4-mapped form");
for (const a of ["", "1.2.3", ":::", "example.com", "1.2.3.4.5"])
    no(IsIP(a), "IsIP refuses " + JSON.stringify(a));

for (const a of ["0", "1", "80", "443", "8080", "65534", "65535"])
    ok(IsPort(a), "valid port " + a);
for (const a of ["", "65536", "99999", "123456", "-1", "+80", "007", "00",
                 "080", "80 ", " 80", "8 0", "0x50", "4e2", "1.5",
                 "\u066080"  ])
    no(IsPort(a), "invalid port " + JSON.stringify(a));

for (const a of ["QQ==", "aGVsbG8=", "aGVsbG8h", "TWFu", "Zm9vYmFy",
                 "SGVsbG8sIHdvcmxkIQ==", "AAAA", "/+//", "//52", "a+b/",
                 "Zm9vYmE=", "TQ=="])
    ok(IsBase64(a), "valid base64 " + a.slice(0, 8));
for (const a of ["", "QQ=", "QQ===", "=QQ=", "====", "A", "AB=", "ABC",
                 "AR==", "AB==", "a-b_", "a b c=", "aGVs bG8=", "TQ== ",
                 "a\u0000b="])
    no(IsBase64(a), "invalid base64 " + JSON.stringify(a));
for (const a of ["QQ", "QQRJ", "aGVsbG8", "dGVzdA", "a-_1", "_-_-",
                 "eyJhbGciOiJIUzI1NiJ9"])
    ok(IsBase64Url(a), "valid base64url " + a.slice(0, 10));
for (const a of ["", "QQ==", "a+b", "a/b", "a.b", "a b", "a=b", "+/=",
                 "n", "QQR", "CAG", "kZKl2", "a-", "AB"])
    no(IsBase64Url(a), "invalid base64url " + JSON.stringify(a));

no(IsBase64("aGVsbG8"), "unpadded base64url text is not canonical base64");
no(IsBase64Url("aGVsbG8="), "padded base64 is not base64url");
no(IsBase64Url("a+b/"), "the standard alphabet is not the url alphabet");

for (const a of ["00", "ff", "FF", "0123456789abcdef", "ABCDEF", "deadBEEF",
                 "00112233"])
    ok(IsHex(a), "valid hex " + a.slice(0, 8));
for (const a of ["", "abc", "0x12", "0X12", "gg", "12 34", "1.2", "ff ", " ff",
                 "ff;ff", "\uFF11" + "2"  ])
    no(IsHex(a), "invalid hex " + JSON.stringify(a));
ok(IsHex("a".repeat(4096)), "4096 bytes is the input cap, and hex fits inside it");
throws(() => IsHex("a".repeat(4097)), "one byte over the cap is a RangeError");

for (const a of ["#fff", "#000", "#aBc123", "#ABCDEF", "#abcdef12",
                 "#FaFbFc99", "#123456", "#00000000"])
    ok(IsHexColor(a), "valid hex color " + a);
for (const a of ["", "fff", "#ff", "#ffff", "#fffffff", "#ffffFFFFFF",
                 "#gggggg", "#fff ", " #fff", "#abg", "rgb(0,0,0)", "#-fff"])
    no(IsHexColor(a), "invalid hex color " + JSON.stringify(a));

for (const a of ["0", "1", "123", "-123", "+123", "1.5", "-1.5", ".5", "5.",
                 "0.0", "007", "12345678901234567890"])
    ok(IsNumeric(a), "valid numeric " + JSON.stringify(a));
for (const a of ["", "-", "+", ".", "-.", "+.", "1.2.3", "1e5", "1E5", "abc",
                 "1 ", " 1", "1,5", "--1", "1-", "0x10", "\u00BD",
                 "\u22125"  ])
    no(IsNumeric(a), "invalid numeric " + JSON.stringify(a));

for (const a of ["2023-01-31", "2023-02-28", "2020-02-29", "2024-02-29",
                 "2000-02-29", "1999-12-31", "0000-01-01", "1970-01-01",
                 "0004-02-29"])
    ok(IsDateString(a), "valid date " + a);
for (const a of ["", "2023-02-29", "1900-02-29", "2100-02-29", "2023-13-01",
                 "2023-00-10", "2023-01-00", "2023-04-31", "2023-1-01",
                 "2023-01-1", "20230101", "2023/01/01", "2023-01-31 ",
                 " 2023-01-31", "2023-1b-01", "23-01-01"])
    no(IsDateString(a), "invalid date " + JSON.stringify(a));

for (const a of ["2023-01-15T10:30:00Z", "2023-01-15t10:30:00z",
                 "2023-12-31T23:59:60Z", "2020-02-29T00:00:00Z",
                 "2023-01-15T10:30:00+05:30", "2023-01-15T10:30:00-08:00",
                 "2023-01-15T10:30:00.123Z", "2023-01-15T10:30:00.123456789Z",
                 "2023-01-15T10:30:00-00:00", "0000-01-01T00:00:00Z",
                 "2023-01-15T10:30:00+00:00"])
    ok(IsRFC3339(a), "valid RFC3339 " + a);
for (const a of ["", "2023-01-15T10:30:00", "2023-01-15 10:30:00Z",
                 "2023-13-01T10:30:00Z", "2023-02-29T00:00:00Z",
                 "2023-01-15T24:00:00Z", "2023-01-15T10:60:00Z",
                 "2023-01-15T10:30:61Z", "2023-01-15T10:30:00+24:00",
                 "2023-01-15T10:30:00+05:60", "2023-1-15T10:30:00Z",
                 "20230115T103000Z", "2023-01-15T10:30:00.Z",
                 "2023-01-15T10:30:00Z ", "2023-01-15T10:30:00ZZ",
                 "2023-01-15T10:30:00+z:30", "2023-01-15"])
    no(IsRFC3339(a), "invalid RFC3339 " + JSON.stringify(a));

for (const a of ['{}', '[]', 'null', 'true', 'false', '"str"', '42', '-1.5e3',
                 '{"a":[1,2,{"b":null}]}', '""', '[1,2,3]'])
    ok(IsJSON(a), "valid JSON " + a);
for (const a of ['', '{', '}', "{'a':1}", 'undefined', '{"a":1,}', 'NaN',
                 '"unterminated', '01', '+1', '[1,]', "{'a':1}", '"\x01"',
                 '{a:1}', 'True'])
    no(IsJSON(a), "invalid JSON " + JSON.stringify(a));
ok(IsJSON("[".repeat(2048) + "]".repeat(2048)),
   "2048-deep nesting at the 4096-byte cap parses");

for (const a of ["a/b", "text/plain", "application/json",
                 "text/html;charset=utf-8", "text/html; charset=utf-8",
                 "application/vnd.api+json", "image/svg+xml",
                 "multipart/form-data; boundary=abc123",
                 'text/plain; charset="utf-8"',
                 'text/plain; a="say \\"hi\\""', "text/plain;a=1;b=2",
                 "text/plain ; charset=utf-8"])
    ok(IsMimeType(a), "valid mime type " + JSON.stringify(a));
for (const a of ["", "text", "/plain", "text/", "text plain", "text/plain ",
                 "text/plain;", "text/plain; ", "text/plain; =v", "text/plain; a=",
                 'text/plain; a="unterminated', 'text/plain; a="x\\"',
                 "text/plain; a=b=c", "text/pl@in", "text/pl ain",
                 "text/plain;charset=utf-8;charset=ascii", "text/plain;a",
                 "text\tplain", "text/plain;a=\u0000x"])
    no(IsMimeType(a), "invalid mime type " + JSON.stringify(a));
ok(IsMimeType("TEXT/PLAIN"), "upper case type/subtype is fine");
no(IsMimeType("text/plain;charset=utf-8;CHARSET=ascii"),
   "a parameter name repeated in another case is still refused");
ok(IsMimeType("a".repeat(127) + "/b"), "a 127-char type half is the limit");
no(IsMimeType("a".repeat(128) + "/b"), "a 128-char type half is over it");

for (const a of ["Str0ng!pass", "Abcdef1!", "Password1!", "xY3!xY3!",
                 "T3st!ng1", "Ab1!Ab1! "])
    ok(IsStrongPassword(a), "strong: " + JSON.stringify(a));
for (const a of ["", "Ab1!", "Ab1!Ab1", "abcdefgh", "ABCDEFGH", "Abcdefg1",
                 "Abcdefg!", "Password1", "aaaaaaaa1!", "AAAAAAAA1!",
                 "P@ssw\u00F6rd1"  , "a b c d e f 1"])
    no(IsStrongPassword(a), "weak: " + JSON.stringify(a));
ok(IsStrongPassword("Aa1!Aa1!  "), "a space is length, not a class");

ok(IsStrongPassword("aaaa", { minLength: 4, minLower: 4, minUpper: 0,
                              minDigits: 0, minSymbols: 0 }),
   "all-zero class minimums relax the classes");
ok(IsStrongPassword("Abcdef1!", { minLength: 8.0 }),
   "8.0 is an integer for these purposes");
ok(IsStrongPassword("aaaaaaaa", { minLower: 0, minUpper: 0, minDigits: 0,
                                  minSymbols: 0 }),
   "a partial options object only overrides what it names");
no(IsStrongPassword("aaaa", { minLength: 5, minLower: 0, minUpper: 0,
                              minDigits: 0, minSymbols: 0 }),
   "minLength is honored");
no(IsStrongPassword("abc", { minLower: 4, minLength: 3, minUpper: 0,
                             minDigits: 0, minSymbols: 0 }),
   "minLower is honored");
no(IsStrongPassword("abcdef1!", {}), "an empty options object keeps the defaults");
ok(IsStrongPassword("Abcdef1!", "not an object"), "a non-object opts is ignored");
ok(IsStrongPassword("Abcdef1!", undefined), "undefined opts keeps defaults");
ok(IsStrongPassword("abc", { minLength: 0, minLower: 0, minUpper: 0,
                             minDigits: 0, minSymbols: 0 }),
   "every minimum at zero still accepts non-empty input");
no(IsStrongPassword("", { minLength: 0, minLower: 0, minUpper: 0,
                          minDigits: 0, minSymbols: 0 }),
   "an empty string is never a strong password");
throws(() => IsStrongPassword("Abcdef1!", { minLength: NaN }),
       "a NaN minLength is a RangeError");
throws(() => IsStrongPassword("Abcdef1!", { minLength: Infinity }),
       "an Infinity minLength is a RangeError");
throws(() => IsStrongPassword("Abcdef1!", { minLength: "8" }),
       "a string minLength is a TypeError");
throws(() => IsStrongPassword("Abcdef1!", { minLength: -1 }),
       "a negative minLength is a RangeError");
throws(() => IsStrongPassword("Abcdef1!", { minLength: 1.5 }),
       "a fractional minLength is a RangeError");
throws(() => IsStrongPassword("Abcdef1!", { minLength: 5000 }),
       "an unsatisfiable minLength is a RangeError");
throws(() => IsStrongPassword(null), "a non-string text throws");
throws(() => IsStrongPassword("Abcdef1!", { get minLength() { throw new Error("x"); } }),
       "a throwing option getter propagates");

{
    try { IsUUID("123e4567-e89b-12d3-a456-426614174000"); } catch (e) {}
    try { IsSemver("1.2.3"); } catch (e) {}
    assert(globalThis.__dyna_v === undefined, "no stray __dyna_v global is created");
}
{
    const before = IsSemver("1.2.3");
    globalThis.__dyna_vx = globalThis.__dyna_vx || {};
    globalThis.__dyna_vx["dyna:semver.isValid"] = function () { return true; };
    assert(IsSemver("1.2.3") === before, "a tampered delegate table does not change IsSemver");
    assert(IsSemver("not a version") === false, "a tampered delegate cannot force a positive result");
    delete globalThis.__dyna_vx["dyna:semver.isValid"];
}
{
    const before = IsUUID("123e4567-e89b-12d3-a456-426614174000");
    Object.defineProperty(Object.prototype, "__dyna_vx_0_1", {
        get() { return () => true; }, configurable: true,
    });
    try {
        assert(IsUUID("not-a-uuid") === false, "a planted bridge accessor cannot forge IsUUID");
        assert(IsUUID("123e4567-e89b-12d3-a456-426614174000") === before, "IsUUID still accepts a valid uuid");
    } finally {
        delete Object.prototype.__dyna_vx_0_1;
    }
}

{
    const N = 200000;
    const t0 = Date.now();
    for (let i = 0; i < N; i++) IsSemver("1.2.3");
    const tSemver = Date.now() - t0;
    const t1 = Date.now();
    for (let i = 0; i < N; i++) IsEmail("a@b.co");
    const tEmail = Date.now() - t1;
    assert(tSemver < 400,
        "IsSemver x" + N + " must stay on the cached delegate (" + tSemver + " ms)");
    assert(tSemver < tEmail * 15 + 150,
        "IsSemver must not recompile its module per call (" + tSemver +
        " ms vs IsEmail " + tEmail + " ms)");
}

for (const fn of [IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii,
                  IsIPv4, IsIPv6, IsIP, IsPort, IsBase64, IsBase64Url, IsHex,
                  IsHexColor, IsNumeric, IsDateString, IsRFC3339, IsJSON,
                  IsMimeType, IsStrongPassword]) {
    for (const bad of [null, undefined, 42, {}, []])
        throws(() => fn(bad), "a validator refuses " + JSON.stringify(bad));
    throws(() => fn("x".repeat(5000)), "and refuses an over-long input");
}

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
