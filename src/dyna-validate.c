/* dyna:validate -- format validators with a check digit or a real grammar.
   NOT dyna:schema: that name is reserved for the JSON Schema engine, and using
   it here would promise one. Full API: see the dyna:* module in dyna-libc.h. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/dyn-codec.h"      /* base64url decode, the JWT shape check */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* A validator answers a question about one field, not a document. */
#define DYN_V_MAX 4096

/* Borrow the argument's bytes, or -1 having thrown. */
static int dyn_v_arg(JSContext *ctx, int argc, JSValueConst *argv,
                     const char *fn, const char **s, size_t *n)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "%s(text): argument must be a string", fn);
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, argv[0]);
    if (!*s)
        return -1;
    if (*n > DYN_V_MAX) {
        JS_FreeCString(ctx, *s);
        JS_ThrowRangeError(ctx, "%s(text): input exceeds %d bytes", fn, DYN_V_MAX);
        return -1;
    }
    return 0;
}

static int dyn_v_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int dyn_v_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* --------------------------------------------------------------- e-mail */

/* RFC 5322 atext, as a TABLE. It was `strchr("!#$...", c)`, and strchr(set, 0)
   returns a pointer to the set's own terminator -- so an embedded NUL passed
   validation and would truncate in any C consumer downstream. */
static const uint8_t DYN_V_ATEXT[256] = {
    ['!']=1,['#']=1,['$']=1,['%']=1,['&']=1,['\'']=1,['*']=1,['+']=1,['-']=1,
    ['/']=1,['=']=1,['?']=1,['^']=1,['_']=1,['`']=1,['{']=1,['|']=1,['}']=1,
    ['~']=1,
};

/* RFC 2045 token characters (an atext minus the tspecials: '/' and '=' and
   friends are reserved for the media-type grammar itself), as a TABLE for the
   same reason atext is one. */
static const uint8_t DYN_V_MIMETOK[256] = {
    ['!']=1,['#']=1,['$']=1,['%']=1,['&']=1,['\'']=1,['*']=1,['+']=1,['-']=1,
    ['.']=1,['^']=1,['_']=1,['`']=1,['{']=1,['|']=1,['}']=1,['~']=1,
};

/* The practical grammar, not RFC 5322's: one unquoted local part, one dotted
   domain with a letters-only TLD. RFC 5322 accepts comments, quoted strings and
   nested folding that no mail system round-trips, so matching it exactly would
   accept addresses that bounce. */
static int dyn_v_email(const char *s, size_t n)
{
    size_t at = (size_t)-1, i, dot = (size_t)-1, dn, lab = 0;
    if (n < 3 || n > 254)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] == '@') {
            if (at != (size_t)-1)
                return 0;                     /* a second @ is not an address */
            at = i;
        }
    if (at == (size_t)-1 || at == 0 || at + 1 >= n)
        return 0;
    if (at > 64)
        return 0;                             /* local part limit, RFC 5321 */
    if (s[0] == '.' || s[at - 1] == '.')
        return 0;
    for (i = 0; i < at; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (i + 1 < at && s[i + 1] == '.')
                return 0;                     /* no consecutive dots */
            continue;
        }
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && !DYN_V_ATEXT[c])
            return 0;
    }
    dn = n - at - 1;
    if (dn < 3 || dn > 253)
        return 0;
    if (s[at + 1] == '.' || s[at + 1] == '-' || s[n - 1] == '.' || s[n - 1] == '-')
        return 0;
    for (i = at + 1; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (i + 1 < n && s[i + 1] == '.')
                return 0;
            if (i > at + 1 && s[i - 1] == '-')
                return 0;
            if (lab > 63)
                return 0;                     /* RFC 1035: a label is at most 63 */
            lab = 0;
            dot = i;
            continue;
        }
        lab++;
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && c != '-')
            return 0;
        if (c == '-' && s[i - 1] == '.')
            return 0;                         /* a label does not begin with '-' */
    }
    if (lab > 63)
        return 0;                             /* the TLD is a label too */
    if (dot == (size_t)-1 || n - dot - 1 < 2)
        return 0;                             /* a TLD is at least two chars */
    for (i = dot + 1; i < n; i++)
        if (!dyn_v_alpha((unsigned char)s[i]))
            return 0;                         /* and letters only */
    return 1;
}

/* ----------------------------------------------------------------- Luhn */

/* The card-number check digit. It catches a single mistyped digit and most
   transpositions -- it says nothing about whether the card exists. */
static int dyn_v_luhn(const char *s, size_t n)
{
    int sum = 0, alt = 0, digits = 0;
    size_t i = n;
    while (i-- > 0) {
        unsigned char c = (unsigned char)s[i];
        int d;
        if (c == ' ' || c == '-')
            continue;
        if (!dyn_v_digit(c))
            return 0;
        d = c - '0';
        if (alt) {
            d *= 2;
            if (d > 9) d -= 9;
        }
        sum += d;
        alt = !alt;
        digits++;
    }
    if (digits < 12 || digits > 19)
        return 0;
    return (sum % 10) == 0;
}

/* ----------------------------------------------------------------- IBAN */

/* Length per country, from the IBAN registry. An IBAN of the wrong length for
   its country is invalid even when the check digits happen to work out. */
typedef struct { char cc[2]; uint8_t len; } dyn_iban_t;
static const dyn_iban_t DYN_IBAN[] = {
    {{'A','D'},24},{{'A','E'},23},{{'A','F'},28},{{'A','L'},28},{{'A','T'},20},{{'A','Z'},28},
    {{'B','A'},20},{{'B','E'},16},{{'B','G'},22},{{'B','H'},22},{{'B','I'},27},
    {{'B','R'},29},{{'B','Y'},28},{{'C','H'},21},{{'C','R'},22},{{'C','Y'},28},
    {{'C','Z'},24},{{'D','E'},22},{{'D','J'},27},{{'D','K'},18},{{'D','O'},28},
    {{'E','E'},20},{{'E','G'},29},{{'E','S'},24},{{'F','I'},18},{{'F','O'},18},
    {{'F','R'},27},{{'G','B'},22},{{'G','E'},22},{{'G','I'},23},{{'G','L'},18},
    {{'G','R'},27},{{'G','T'},28},{{'H','R'},21},{{'H','U'},28},{{'I','E'},22},
    {{'I','L'},23},{{'I','Q'},23},{{'I','R'},26},{{'I','S'},26},{{'I','T'},27},
    {{'J','O'},30},{{'K','W'},30},{{'K','Z'},20},{{'L','B'},28},{{'L','C'},32},
    {{'L','I'},21},{{'L','T'},20},{{'L','U'},20},{{'L','V'},21},{{'L','Y'},25},
    {{'M','C'},27},{{'M','D'},24},{{'M','E'},22},{{'M','K'},19},{{'M','N'},20},
    {{'M','R'},27},{{'M','T'},31},{{'M','U'},30},{{'N','I'},28},{{'N','L'},18},{{'N','O'},15},
    {{'O','M'},23},{{'P','K'},24},{{'P','L'},28},{{'P','S'},29},{{'P','T'},25},
    {{'Q','A'},29},{{'R','O'},24},{{'R','S'},22},{{'R','U'},33},{{'S','A'},24},
    {{'S','C'},31},{{'S','D'},18},{{'S','E'},24},{{'S','I'},19},{{'S','K'},24},
    {{'S','M'},27},{{'S','O'},23},{{'S','T'},25},{{'S','V'},28},{{'T','L'},23},{{'T','N'},24},{{'T','R'},26},
    {{'U','A'},29},{{'V','A'},22},{{'V','G'},24},{{'X','K'},20},
};

