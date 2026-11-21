/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:validate
/** dyna:validate — boolean predicates for common formats: email, URL, UUID, IP, credit card, IBAN, JWT shape, base64, hex, JSON, MIME, RFC 3339, strong passwords. */
declare module "dyna:validate" {
    /** ASCII letters only. */
    function IsAlpha(text: string): boolean;
    /** ASCII letters and digits. */
    function IsAlphanumeric(text: string): boolean;
    /** Non-empty text whose every byte is below 0x80; IsAscii("") is false. */
    function IsAscii(text: string): boolean;
    /** The practical email grammar; quoted strings and comments are refused. */
    function IsEmail(text: string): boolean;
    /** Luhn check digit over 12..19 digits. */
    function IsCreditCard(text: string): boolean;
    /** Country-length check plus mod-97 over the rearranged digits. */
    function IsIBAN(text: string): boolean;
    /** RFC 1035 label grammar; an IP literal is not a domain. */
    function IsDomain(text: string): boolean;
    /** The dyna:url constructor accepts it, with a non-empty host for special schemes. */
    function IsURL(text: string): boolean;
    /** Lowercase letters, digits, single hyphens; at most 64 chars. */
    function IsSlug(text: string): boolean;
    /** The RFC 4122 canonical 8-4-4-4-12 form only. */
    function IsUUID(text: string): boolean;
    /** JWS Compact Serialization with a JSON header naming an alg; opts.algs (array) also requires that alg in the list.
     *  Unknown opts keys or a non-object opts throw TypeError; a throwing opts.algs read or length is rethrown. */
    function IsJWT(text: string, opts?: { algs?: string[] }): boolean;
    /** The dyna:semver parser accepts it. */
    function IsSemver(text: string): boolean;
    /** ITU-T E.164: optional `+`, 8..15 digits. */
    function IsE164(text: string): boolean;
    /** Strict dotted-quad: four decimal octets 0..255, no leading zeros, no whitespace. */
    function IsIPv4(text: string): boolean;
    /** RFC 4291 text grammar incl. `::` compression (once) and an embedded IPv4 tail; no zone ids. */
    function IsIPv6(text: string): boolean;
    /** IsIPv4 or IsIPv6. */
    function IsIP(text: string): boolean;
    /** Decimal 0..65535, no sign, no leading zeros; "0" is allowed (a kernel-assigned port). */
    function IsPort(text: string): boolean;
    /** RFC 4648 canonical base64: `+/` alphabet, proper padding, zero pad bits. */
    function IsBase64(text: string): boolean;
    /** RFC 4648 sec.5 base64url: the `-_` alphabet, unpadded and canonical (zero pad bits). */
    function IsBase64Url(text: string): boolean;
    /** An even-length run of hex digits -- the shape a byte string must have. */
    function IsHex(text: string): boolean;
    /** `#RGB`, `#RRGGBB` or `#RRGGBBAA`; the `#` is required. */
    function IsHexColor(text: string): boolean;
    /** JSON.parse accepts it (any JSON value; input cap 4096 bytes). */
    function IsJSON(text: string): boolean;
    /** RFC 6838 simplified: token type/subtype, optional `; name=value` parameters. */
    function IsMimeType(text: string): boolean;
    /** Full RFC 3339 date-time: `T` separator, `Z` or `+/-HH:MM` offset; the leap second `:60` is allowed. */
    function IsRFC3339(text: string): boolean;
    /** Strict ISO calendar date YYYY-MM-DD with real month/day validation (leap years included). */
    function IsDateString(text: string): boolean;
    /** Optional sign, digits, at most one decimal point; exponent notation is refused. */
    function IsNumeric(text: string): boolean;
    /** Printable ASCII with class minimums; defaults 8/1/1/1/1; symbols are printable non-alphanumeric ASCII. */
    function IsStrongPassword(text: string, opts?: {
        minLength?: number;
        minLower?: number;
        minUpper?: number;
        minDigits?: number;
        minSymbols?: number;
    }): boolean;
}
