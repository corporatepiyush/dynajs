/*
 * test_validk_core.c -- src/core/validk (the dyna:validate byte-level
 * predicates) as a standalone C library.
 *
 * Oracles are independent of the code: IPv4/IPv6 against libc inet_pton
 * plus the documented strictness (no leading-zero octets, at most four hex
 * digits per group, no zone id); dates against month lengths taken as
 * differences of a proleptic Gregorian day count (years 0000..9999); RFC 3339 against an ABNF reader written here (T or t,
 * Z or z, :60 allowed, offsets up to 23:59); base64/base64url by decoding
 * and re-encoding with an RFC 4648 codec written here (canonical means the
 * re-encode is byte-identical); Luhn and IBAN mod-97 by long division over
 * the digit expansion, with IBAN lengths from the SWIFT registry for the
 * countries listed here; the rest (port, hex, hex colour, numeric, slug,
 * E.164, domain labels) from the grammar in dynajs.d.ts applied to 300k
 * random strings each. Email and MIME have only a practical grammar, so
 * they are checked on fixed accept/refuse tables.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "dyn-validk.h"

static int fails, checks;

#define CHECK(c, ...)                                       \
    do {                                                    \
        checks++;                                           \
        if (!(c)) {                                         \
            fails++;                                        \
            if (fails < 30) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

static unsigned long long rs = 0x7a3c19e5d2b84f61ull;
static unsigned rnd(unsigned n)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs % n);
}

static void rstr(char* s, size_t* n, const char* al, unsigned maxlen)
{
    size_t k = rnd(maxlen + 1), i, m = strlen(al);
    for (i = 0; i < k; i++)
        s[i] = al[rnd((unsigned)m)];
    s[k] = 0;
    *n = k;
}

static int isdig(int c) { return c >= '0' && c <= '9'; }
static int ishex(int c) { return isdig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static int no_lead0_v4(const char* s)
{
    const char* f = strrchr(s, ':');
    f = f ? f + 1 : s;
    for (; *f; f++)
        if (*f == '0' && (f == s || f[-1] == '.' || f[-1] == ':') && isdig(f[1]))
            return 0;
    return 1;
}

static int o_ipv4(const char* s)
{
    unsigned char b[4];
    return strchr(s, ':') == NULL && inet_pton(AF_INET, s, b) == 1 && no_lead0_v4(s);
}

static int o_ipv6(const char* s)
{
    unsigned char b[16];
    int run = 0;
    const char* f;
    if (strchr(s, '%') || !strchr(s, ':'))
        return 0;
    for (f = s; *f; f++) {
        run = (*f == ':' || *f == '.') ? 0 : run + 1;
        if (run > 4)
            return 0;
    }
    return inet_pton(AF_INET6, s, b) == 1 && (!strchr(s, '.') || no_lead0_v4(s));
}

static long days_from_civil(long y, long m, long d)
{
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int o_date(const char* s, size_t n)
{
    size_t i;
    long y, m, d;
    if (n != 10 || s[4] != '-' || s[7] != '-')
        return 0;
    for (i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdig(s[i]))
            return 0;
    y = atol(s);
    m = atol(s + 5);
    d = atol(s + 8);
    if (m < 1 || m > 12 || d < 1)
        return 0;
    return d <= (m == 12 ? days_from_civil(y + 1, 1, 1) : days_from_civil(y, m + 1, 1)) - days_from_civil(y, m, 1);
}

static int two(const char* s, int max) { return isdig(s[0]) && isdig(s[1]) && (s[0] - '0') * 10 + (s[1] - '0') <= max; }

static int o_rfc3339(const char* s, size_t n)
{
    size_t i = 19;
    if (n < 20 || !o_date(s, 10) || (s[10] != 'T' && s[10] != 't') || s[13] != ':' || s[16] != ':')
        return 0;
    if (!two(s + 11, 23) || !two(s + 14, 59) || !two(s + 17, 60))
        return 0;
    if (s[i] == '.') {
        size_t a = ++i;
        while (i < n && isdig(s[i]))
            i++;
        if (i == a)
            return 0;
    }
    if (i + 1 == n && (s[i] == 'Z' || s[i] == 'z'))
        return 1;
    return i + 6 == n && (s[i] == '+' || s[i] == '-') && two(s + i + 1, 23) && s[i + 3] == ':' && two(s + i + 4, 59);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64U[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t enc(const unsigned char* b, size_t n, const char* al, int pad, char* out)
{
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = (unsigned)b[i] << 16 | (unsigned)b[i + 1] << 8 | b[i + 2];
        out[o++] = al[v >> 18];
        out[o++] = al[v >> 12 & 63];
        out[o++] = al[v >> 6 & 63];
        out[o++] = al[v & 63];
    }
    if (n - i == 1) {
        out[o++] = al[b[i] >> 2];
        out[o++] = al[(b[i] & 3) << 4];
        if (pad) {
            out[o++] = '=';
            out[o++] = '=';
        }
    } else if (n - i == 2) {
        unsigned v = (unsigned)b[i] << 8 | b[i + 1];
        out[o++] = al[v >> 10];
        out[o++] = al[v >> 4 & 63];
        out[o++] = al[(v & 15) << 2];
        if (pad)
            out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static int o_b64(const char* s, size_t n, const char* al, int pad)
{
    unsigned char b[512];
    char re[700];
    size_t i, bits = 0, k = 0, m = n;
    unsigned acc = 0;
    if (n == 0)
        return 0;
    while (pad && m > 0 && s[m - 1] == '=')
        m--;
    for (i = 0; i < m; i++) {
        const char* p = s[i] ? strchr(al, s[i]) : NULL;
        if (!p)
            return 0;
        acc = acc << 6 | (unsigned)(p - al);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            b[k++] = (unsigned char)(acc >> bits);
        }
    }
    enc(b, k, al, pad, re);
    return strlen(re) == n && memcmp(re, s, n) == 0;
}

static int mod97(const char* digits)
{
    int r = 0;
    for (; *digits; digits++)
        r = (r * 10 + (*digits - '0')) % 97;
    return r;
}

static void iban_expand(const char* s, char* out)
{
    size_t n = strlen(s), i;
    char* o = out;
    for (i = 0; i < n; i++) {
        char c = s[(i + 4) % n];
        if (isdig(c))
            *o++ = c;
        else
            o += sprintf(o, "%d", c - 'A' + 10);
    }
    *o = 0;
}

static const struct {
    const char* cc;
    int len;
} REG[] = { { "GB", 22 }, { "DE", 22 }, { "FR", 27 }, { "NL", 18 }, { "BE", 16 }, { "CH", 21 }, { "ES", 24 }, { "IT", 27 },
    { "AT", 20 }, { "NO", 15 }, { "MT", 31 }, { "SE", 24 }, { "PL", 28 }, { "IE", 22 }, { "DK", 18 }, { "FI", 18 }, { "PT", 25 }, { "LU", 20 } };

static void t_iban(void)
{
    static const char* KNOWN[] = { "GB82WEST12345698765432", "DE89370400440532013000", "FR1420041010050500013M02606", "NL91ABNA0417164300",
        "BE68539007547034", "CH9300762011623852957", "GB82 WEST 1234 5698 7654 32", "gb82west12345698765432", "GB82-WEST-1234-5698-7654-32" };
    static const char AN[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t i;
    int k;
    for (i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++)
        CHECK(dyn_v_iban(KNOWN[i], strlen(KNOWN[i])) == 1, "iban known %s", KNOWN[i]);
    CHECK(dyn_v_iban("GB82WEST1234569876543", 21) == 0, "iban short");
    CHECK(dyn_v_iban("ZZ82WEST12345698765432", 22) == 0, "iban unknown country");
    for (k = 0; k < 40000; k++) {
        char s[40], x[100], chk[3];
        int c = (int)rnd(sizeof(REG) / sizeof(REG[0])), j, r, want;
        int len = REG[c].len;
        memcpy(s, REG[c].cc, 2);
        s[2] = s[3] = '0';
        for (j = 4; j < len; j++)
            s[j] = AN[rnd(rnd(3) ? 10 : 36)];
        s[len] = 0;
        iban_expand(s, x);
        r = 98 - mod97(x);
        snprintf(chk, sizeof(chk), "%02d", r);
        s[2] = chk[0];
        s[3] = chk[1];
        CHECK(dyn_v_iban(s, (size_t)len) == 1, "iban gen %s", s);
        j = 2 + (int)rnd((unsigned)len - 2);
        s[j] = AN[rnd(36)];
        if (j < 4 && !isdig(s[j]))
            want = 0;
        else {
            iban_expand(s, x);
            want = mod97(x) == 1;
        }
        CHECK(dyn_v_iban(s, (size_t)len) == want, "iban mutated %s want %d", s, want);
        CHECK(dyn_v_iban(s, (size_t)len - 1) == 0, "iban truncated %s", s);
    }
}

static int o_luhn(const char* s, size_t n)
{
    char d[64];
    size_t k = 0, i;
    int sum = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == ' ' || s[i] == '-')
            continue;
        if (!isdig(s[i]) || k >= sizeof(d))
            return 0;
        d[k++] = s[i];
    }
    if (k < 12 || k > 19)
        return 0;
    for (i = 0; i < k; i++) {
        int v = d[k - 1 - i] - '0';
        if (i % 2)
            v = v * 2 > 9 ? v * 2 - 9 : v * 2;
        sum += v;
    }
    return sum % 10 == 0;
}

static int o_port(const char* s, size_t n)
{
    size_t i;
    if (n == 0 || n > 5 || (n > 1 && s[0] == '0'))
        return 0;
    for (i = 0; i < n; i++)
        if (!isdig(s[i]))
            return 0;
    return atol(s) <= 65535;
}

static int o_hex(const char* s, size_t n)
{
    size_t i;
    if (n == 0 || n % 2)
        return 0;
    for (i = 0; i < n; i++)
        if (!ishex(s[i]))
            return 0;
    return 1;
}

static int o_hexcolor(const char* s, size_t n)
{
    size_t i;
    if (n != 4 && n != 7 && n != 9)
        return 0;
    if (s[0] != '#')
        return 0;
    for (i = 1; i < n; i++)
        if (!ishex(s[i]))
            return 0;
    return 1;
}

static int o_numeric(const char* s, size_t n)
{
    size_t i = 0;
    int dig = 0, dots = 0;
    if (n && (s[0] == '+' || s[0] == '-'))
        i++;
    for (; i < n; i++) {
        if (s[i] == '.')
            dots++;
        else if (isdig(s[i]))
            dig++;
        else
            return 0;
    }
    return dig > 0 && dots <= 1;
}

static int o_slug(const char* s, size_t n)
{
    size_t i;
    if (n == 0 || n > 64 || s[0] == '-' || s[n - 1] == '-' || strstr(s, "--"))
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || isdig(s[i]) || s[i] == '-'))
            return 0;
    return 1;
}

static int o_e164(const char* s, size_t n)
{
    size_t i = s[0] == '+', d = 0;
    for (; i < n; i++, d++)
        if (!isdig(s[i]))
            return 0;
    return d >= 8 && d <= 15;
}

static int o_domain(const char* s, size_t n)
{
    char buf[400], *lab, *save = NULL;
    int labels = 0;
    if (n < 3 || n > 253 || s[0] == '.' || s[n - 1] == '.' || strstr(s, ".."))
        return 0;
    memcpy(buf, s, n + 1);
    for (lab = strtok_r(buf, ".", &save); lab; lab = strtok_r(NULL, ".", &save)) {
        size_t l = strlen(lab), i;
        if (l == 0 || l > 63 || lab[0] == '-' || lab[l - 1] == '-')
            return 0;
        for (i = 0; i < l; i++)
            if (!((lab[i] | 32) >= 'a' && (lab[i] | 32) <= 'z') && !isdig(lab[i]) && lab[i] != '-')
                return 0;
        labels++;
    }
    return labels >= 2;
}

typedef int (*kfn)(const char*, size_t);

static void fuzz(const char* name, kfn k, kfn o, const char* al, unsigned maxlen, int iters)
{
    int i;
    for (i = 0; i < iters; i++) {
        char s[400];
        size_t n;
        rstr(s, &n, al, maxlen);
        CHECK(k(s, n) == o(s, n), "%s '%s' got %d want %d", name, s, k(s, n), o(s, n));
    }
}

static int o_ipv4n(const char* s, size_t n) { (void)n; return o_ipv4(s); }
static int o_ipv6n(const char* s, size_t n) { (void)n; return o_ipv6(s); }
static int o_b64n(const char* s, size_t n) { return o_b64(s, n, B64, 1); }
static int o_b64un(const char* s, size_t n) { return o_b64(s, n, B64U, 0); }

int main(void)
{
    int k;
    fuzz("ipv4", dyn_v_ipv4, o_ipv4n, "0123456789...", 16, 300000);
    fuzz("ipv6", dyn_v_ipv6, o_ipv6n, "0123456789abcdefABCDEF:::...%", 46, 300000);
    for (k = 0; k < 100000; k++) {
        char s[64];
        unsigned char b[16];
        int j;
        for (j = 0; j < 16; j++)
            b[j] = rnd(3) ? 0 : (unsigned char)rnd(256);
        if (rnd(4) == 0) {
            memset(b, 0, 10);
            b[10] = b[11] = 0xff;
        }
        inet_ntop(AF_INET6, b, s, sizeof(s));
        CHECK(dyn_v_ipv6(s, strlen(s)) == 1, "ipv6 ntop %s", s);
        inet_ntop(AF_INET, b + 12, s, sizeof(s));
        CHECK(dyn_v_ipv4(s, strlen(s)) == 1, "ipv4 ntop %s", s);
    }
    for (k = 0; k < 200000; k++) {
        char s[64];
        int y = (int)rnd(10000), m = (int)rnd(14), d = (int)rnd(33);
        snprintf(s, sizeof(s), "%04d-%02d-%02d", y, m, d);
        CHECK(dyn_v_datestring(s, 10) == o_date(s, 10), "date %s", s);
        snprintf(s, sizeof(s), "%04d-%02d-%02d%c%02u:%02u:%02u%s%s", y, m % 13, d % 32, "Tt T"[rnd(4)], rnd(25), rnd(61), rnd(62),
            rnd(3) ? "" : (rnd(4) ? ".123" : "."), (const char* const[]) { "Z", "z", "+05:30", "-23:59", "+24:00", "+05:60", "", "+0530", "Zx" }[rnd(9)]);
        CHECK(dyn_v_rfc3339(s, strlen(s)) == o_rfc3339(s, strlen(s)), "rfc3339 %s got %d", s, dyn_v_rfc3339(s, strlen(s)));
    }
    fuzz("date", dyn_v_datestring, o_date, "0123456789--", 11, 100000);
    for (k = 0; k < 60000; k++) {
        unsigned char b[64];
        char s[200];
        size_t n = rnd(40), j, m;
        for (j = 0; j < n; j++)
            b[j] = (unsigned char)rnd(256);
        m = enc(b, n, B64, 1, s);
        CHECK(n == 0 || dyn_v_base64(s, m) == 1, "b64 canonical %s", s);
        if (m) {
            s[rnd((unsigned)m)] = "AB+/=_-a"[rnd(8)];
            CHECK(dyn_v_base64(s, m) == o_b64(s, m, B64, 1), "b64 mutated %s", s);
        }
        m = enc(b, n, B64U, 0, s);
        CHECK(n == 0 || dyn_v_base64url(s, m) == 1, "b64u canonical %s", s);
        if (m) {
            s[rnd((unsigned)m)] = "AB+/=_-a"[rnd(8)];
            CHECK(dyn_v_base64url(s, m) == o_b64(s, m, B64U, 0), "b64u mutated %s", s);
        }
    }
    fuzz("b64", dyn_v_base64, o_b64n, "AQgw+/==", 12, 200000);
    fuzz("b64u", dyn_v_base64url, o_b64un, "AQgw-_=", 12, 200000);
    t_iban();
    for (k = 0; k < 100000; k++) {
        char s[40];
        int n = 10 + (int)rnd(12), j, sum = 0;
        for (j = 0; j < n; j++)
            s[j] = (char)('0' + rnd(10));
        s[n] = 0;
        for (j = 0; j < n - 1; j++) {
            int v = s[n - 2 - j] - '0';
            if (j % 2 == 0)
                v = v * 2 > 9 ? v * 2 - 9 : v * 2;
            sum += v;
        }
        s[n - 1] = (char)('0' + (10 - sum % 10) % 10);
        CHECK(dyn_v_luhn(s, (size_t)n) == (n >= 12 && n <= 19), "luhn generated %s", s);
    }
    fuzz("luhn", dyn_v_luhn, o_luhn, "0123456789 -x", 22, 300000);
    fuzz("port", dyn_v_port, o_port, "0123456789+", 6, 300000);
    fuzz("hex", dyn_v_hex, o_hex, "0123456789abcdefABCDEFgx", 8, 300000);
    fuzz("hexcolor", dyn_v_hexcolor, o_hexcolor, "#0123456789abcdefABCDEFg", 10, 300000);
    fuzz("numeric", dyn_v_numeric, o_numeric, "0123456789+-.e", 6, 300000);
    fuzz("slug", dyn_v_slug, o_slug, "abz09--A_", 10, 300000);
    fuzz("e164", dyn_v_e164, o_e164, "0123456789+ ", 17, 300000);
    fuzz("domain", dyn_v_domain_grammar, o_domain, "ab9-..X_", 12, 300000);
    {
        char s[300];
        memset(s, 'a', 70);
        s[70] = 0;
        CHECK(dyn_v_port("65535", 5) == 1 && dyn_v_port("65536", 5) == 0 && dyn_v_port("0", 1) == 1 && dyn_v_port("99999", 5) == 0, "port bounds");
        CHECK(dyn_v_slug(s, 64) == 1 && dyn_v_slug(s, 65) == 0, "slug 64 cap");
        memcpy(s + 63, ".com", 5);
        CHECK(dyn_v_domain_grammar(s, strlen(s)) == 1, "63-octet label");
        memset(s, 'a', 70);
        memcpy(s + 64, ".com", 5);
        CHECK(dyn_v_domain_grammar(s, strlen(s)) == 0, "64-octet label");
    }
    {
        static const char* YES[] = { "a@b.co", "first.last@example.com", "x+tag@sub.example.org", "a_b-c@d-e.fg", "UPPER@EXAMPLE.COM", "1@2.io" };
        static const char* NO[] = { "", "a", "@b.co", "a@", "a@b", "a@@b.co", "a b@c.co", "\"q\"@b.co", "a(c)@b.co", "a..b@c.co", ".a@b.co", "a.@b.co", "a@b..co", "a@-b.co", "a@b.co." };
        size_t i;
        for (i = 0; i < sizeof(YES) / sizeof(YES[0]); i++)
            CHECK(dyn_v_email(YES[i], strlen(YES[i])) == 1, "email yes %s", YES[i]);
        for (i = 0; i < sizeof(NO) / sizeof(NO[0]); i++)
            CHECK(dyn_v_email(NO[i], strlen(NO[i])) == 0, "email no %s", NO[i]);
    }
    {
        static const char* YES[] = { "text/plain", "application/json", "text/html; charset=utf-8", "image/svg+xml", "application/vnd.api+json", "multipart/form-data; boundary=x" };
        static const char* NO[] = { "", "text", "text/", "/plain", "text/plain;", "text plain", "te xt/plain", "text/plain; charset", "text/plain/x" };
        size_t i;
        for (i = 0; i < sizeof(YES) / sizeof(YES[0]); i++)
            CHECK(dyn_v_mime(YES[i], strlen(YES[i])) == 1, "mime yes %s", YES[i]);
        for (i = 0; i < sizeof(NO) / sizeof(NO[0]); i++)
            CHECK(dyn_v_mime(NO[i], strlen(NO[i])) == 0, "mime no %s", NO[i]);
    }
    printf("%s test_validk_core: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