/* mod-97 over the rearranged digits, folded incrementally: the expanded number
   is up to 70 digits and does not fit any integer type. */
static int dyn_v_iban(const char *s, size_t n)
{
    char buf[64];
    size_t k = 0, i;
    unsigned rem = 0;
    int len = 0;

    for (i = 0; i < n; i++) {                 /* strip the separators banks print */
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '-')
            continue;
        if (k >= sizeof buf)
            return 0;
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        buf[k++] = (char)c;
    }
    if (k < 15 || k > 34)
        return 0;
    if (!dyn_v_alpha((unsigned char)buf[0]) || !dyn_v_alpha((unsigned char)buf[1]))
        return 0;
    if (!dyn_v_digit((unsigned char)buf[2]) || !dyn_v_digit((unsigned char)buf[3]))
        return 0;
    for (i = 0; i < countof(DYN_IBAN); i++)
        if (DYN_IBAN[i].cc[0] == buf[0] && DYN_IBAN[i].cc[1] == buf[1]) {
            len = DYN_IBAN[i].len;
            break;
        }
    if (!len || (size_t)len != k)
        return 0;                             /* unknown country, or wrong length */
    for (i = 4; i < k; i++)
        if (!dyn_v_alpha((unsigned char)buf[i]) && !dyn_v_digit((unsigned char)buf[i]))
            return 0;
    /* the first four characters move to the end, letters become 10..35 */
    for (i = 0; i < k; i++) {
        unsigned char c = (unsigned char)buf[(i + 4) % k];
        if (dyn_v_digit(c)) {
            rem = (rem * 10 + (unsigned)(c - '0')) % 97;
        } else {
            unsigned v = (unsigned)(c - 'A') + 10;
            rem = (rem * 100 + v) % 97;
        }
    }
    return rem == 1;
}

/* ----------------------------------------------------------------- IPv4 */

/* Strict dotted-quad: exactly four decimal fields, 0..255, no leading zeros
   ("127.000.000.001" is refused outright -- some resolvers read it as octal,
   so its meaning is ambiguous, and a validator exists to refuse ambiguity).
   Byte-oriented on the UTF-8 form; any byte >= 0x80 fails the digit class. */
static int dyn_v_ipv4(const char *s, size_t n)
{
    size_t i = 0, f;

    if (n < 7 || n > 15)                  /* 0.0.0.0 .. 255.255.255.255 */
        return 0;
    for (f = 0; f < 4; f++) {
        size_t start = i, d;
        int v = 0;
        while (i < n && dyn_v_digit((unsigned char)s[i]))
            i++;
        d = i - start;
        if (d == 0 || d > 3)
            return 0;                     /* an empty or 4-digit field */
        if (d > 1 && s[start] == '0')
            return 0;                     /* a leading zero ("01", "001") */
        for (d = start; d < i; d++)
            v = v * 10 + (s[d] - '0');
        if (v > 255)
            return 0;
        if (f < 3) {
            if (i >= n || s[i] != '.')
                return 0;
            i++;
        }
    }
    return i == n;                        /* nothing may trail the 4th field */
}

/* ----------------------------------------------------------------- IPv6 */

/* A group is 1..4 hex digits. Leading zeros inside a group ARE grammar-legal
   (RFC 4291 only says they MAY be suppressed; suppressing them is RFC 5952's
   canonical-form business, not a validity question). */
static int dyn_v_ipv6_group(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || n > 4)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_digit(c) && (c < 'a' || c > 'f') && (c < 'A' || c > 'F'))
            return 0;
    }
    return 1;
}

/* RFC 4291 sec.2.2: eight hex groups, `::` standing for ONE OR MORE all-zero
   groups (so it appears at most once, and when it appears at most seven
   groups may be written -- "::1:2:3:4:5:6:7:8" is nine groups and refused),
   and the embedded-IPv4 tail x:x:x:x:x:x:d.d.d.d only ever LAST, counting as
   two groups with the same strict leading-zero rule as IsIPv4. A zone id
   ("fe80::1%eth0") is interface metadata, not address text, and is refused.
   "::" alone is the unspecified address and is valid. */
static int dyn_v_ipv6(const char *s, size_t n)
{
    size_t i = 0, groups = 0;
    int compressed = 0;

    if (n < 2 || n > 45)                  /* "::" .. 0000:...:255.255.255.255 */
        return 0;
    if (s[0] == ':') {
        if (s[1] != ':')
            return 0;                     /* a lone leading colon */
        compressed = 1;
        i = 2;
    }
    while (i < n) {
        size_t start = i;
        int dots = 0;
        while (i < n && s[i] != ':') {
            if (s[i] == '.')
                dots = 1;
            i++;
        }
        if (dots) {                       /* the IPv4 tail, or nothing else */
            if (i != n)
                return 0;                 /* it is only ever the last group */
            if (!dyn_v_ipv4(s + start, i - start))
                return 0;
            groups += 2;                  /* an IPv4 tail is two groups */
            break;
        }
        if (!dyn_v_ipv6_group(s + start, i - start))
            return 0;
        groups++;
        if (i < n) {                      /* a group ends only at a colon */
            i++;
            if (i < n && s[i] == ':') {
                if (compressed)
                    return 0;             /* `::` may appear once */
                compressed = 1;
                i++;
            } else if (i == n) {
                return 0;                 /* a trailing single colon */
            }
        }
    }
    if (compressed)
        return groups <= 7;               /* `::` stands for >= 1 zero group */
    return groups == 8;
}

/* ------------------------------------------------------------------ port */

/* A TCP/UDP port as a decimal string: 0..65535, no sign, no leading zeros.
   "0" IS allowed -- port 0 is a real value (bind it and the kernel assigns an
   ephemeral port), so it is not treated as an obfuscated "00". */
static int dyn_v_port(const char *s, size_t n)
{
    size_t i;
    unsigned v = 0;

    if (n == 0 || n > 5)
        return 0;
    if (n > 1 && s[0] == '0')
        return 0;
    for (i = 0; i < n; i++) {
        if (!dyn_v_digit((unsigned char)s[i]))
            return 0;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    return v <= 65535;
}

/* ---------------------------------------------------------------- base64 */

/* A base64 digit's 6-bit value, or -1 outside the standard alphabet. */
static int dyn_v_b64val(unsigned char c)
{
    if (dyn_v_alpha(c))
        return c <= 'Z' ? c - 'A' : c - 'a' + 26;
    if (dyn_v_digit(c))
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* RFC 4648 base64, CANONICAL: the standard alphabet (+/), '=' only as the
   final one or two characters, and -- the part casual checks skip -- the pad
   bits zero, as a canonical encoder leaves them: "AR==" decodes to a byte,
   but 'R' carries non-zero discarded bits, so what it encodes depends on the
   decoder's leniency and a validator refuses it. Unpadded input is refused:
   the unpadded spelling is base64url's, and accepting both here would make
   the two validators interchangeable when they are not. */
static int dyn_v_base64(const char *s, size_t n)
{
    size_t i, core;

    if (n == 0 || (n % 4) != 0)
        return 0;                         /* canonical base64 always pads */
    core = n;
    while (core > 0 && s[core - 1] == '=')
        core--;
    if (n - core > 2)
        return 0;                         /* '=' never carries data */
    for (i = 0; i < core; i++)
        if (dyn_v_b64val((unsigned char)s[i]) < 0)
            return 0;
    if (core < n) {
        int v = dyn_v_b64val((unsigned char)s[core - 1]);
        if (n - core == 2 && (v & 0x0f))  /* 2 pads: 4 discarded bits */
            return 0;
        if (n - core == 1 && (v & 0x03))  /* 1 pad: 2 discarded bits */
            return 0;
    }
    return 1;
}

/* RFC 4648 sec.5 base64url, UNPADDED (the RFC 7515 rule JWT segments follow):
   the alternative alphabet -_, and no '=' anywhere. CANONICAL, exactly like
   IsBase64: no encoder emits a final lone character (length % 4 == 1 would
   carry two stray bits), and the bits a 2- or 3-character final quantum
   wastes are zero -- "QQR" decodes, but which byte it ends in depends on the
   decoder's leniency, so a validator refuses it. Empty is false -- every
   predicate here answers a question about data-bearing strings. */
static int dyn_v_base64url(const char *s, size_t n)
{
    size_t i;
    int v;

    if (n == 0)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && c != '-' && c != '_')
            return 0;
    }
    if ((n % 4) == 1)
        return 0;                     /* a lone final character is noise */
    if ((n % 4) == 2) {
        v = dyn_v_b64val((unsigned char)s[n - 1]);
        if (v & 0x0f)                 /* 4 discarded bits must be zero */
            return 0;
    } else if ((n % 4) == 3) {
        v = dyn_v_b64val((unsigned char)s[n - 1]);
        if (v & 0x03)                 /* 2 discarded bits must be zero */
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ hex */

/* An even-length run of hex digits -- the shape a byte string must have; an
   odd one is refused because no nibble-oriented consumer can eat it. */
static int dyn_v_hex(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || (n % 2) != 0)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_digit(c) && (c < 'a' || c > 'f') && (c < 'A' || c > 'F'))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------- hex color */

/* '#', then 3, 6 or 8 hex digits: the #RGB, #RRGGBB and #RRGGBBAA forms.
   The '#' is REQUIRED (named colors are not hex and this is not a color
   parser), and the 4-digit #RGBA short form is not accepted -- spell the
   alpha out. Case-insensitive, like CSS. */
static int dyn_v_hexcolor(const char *s, size_t n)
{
    size_t i;

    if (n != 4 && n != 7 && n != 9)
        return 0;
    if (s[0] != '#')
        return 0;
    for (i = 1; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_digit(c) && (c < 'a' || c > 'f') && (c < 'A' || c > 'F'))
            return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- numeric */

/* Optional sign, then digits with at most one decimal point and at least one
   digit somewhere -- so ".5" and "5." pass, "" "-" and "." do not. EXPONENT
   NOTATION IS REFUSED on purpose: "1e5" is a calculator's spelling, and this
   predicate screens ledger/spreadsheet-style input where the literal decimal
   form is what survives a CSV round-trip. */
static int dyn_v_numeric(const char *s, size_t n)
{
    size_t i = 0, digits = 0, dots = 0;

    if (i < n && (s[i] == '+' || s[i] == '-'))
        i++;
    for (; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (++dots > 1)
                return 0;
            continue;
        }
        if (!dyn_v_digit(c))
            return 0;
        digits++;
    }
    return digits > 0;                    /* a lone sign is not a number */
}

/* ------------------------------------------------------------ date/time */

/* Proleptic Gregorian: a leap day exists only in leap years. Year 0000 is a
   leap year by the same rule and stays valid -- the predicate is about the
   calendar, not about which years have happened. */
static int dyn_v_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* Strict ISO calendar date: YYYY-MM-DD, exactly ten characters, with the
   month and day ranges actually checked (leap years included). */
static int dyn_v_datestring(const char *s, size_t n)
{
    static const int MDAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y, m, d;
    size_t i;

    if (n != 10)
        return 0;
    for (i = 0; i < n; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return 0;
        } else if (!dyn_v_digit((unsigned char)s[i])) {
            return 0;
        }
    }
    y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    m = (s[5]-'0')*10 + (s[6]-'0');
    d = (s[8]-'0')*10 + (s[9]-'0');
    if (m < 1 || m > 12 || d < 1)
        return 0;
    if (m == 2 && dyn_v_leap(y))
        return d <= 29;
    return d <= MDAYS[m - 1];
}

/* RFC 3339 date-time: full-date "T" full-time. The deliberate choices:
   - lowercase "t"/"z" accepted -- RFC 3339's ABNF literals are
     case-insensitive; a SPACE separator is not (that spelling needs "further
     agreement" per sec.5.6, and a validator should not guess agreements);
   - ":60" is allowed at any date (the leap second, sec.5.7 permits it; which
     dates carried one is an ephemeris question, not a grammar one);
   - the day must exist, hours are 00-23, minutes 00-59;
   - the offset is "Z"/"z" or +/-HH:MM with HH 00-23, MM 00-59; "-00:00" is
     allowed (RFC 3339 gives it a distinct meaning);
   - a fractional second ("." 1*digit) may follow the seconds. */
static int dyn_v_rfc3339(const char *s, size_t n)
{
    size_t i;
    int h, mi, sec;

    if (n < 20)                           /* 0000-01-01T00:00:00Z */
        return 0;
    if (!dyn_v_datestring(s, 10))
        return 0;
    if (s[10] != 'T' && s[10] != 't')
        return 0;
    for (i = 11; i < 19; i++) {
        if (i == 13 || i == 16) {
            if (s[i] != ':')
                return 0;
        } else if (!dyn_v_digit((unsigned char)s[i])) {
            return 0;
        }
    }
    h   = (s[11]-'0')*10 + (s[12]-'0');
    mi  = (s[14]-'0')*10 + (s[15]-'0');
    sec = (s[17]-'0')*10 + (s[18]-'0');
    if (h > 23 || mi > 59 || sec > 60)
        return 0;
    i = 19;
    if (i < n && s[i] == '.') {           /* time-secfrac */
        size_t fd = 0;
        i++;
        while (i < n && dyn_v_digit((unsigned char)s[i])) {
            i++;
            fd++;
        }
        if (fd == 0)
            return 0;                     /* a bare '.' is not a fraction */
    }
    if (i < n && (s[i] == 'Z' || s[i] == 'z'))
        return i + 1 == n;
    if (i + 6 != n || (s[i] != '+' && s[i] != '-'))
        return 0;
    if (!dyn_v_digit((unsigned char)s[i+1]) ||
        !dyn_v_digit((unsigned char)s[i+2]) || s[i+3] != ':' ||
        !dyn_v_digit((unsigned char)s[i+4]) ||
        !dyn_v_digit((unsigned char)s[i+5]))
        return 0;
    h  = (s[i+1]-'0')*10 + (s[i+2]-'0');
    mi = (s[i+4]-'0')*10 + (s[i+5]-'0');
    return h <= 23 && mi <= 59;
}

/* ------------------------------------------------------------ media type */

/* A media-type name run: the RFC 6838 restricted-name charset (letters,
   digits, !#$&-^_.+) plus the rest of the RFC 2045 token extras (%'*`{|}~),
   1..127 of them (RFC 2045 bounds a field at 127 octets). This is the charset
   both halves of "type/subtype" and every parameter name and value token
   obeys here. */
static int dyn_v_mime_token(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || n > 127)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && !DYN_V_MIMETOK[c])
            return 0;
    }
    return 1;
}

/* ASCII-only case-fold for the parameter-name duplicate check; MIME has no
   business pulling in a locale. */
static int dyn_v_mime_names_equal(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z')
            x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (unsigned char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return 1;
}

/* RFC 6838 simplified to the subset real Content-Type headers use:
   - "type/subtype", each half from the restricted name charset
     (dyn_v_mime_token; case-insensitive per the RFC, so any case passes);
     no whitespace may touch the halves;
   - optional parameters "; name=value": the name is a token and may not
     repeat, the value is a token or a quoted string with backslash escapes;
   - linear whitespace is allowed around the ';' separators, NOWHERE else;
   - no folding, comments, or RFC 2231 continuations. */
static int dyn_v_mime(const char *s, size_t n)
{
    const char *np[64];
    size_t nl[64], nn = 0, k, i = 0, start;

    if (n < 3 || n > 255)
        return 0;
    start = i;
    while (i < n && s[i] != '/' && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
        i++;
    if (i >= n || s[i] != '/')
        return 0;                         /* the '/' is not optional */
    if (!dyn_v_mime_token(s + start, i - start))
        return 0;
    i++;                                  /* past the '/' */
    start = i;
    while (i < n && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
        i++;
    if (!dyn_v_mime_token(s + start, i - start))
        return 0;
    while (i < n) {                       /* parameters, "; name=value" */
        size_t plen, w, vstart;
        if (s[i] == ' ' || s[i] == '\t') {
            while (i < n && (s[i] == ' ' || s[i] == '\t'))
                i++;
            if (i >= n || s[i] != ';')
                return 0;                 /* LWSP leads nowhere: it touches
                                             the subtype, which is refused */
        }
        if (s[i] != ';')
            return 0;
        i++;
        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            i++;
        if (i >= n)
            return 0;                     /* a ';' must introduce a parameter */
        start = i;
        while (i < n && s[i] != '=' && s[i] != ';' &&
               s[i] != ' ' && s[i] != '\t')
            i++;
        plen = i - start;
        if (!dyn_v_mime_token(s + start, plen))
            return 0;
        for (k = 0; k < nn; k++)
            if (nl[k] == plen && dyn_v_mime_names_equal(np[k], s + start, plen))
                return 0;                 /* a parameter name repeats (MIME
                                             parameter names are case-
                                             insensitive, RFC 2045 sec.5.1) */
        w = i;
        while (w < n && (s[w] == ' ' || s[w] == '\t'))
            w++;
        if (w >= n || s[w] != '=')
            return 0;                     /* name= is not optional */
        if (nn < countof(np)) {
            np[nn] = s + start;
            nl[nn] = plen;
            nn++;
        }
        i = w + 1;                        /* past the '='; no LWSP after it */
        if (i >= n)
            return 0;
        if (s[i] == '"') {                /* quoted-string, RFC 2045 qdtext */
            i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\') {
                    if (i + 1 >= n)
                        return 0;         /* a trailing backslash escapes nothing */
                    i++;                  /* the escaped character is opaque */
                } else if ((unsigned char)s[i] < 0x20 ||
                           (unsigned char)s[i] == 0x7f) {
                    return 0;             /* no control characters */
                }
                i++;
            }
            if (i >= n)
                return 0;                 /* unterminated */
            i++;                          /* past the closing quote */
            if (i < n && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
                return 0;                 /* garbage after the value */
        } else {
            vstart = i;
            while (i < n && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
                i++;
            if (!dyn_v_mime_token(s + vstart, i - vstart))
                return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------ char classes */

enum { V_ALPHA, V_ALNUM, V_ASCII, V_EMAIL, V_LUHN, V_IBAN,
       V_IPV4, V_IPV6, V_IP, V_PORT, V_BASE64, V_BASE64URL, V_HEX,
       V_HEXCOLOR, V_NUMERIC, V_DATESTRING, V_RFC3339, V_MIME };

static JSValue dyn_v_check(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    static const char *const NAMES[] = {
        "IsAlpha", "IsAlphanumeric", "IsAscii", "IsEmail", "IsCreditCard", "IsIBAN",
        "IsIPv4", "IsIPv6", "IsIP", "IsPort", "IsBase64", "IsBase64Url",
        "IsHex", "IsHexColor", "IsNumeric", "IsDateString", "IsRFC3339", "IsMimeType"
    };
    const char *s;
    size_t n, i;
    int ok = 1;

    if (dyn_v_arg(ctx, argc, argv, NAMES[magic], &s, &n) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case V_EMAIL: ok = dyn_v_email(s, n); break;
    case V_LUHN:  ok = dyn_v_luhn(s, n);  break;
    case V_IBAN:  ok = dyn_v_iban(s, n);  break;
    case V_IPV4:  ok = dyn_v_ipv4(s, n);  break;
    case V_IPV6:  ok = dyn_v_ipv6(s, n);  break;
    case V_IP:    ok = dyn_v_ipv4(s, n) || dyn_v_ipv6(s, n); break;
    case V_PORT:  ok = dyn_v_port(s, n);  break;
    case V_BASE64:    ok = dyn_v_base64(s, n);    break;
    case V_BASE64URL: ok = dyn_v_base64url(s, n); break;
    case V_HEX:       ok = dyn_v_hex(s, n);       break;
    case V_HEXCOLOR:  ok = dyn_v_hexcolor(s, n);  break;
    case V_NUMERIC:   ok = dyn_v_numeric(s, n);   break;
    case V_DATESTRING: ok = dyn_v_datestring(s, n); break;
    case V_RFC3339:    ok = dyn_v_rfc3339(s, n);  break;
    case V_MIME:       ok = dyn_v_mime(s, n);     break;
    default:
        /* An empty string satisfies no class: "is it all letters" over nothing
           is a question with no useful yes. */
        if (n == 0) { ok = 0; break; }
        for (i = 0; i < n && ok; i++) {
            unsigned char c = (unsigned char)s[i];
            if (magic == V_ALPHA)      ok = dyn_v_alpha(c);
            else if (magic == V_ALNUM) ok = dyn_v_alpha(c) || dyn_v_digit(c);
            else                       ok = c < 0x80;
        }
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

/* ------------------------------------------------------- module bridge */

/* The parsers behind IsURL/IsDomain/IsUUID/IsSemver are static inside their
   own modules, so the module's JS exports are the entry points: a one-import
   bridge module hands the export over. The cache lives in a single well-known
   holder ON globalThis -- never in a C static: a static JSValue is a leaked
   refcount at JS_FreeRuntime (the gc_obj_list assert), and a global keyed on
   nothing breaks worker contexts. These validators REQUIRE dyna:url/net/uuid/
   semver to be registered; a build without one throws the bridge's named
   error on first use. */

#define DYN_V_HOLDER "__dyna_vx"

/* Evaluate `body` as a module; it stores its result in the global holder
   (__dyna_vx, below) under `key`. Returns the stored slot. */
static JSValue dyn_v_bridge(JSContext *ctx, const char *body, const char *key)
{
    JSValue r, v, g;

    r = JS_Eval(ctx, body, strlen(body), "<dyna:validate>",
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r))
        return r;
    v = JS_EvalFunction(ctx, r);       /* consumes r; runs the import graph */
    if (JS_IsException(v))
        return v;
    if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
        JSValue err = JS_DupValue(ctx, JS_PromiseResult(ctx, v));
        JS_FreeValue(ctx, v);
        return JS_Throw(ctx, err);      /* a module we depend on is missing */
    }
    JS_FreeValue(ctx, v);
    g = JS_GetGlobalObject(ctx);
    v = JS_GetPropertyStr(ctx, g, DYN_V_HOLDER);
    JS_FreeValue(ctx, g);
    if (JS_IsObject(v)) {
        JSValue slot = JS_GetPropertyStr(ctx, v, key);
        JS_FreeValue(ctx, v);
        return slot;
    }
    return v;
}

/* Read the cached export for `slotname` from the global holder, or
   JS_UNDEFINED (missing holder or missing slot). */
static JSValue dyn_v_slot_get(JSContext *ctx, const char *slotname)
{
    JSValue g, holder, v;
    g = JS_GetGlobalObject(ctx);
    holder = JS_GetPropertyStr(ctx, g, DYN_V_HOLDER);
    JS_FreeValue(ctx, g);
    if (!JS_IsObject(holder)) {
        JS_FreeValue(ctx, holder);
        return JS_UNDEFINED;
    }
    v = JS_GetPropertyStr(ctx, holder, slotname);
    JS_FreeValue(ctx, holder);
    return v;
}

/* Fetch `fnname` from `module`, bridging on first use. The cache entry is a
   property of the global holder KEYED BY "module.fnname" (bare fnname
   collides: isValid exists in both dyna:net and dyna:semver), so
   JS_FreeContext releases the refcount. */
static JSValue dyn_v_export(JSContext *ctx, const char *module,
                            const char *fnname)
{
    char src[192], key[96];
    JSValue v;

    snprintf(key, sizeof key, "%s.%s", module, fnname);
    v = dyn_v_slot_get(ctx, key);
    if (JS_IsFunction(ctx, v))
        return v;
    JS_FreeValue(ctx, v);
    snprintf(src, sizeof src,
             "import * as m from \"%s\";\n"
             "globalThis." DYN_V_HOLDER " = globalThis." DYN_V_HOLDER " || {};\n"
             "globalThis." DYN_V_HOLDER "[\"%s\"] = m.%s;\n",
             module, key, fnname);
    v = dyn_v_bridge(ctx, src, key);
    if (JS_IsException(v))
        return v;
    if (!JS_IsFunction(ctx, v)) {
        JSValue e = v;
        v = JS_ThrowTypeError(ctx, "dyna:validate: %s has no export %s",
                              module, fnname);
        JS_FreeValue(ctx, e);
        return v;
    }
    return v;
}

/* Call a module export with one string argument: 1/0 from JS_ToBool, -1 on a
   bridge, allocation or DELEGATE failure. The validators never throw on
   content, so an exception surfacing from here is a real bug (or a bridged
   module's own throw) and propagates -- swallowing it into `false` would make
   a broken delegate indistinguishable from invalid input. */
static int dyn_v_js_bool(JSContext *ctx, const char *module,
                         const char *fnname, const char *s, size_t n)
{
    JSValue fn, arg, v;
    JSValueConst a1[1];
    int ok;

    fn = dyn_v_export(ctx, module, fnname);
    if (JS_IsException(fn))
        return -1;
    arg = JS_NewStringLen(ctx, s, n);
    if (JS_IsException(arg)) {
        JS_FreeValue(ctx, fn);
        return -1;
    }
    a1[0] = arg;
    v = JS_Call(ctx, fn, JS_UNDEFINED, 1, a1);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(v))
        return -1;                        /* the pending exception propagates */
    ok = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return ok;
}

/* ---------------------------------------------------------------- slug */

/* A slug is a URL-label-shaped token: lowercase letters, digits and single
   hyphens. No RFC; the 64-char bound keeps it inside any URL label. */
static int dyn_v_slug(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || n > 64)
        return 0;
    if (s[0] == '-')
        return 0;                         /* no leading hyphen */
    if (s[n - 1] == '-')
        return 0;                         /* no trailing hyphen */
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z')
            continue;
        if (dyn_v_digit(c))
            continue;
        if (c == '-') {
            if (i + 1 < n && s[i + 1] == '-')
                return 0;                 /* no double hyphens */
            continue;
        }
        return 0;                         /* anything else is not a slug */
    }
    return 1;
}

/* ----------------------------------------------------------------- E.164 */

/* ITU-T E.164 sec.3.1: an optional '+' and at most 15 digits, nothing else.
   The 8-digit floor is deliberate: a real E.164 number is a country code
   (1-3 digits) plus a subscriber number no real numbering plan issues below
   five digits, so anything shorter is noise a validator should refuse. */
static int dyn_v_e164(const char *s, size_t n)
{
    size_t i = 0, digits = 0;

    if (s[0] == '+')
        i = 1;
    if (i == n)
        return 0;                         /* '+' alone is not a number */
    for (; i < n; i++) {
        if (!dyn_v_digit((unsigned char)s[i]))
            return 0;                     /* only digits survive the '+' */
        digits++;
    }
    return digits >= 8 && digits <= 15;   /* E.164 length window */
}

/* --------------------------------------------------------------- domain */

/* RFC 1035 label grammar over the input, with at least one dot (a domain, not
   a bare host). net.isValid (dyna:net) decides the IP-literal half. */
static int dyn_v_domain_grammar(const char *s, size_t n)
{
    size_t i, lab = 0, dots = 0;

    if (n < 3 || n > 253)
        return 0;                         /* RFC 1035: name <= 253 octets */
    for (i = 0; i <= n; i++) {
        unsigned char c = (i < n) ? (unsigned char)s[i] : (unsigned char)'.';
        if (c == '.') {
            if (lab == 0 || lab > 63)
                return 0;                 /* empty or over-long label */
            if (s[i - lab] == '-' || s[i - 1] == '-')
                return 0;                 /* no hyphen at a label edge */
            if (i < n)
                dots++;
            lab = 0;
        } else if (dyn_v_alpha(c) || dyn_v_digit(c) || c == '-') {
            lab++;
        } else {
            return 0;                     /* not a domain character */
        }
    }
    return dots >= 1;
}

/* ------------------------------------------------------------------ JWT */

/* JWS Compact Serialization (RFC 7515 sec.3.1): three unpadded base64url
   segments. dyna-crypto exposes no decode-without-verify, so the structure is
   checked here with its decoder and the engine's own JSON parser. */
static size_t dyn_v_b64len(size_t n)
{
    size_t r = n % 4;
    return n / 4 * 3 + (r == 2 ? 1 : r == 3 ? 2 : 0);
}

/* `algs` (optional) is an alg allowlist: when provided, the header's alg must
   be one of its strings. Absent/undefined keeps the structural-only behavior
   (any string alg passes). */
static int dyn_v_jwt(JSContext *ctx, const char *s, size_t n, JSValueConst algs)
{
    size_t d1 = (size_t)-1, d2 = (size_t)-1, i;
    uint8_t hb[3073], pb[3073], sb[3073];
    char hs[4100], ps[4100], ss[4100];
    size_t hl, pl, sl, hlen, plen;
    JSValue h, v;
    int in_alg = 1;

    for (i = 0; i < n; i++) {
        if (s[i] == '=') {
            return 0;                 /* base64url segments carry no padding */
        } else if (s[i] == '.') {
            if (d1 == (size_t)-1)
                d1 = i;
            else if (d2 == (size_t)-1)
                d2 = i;
            else
                return 0;                 /* a third dot: not three segments */
        }
    }
    if (d1 == (size_t)-1 || d2 == (size_t)-1)
        return 0;                         /* need two dots */
    if (d1 == 0 || d2 == d1 + 1 || n - d2 - 1 == 0)
        return 0;                         /* every segment is non-empty */
    hlen = d1;
    plen = d2 - d1 - 1;

    hl = dyn_codec_base64url_decode(s, hlen, hb, hs);
    if (hl == DYN_CODEC_BAD || hl > dyn_v_b64len(hlen))
        return 0;                         /* header must be base64url */
    hb[hl] = 0;
    h = JS_ParseJSON(ctx, (const char *)hb, hl, "<jwt>");
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;                         /* header must be JSON */
    }
    if (!JS_IsObject(h) || JS_IsArray(ctx, h)) {
        JS_FreeValue(ctx, h);
        return 0;                         /* header must be a JSON object */
    }
    v = JS_GetPropertyStr(ctx, h, "alg");
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, h);
        return 0;                         /* header must name an alg */
    }
    if (!JS_IsUndefined(algs)) {
        int inlist = 0;
        if (JS_IsArray(ctx, algs)) {
            JSValue lenv = JS_GetPropertyStr(ctx, algs, "length");
            uint32_t an = 0, ai;
            const char *alg = JS_ToCString(ctx, v);
            JS_ToUint32(ctx, &an, lenv);
            JS_FreeValue(ctx, lenv);
            for (ai = 0; ai < an && !inlist; ai++) {
                JSValue e = JS_GetPropertyUint32(ctx, algs, ai);
                const char *ea = JS_ToCString(ctx, e);
                if (alg && ea && strcmp(alg, ea) == 0)
                    inlist = 1;
                if (ea)
                    JS_FreeCString(ctx, ea);
                JS_FreeValue(ctx, e);
            }
            if (alg)
                JS_FreeCString(ctx, alg);
        }
        in_alg = inlist;
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, h);
    if (!in_alg)
        return 0;                         /* alg not in the caller's allowlist */

    pl = dyn_codec_base64url_decode(s + d1 + 1, plen, pb, ps);
    if (pl == DYN_CODEC_BAD || pl > dyn_v_b64len(plen))
        return 0;                         /* claims must be base64url */
    pb[pl] = 0;
    h = JS_ParseJSON(ctx, (const char *)pb, pl, "<jwt>");
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;                         /* claims must be JSON */
    }
    if (!JS_IsObject(h) || JS_IsArray(ctx, h)) {
        JS_FreeValue(ctx, h);
        return 0;                         /* claims must be a JSON object */
    }
    JS_FreeValue(ctx, h);

    sl = dyn_codec_base64url_decode(s + d2 + 1, n - d2 - 1, sb, ss);
    if (sl == DYN_CODEC_BAD || sl > dyn_v_b64len(n - d2 - 1))
        return 0;                         /* signature must be base64url */
    return 1;
}


/* ----------------------------------------------------------- extensions */

enum { VX_URL, VX_DOMAIN, VX_SLUG, VX_UUID, VX_JWT, VX_SEMVER, VX_E164,
       VX_JSON, VX_STRONGPW };

static JSValue dyn_v_check_ext(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    static const char *const NAMES[] = {
        "IsURL", "IsDomain", "IsSlug", "IsUUID", "IsJWT", "IsSemver", "IsE164",
        "IsJSON", "IsStrongPassword"
    };
    const char *s;
    size_t n;
    int ok = 0;

    (void)this_val;
    if (dyn_v_arg(ctx, argc, argv, NAMES[magic], &s, &n) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case VX_URL: {
        JSValue fn, arg, u;
        JSValueConst a1[1];
        fn = dyn_v_export(ctx, "dyna:url", "URL");
        if (JS_IsException(fn)) { JS_FreeCString(ctx, s); return fn; }
        arg = JS_NewStringLen(ctx, s, n);
        if (JS_IsException(arg)) {
            JS_FreeValue(ctx, fn);
            JS_FreeCString(ctx, s);
            return arg;
        }
        a1[0] = arg;
        u = JS_CallConstructor(ctx, fn, 1, a1);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(u)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            break;                        /* the ctor refused it: not a URL */
        }
        ok = 1;
        /* A special scheme with an empty host is the parser's lenient corner
           ("https://"); the validator checks the value the ctor returned. */
        {
            JSValue proto = JS_GetPropertyStr(ctx, u, "protocol");
            JSValue host = JS_GetPropertyStr(ctx, u, "hostname");
            const char *p = JS_IsString(proto) ? JS_ToCString(ctx, proto) : NULL;
            const char *h = JS_IsString(host) ? JS_ToCString(ctx, host) : NULL;
            if (!p || !h) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                ok = 0;
            } else if (h[0] == '\0' &&
                       (!strcmp(p, "http:") || !strcmp(p, "https:") ||
                        !strcmp(p, "ws:") || !strcmp(p, "wss:") ||
                        !strcmp(p, "ftp:"))) {
                ok = 0;
            }
            if (p) JS_FreeCString(ctx, p);
            if (h) JS_FreeCString(ctx, h);
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, host);
        }
        JS_FreeValue(ctx, u);
        break;
    }
    case VX_DOMAIN:
        if (dyn_v_domain_grammar(s, n)) {
            int r = dyn_v_js_bool(ctx, "dyna:net", "isValid",
                                  s, n);
            if (r < 0) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
            ok = !r;                      /* an IP literal is not a domain */
        }
        break;
    case VX_SLUG:
        ok = dyn_v_slug(s, n);
        break;
    case VX_UUID:
        /* RFC 4122 canonical 8-4-4-4-12 only: 36 chars, then the module's
           parser decides the hex and dash positions. */
        if (n == 36) {
            int r = dyn_v_js_bool(ctx, "dyna:uuid", "validate",
                                  s, n);
            if (r < 0) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
            ok = r;
        }
        break;
    case VX_JWT: {
        /* IsJWT(token[, {algs: ["HS256", ...]}]): the optional options object
           carries an alg allowlist; without it the check stays structural. */
        JSValue algs = JS_UNDEFINED;
        if (argc > 1 && JS_IsObject(argv[1])) {
            algs = JS_GetPropertyStr(ctx, argv[1], "algs");
            if (JS_IsException(algs)) {
                /* an options getter threw: propagate, never leave the stale
                   exception pending (it would fire at an unrelated operation) */
                JS_FreeCString(ctx, s);
                return JS_EXCEPTION;
            }
        }
        ok = dyn_v_jwt(ctx, s, n, algs);
        JS_FreeValue(ctx, algs);
        break;
    }
    case VX_SEMVER: {
        int r = dyn_v_js_bool(ctx, "dyna:semver", "isValid",
                              s, n);
        if (r < 0) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
        ok = r;
        break;
    }
    case VX_E164:
        ok = dyn_v_e164(s, n);
        break;
    case VX_JSON: {
        /* The engine's own JSON parser decides, exactly as it does for a JWT
           header: any JSON value counts (object, array, string, number,
           bool, null). The DYN_V_MAX input cap dyn_v_arg enforces -- thrown
           as a RangeError, not silently truncated -- is the defensive size
           limit; there is no separate one. */
        JSValue v = JS_ParseJSON(ctx, s, n, "<IsJSON>");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            ok = 0;                       /* a parse error is the answer, not a throw */
        } else {
            JS_FreeValue(ctx, v);
            ok = 1;
        }
        break;
    }
    case VX_STRONGPW: {
        /* IsStrongPassword(text, { minLength, minLower, minUpper, minDigits,
           minSymbols }): defaults 8/1/1/1/1. A "symbol" is a printable
           non-alphanumeric ASCII character (a space is allowed but counts
           only toward the length, never toward a class). The input must be
           printable ASCII outright -- a byte >= 0x80 would make the classes
           meaningless (is a Cyrillic letter lower? upper? neither?) -- so a
           password containing one is refused, not guessed at. An empty
           string is never one.
           Option values must be non-negative integers <= 4096: a non-number
           throws TypeError, a negative/fractional/over-large one RangeError
           (no input can satisfy more); an absent option keeps its default,
           and a second argument that is not an object is ignored, matching
           IsJWT. */
        static const char *const OPTS[] = { "minLength", "minLower", "minUpper",
                                            "minDigits", "minSymbols" };
        uint32_t mins[5] = { 8, 1, 1, 1, 1 };
        uint32_t counts[5] = { 0, 0, 0, 0, 0 };
        uint32_t oi;
        size_t si;

        ok = 0;
        if (argc > 1 && JS_IsObject(argv[1])) {
            for (oi = 0; oi < 5; oi++) {
                JSValue v = JS_GetPropertyStr(ctx, argv[1], OPTS[oi]);
                double dv;
                if (JS_IsException(v)) {
                    JS_FreeCString(ctx, s);
                    return JS_EXCEPTION;  /* an options getter threw */
                }
                if (JS_IsUndefined(v)) {
                    JS_FreeValue(ctx, v);
                    continue;
                }
                if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &dv, v) < 0) {
                    JS_FreeValue(ctx, v);
                    JS_FreeCString(ctx, s);
                    return JS_ThrowTypeError(ctx,
                        "%s(opts): %s must be a number", NAMES[magic], OPTS[oi]);
                }
                /* the ordered form keeps NaN and +/-Infinity away from the
                   (uint32_t) cast, whose conversion would be undefined */
                if (!(dv >= 0 && dv <= 4096 && dv == (uint32_t)dv)) {
                    JS_FreeValue(ctx, v);
                    JS_FreeCString(ctx, s);
                    return JS_ThrowRangeError(ctx,
                        "%s(opts): %s must be a non-negative integer <= 4096",
                        NAMES[magic], OPTS[oi]);
                }
                mins[oi] = (uint32_t)dv;
                JS_FreeValue(ctx, v);
            }
        }
        if (n == 0)
            break;
        ok = 1;
        for (si = 0; si < n; si++) {
            unsigned char c = (unsigned char)s[si];
            if (c < 0x20 || c > 0x7e) {   /* control or non-ASCII */
                ok = 0;
                break;
            }
            if (c >= 'a' && c <= 'z')      counts[1]++;
            else if (c >= 'A' && c <= 'Z') counts[2]++;
            else if (dyn_v_digit(c))       counts[3]++;
            else if (c != ' ')             counts[4]++;  /* a space is length only */
        }
        if (ok)
            ok = (uint32_t)n >= mins[0] && counts[1] >= mins[1] &&
                 counts[2] >= mins[2] && counts[3] >= mins[3] &&
                 counts[4] >= mins[4];
        break;
    }
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry dyn_v_funcs_ext[] = {
    JS_CFUNC_MAGIC_DEF("IsURL", 1, dyn_v_check_ext, VX_URL),
    JS_CFUNC_MAGIC_DEF("IsDomain", 1, dyn_v_check_ext, VX_DOMAIN),
    JS_CFUNC_MAGIC_DEF("IsSlug", 1, dyn_v_check_ext, VX_SLUG),
    JS_CFUNC_MAGIC_DEF("IsUUID", 1, dyn_v_check_ext, VX_UUID),
    JS_CFUNC_MAGIC_DEF("IsJWT", 1, dyn_v_check_ext, VX_JWT),
    JS_CFUNC_MAGIC_DEF("IsSemver", 1, dyn_v_check_ext, VX_SEMVER),
    JS_CFUNC_MAGIC_DEF("IsE164", 1, dyn_v_check_ext, VX_E164),
    JS_CFUNC_MAGIC_DEF("IsJSON", 1, dyn_v_check_ext, VX_JSON),
    JS_CFUNC_MAGIC_DEF("IsStrongPassword", 1, dyn_v_check_ext, VX_STRONGPW),
};

static const JSCFunctionListEntry dyn_v_funcs[] = {
    JS_CFUNC_MAGIC_DEF("IsAlpha", 1, dyn_v_check, V_ALPHA),
    JS_CFUNC_MAGIC_DEF("IsAlphanumeric", 1, dyn_v_check, V_ALNUM),
    JS_CFUNC_MAGIC_DEF("IsAscii", 1, dyn_v_check, V_ASCII),
    JS_CFUNC_MAGIC_DEF("IsEmail", 1, dyn_v_check, V_EMAIL),
    JS_CFUNC_MAGIC_DEF("IsCreditCard", 1, dyn_v_check, V_LUHN),
    JS_CFUNC_MAGIC_DEF("IsIBAN", 1, dyn_v_check, V_IBAN),
    JS_CFUNC_MAGIC_DEF("IsIPv4", 1, dyn_v_check, V_IPV4),
    JS_CFUNC_MAGIC_DEF("IsIPv6", 1, dyn_v_check, V_IPV6),
    JS_CFUNC_MAGIC_DEF("IsIP", 1, dyn_v_check, V_IP),
    JS_CFUNC_MAGIC_DEF("IsPort", 1, dyn_v_check, V_PORT),
    JS_CFUNC_MAGIC_DEF("IsBase64", 1, dyn_v_check, V_BASE64),
    JS_CFUNC_MAGIC_DEF("IsBase64Url", 1, dyn_v_check, V_BASE64URL),
    JS_CFUNC_MAGIC_DEF("IsHex", 1, dyn_v_check, V_HEX),
    JS_CFUNC_MAGIC_DEF("IsHexColor", 1, dyn_v_check, V_HEXCOLOR),
    JS_CFUNC_MAGIC_DEF("IsNumeric", 1, dyn_v_check, V_NUMERIC),
    JS_CFUNC_MAGIC_DEF("IsDateString", 1, dyn_v_check, V_DATESTRING),
    JS_CFUNC_MAGIC_DEF("IsRFC3339", 1, dyn_v_check, V_RFC3339),
    JS_CFUNC_MAGIC_DEF("IsMimeType", 1, dyn_v_check, V_MIME),
};

static int dyn_v_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs)) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_v_funcs_ext,
                                  countof(dyn_v_funcs_ext));
}

int js_nat_init_validate(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:validate", dyn_v_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs)) < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_v_funcs_ext,
                                  countof(dyn_v_funcs_ext));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_VALIDATE */
