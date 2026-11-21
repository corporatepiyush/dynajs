/*
 * test_idna_core.c -- src/core/idna + src/core/urlrec as standalone C libraries.
 *
 * Links only the two libraries, libunicode.c and cutils.c (no engine). Checks:
 *  1. dyn_idna_utf8_next is byte-for-byte the decoder the url module used before
 *     the extraction (cutils' unicode_from_utf8): every lead byte, every 2- and
 *     3-byte sequence, and every 4..6-byte sequence over a class-covering set of
 *     continuation bytes, at every max_len that matters.
 *  2. Punycode vectors from RFC 3492 section 7.1 (B, C, L, P).
 *  3. UTS #46 section 4 / Table 1 conversions (Bücher.DE, faß.de both modes).
 *  4. WHATWG URL Standard parsing outcomes (host lowercasing, dot segments,
 *     IPv6 compression, IPv4 number forms, default ports, IDNA host).
 *  5. application/x-www-form-urlencoded round trip (WHATWG URL section 5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "dyn-idna.h"
#include "dyn-urlrec.h"

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static long utf8_cases;

static void utf8_one(const uint8_t* buf, int len)
{
    int ml;
    for (ml = 1; ml <= len; ml++) {
        const uint8_t *pa = NULL, *pb = NULL;
        int a = unicode_from_utf8(buf, ml, &pa);
        int b = dyn_idna_utf8_next(buf, ml, &pb);
        utf8_cases++;
        if (a != b || (a >= 0 && pa != pb)) {
            fails++;
            printf("FAIL utf8 len=%d ml=%d bytes=%02x %02x %02x %02x %02x %02x ref=%d got=%d\n",
                len, ml, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], a, b);
            return;
        }
    }
}

static void test_utf8(void)
{
    static const uint8_t cls[] = { 0x00, 0x41, 0x7f, 0x80, 0x81, 0x9f, 0xa0, 0xbf, 0xc0, 0xc2, 0xdf, 0xe0, 0xf0, 0xff };
    uint8_t b[6];
    unsigned i, j, k, l, m;
    memset(b, 0, sizeof b);
    for (i = 0; i < 256; i++) {
        b[0] = (uint8_t)i;
        utf8_one(b, 6);
        for (j = 0; j < 256; j++) {
            b[1] = (uint8_t)j;
            utf8_one(b, 2);
            if (i >= 0xe0)
                for (k = 0; k < 256; k++) {
                    b[2] = (uint8_t)k;
                    utf8_one(b, 3);
                }
        }
    }
    for (i = 0xf0; i < 256; i++)
        for (j = 0; j < sizeof cls; j++)
            for (k = 0; k < sizeof cls; k++)
                for (l = 0; l < sizeof cls; l++)
                    for (m = 0; m < sizeof cls; m++) {
                        b[0] = (uint8_t)i;
                        b[1] = cls[j];
                        b[2] = cls[k];
                        b[3] = cls[l];
                        b[4] = cls[m];
                        b[5] = 0x80;
                        utf8_one(b, 6);
                        b[5] = 0xbf;
                        utf8_one(b, 6);
                        b[5] = 0x41;
                        utf8_one(b, 6);
                    }
    CHECK(utf8_cases > 4000000, "utf8 differential ran %ld cases", utf8_cases);
}

static int cps_eq_ascii(const dyn_cp_t* c, const char* want)
{
    size_t i, n = strlen(want);
    if (c->n != n)
        return 0;
    for (i = 0; i < n; i++)
        if (c->p[i] != (uint8_t)want[i])
            return 0;
    return 1;
}

static void puny_vec(const char* tag, const uint32_t* cps, int n, const char* want)
{
    dyn_cp_t out, back;
    size_t i;
    int ok;
    dyn_cp_init(&out);
    CHECK(pc_encode(cps, n, &out) == 0, "RFC 3492 7.1 (%s) encode failed", tag);
    CHECK(cps_eq_ascii(&out, want), "RFC 3492 7.1 (%s) encode != %s", tag, want);
    dyn_cp_init(&back);
    CHECK(pc_decode(want, strlen(want), &back) == 0, "RFC 3492 7.1 (%s) decode failed", tag);
    ok = back.n == (size_t)n;
    for (i = 0; ok && i < back.n; i++)
        ok = back.p[i] == cps[i];
    CHECK(ok, "RFC 3492 7.1 (%s) decode mismatch", tag);
    dyn_cp_free(&out);
    dyn_cp_free(&back);
}

static void test_punycode(void)
{
    static const uint32_t b[] = { 0x4ED6, 0x4EEC, 0x4E3A, 0x4EC0, 0x4E48, 0x4E0D, 0x8BF4, 0x4E2D, 0x6587 };
    static const uint32_t c[] = { 0x4ED6, 0x5011, 0x7232, 0x4EC0, 0x9EBD, 0x4E0D, 0x8AAA, 0x4E2D, 0x6587 };
    static const uint32_t l[] = { 0x0033, 0x5E74, 0x0042, 0x7D44, 0x91D1, 0x516B, 0x5148, 0x751F };
    static const uint32_t p[] = { 0x305D, 0x306E, 0x30B9, 0x30D4, 0x30FC, 0x30C9, 0x3067 };
    puny_vec("B", b, 9, "ihqwcrb4cv8a8dqg056pqjye");
    puny_vec("C", c, 9, "ihqwctvzc91f659drss3x8bo0yb");
    puny_vec("L", l, 8, "3B-ww4c5e180e575a65lsy2b");
    puny_vec("P", p, 7, "d9juau41awczczp");
}

static void to_ascii(const char* in, int transitional, const char* want)
{
    dyn_cp_t src, out;
    dyn_idna_proc_t p;
    const uint8_t *s = (const uint8_t*)in, *e = s + strlen(in), *np;
    dyn_cp_init(&src);
    dyn_cp_init(&out);
    while (s < e) {
        int c = dyn_idna_utf8_next(s, (int)(e - s), &np);
        if (c < 0)
            break;
        dyn_cp_put(&src, (uint32_t)c);
        s = np;
    }
    dyn_idna_to_ascii(&p, src.p, (int)src.n, transitional, 0, &out);
    CHECK(!p.oom && !p.nerrors && cps_eq_ascii(&out, want),
        "UTS #46 ToASCII(%s, transitional=%d) != %s (errors %d)", in, transitional, want, p.nerrors);
    dyn_idna_labs_free(&p.labs);
    dyn_cp_free(&p.cps);
    dyn_cp_free(&src);
    dyn_cp_free(&out);
}

static void test_uts46(void)
{
    to_ascii("B\xc3\xbc"
             "cher.DE",
        0, "xn--bcher-kva.de");
    to_ascii("fa\xc3\x9f.de", 1, "fass.de");
    to_ascii("fa\xc3\x9f.de", 0, "xn--fa-hia.de");
    CHECK(dyn_idna_ascii_fast_check("www.example.com", 15) == 1, "fast check accepts plain LDH");
    CHECK(dyn_idna_ascii_fast_check("xn--a.example", 13) == 0, "fast check defers xn-- labels");
    CHECK(dyn_idna_hostname_ok("xn--bcher-kva.de", 16), "hostname_ok accepts a valid A-label");
}

static void url_vec(const char* in, const char* want)
{
    dyn_url_t* u = (dyn_url_t*)calloc(1, sizeof *u);
    dyn_sb_t b;
    int rc = dyn_url_parse(u, in, strlen(in), NULL);
    dyn_sb_init(&b);
    if (rc == 0)
        dyn_url_serialize(&b, u);
    CHECK(rc == 0 && b.n == strlen(want) && !memcmp(b.p, want, b.n),
        "WHATWG URL parse(%s) -> %.*s, want %s", in, (int)b.n, b.p ? b.p : "", want);
    dyn_sb_free(&b);
    dyn_url_free(u);
}

static void test_url(void)
{
    url_vec("https://EXAMPLE.com/a/../b", "https://example.com/b");
    url_vec("http://[0:0:0:0:0:0:0:1]:80/x", "http://[::1]/x");
    url_vec("http://0x7f.1/", "http://127.0.0.1/");
    url_vec("https://example.com:443/", "https://example.com/");
    url_vec("http://example.com:443/", "http://example.com:443/");
    url_vec("https://b\xc3\xbc"
            "cher.de/",
        "https://xn--bcher-kva.de/");
    {
        dyn_url_t* u = (dyn_url_t*)calloc(1, sizeof *u);
        CHECK(dyn_url_parse(u, "http://exa mple.com/", 20, NULL) != 0, "WHATWG URL: space in a host is a failure");
        dyn_url_free(u);
    }
}

static void test_form(void)
{
    dyn_sp_list_t l;
    dyn_sb_t b;
    const char* q = "a=1&b=%20&c+d=e&&x";
    dyn_sp_list_init(&l);
    dyn_sp_parse(q, strlen(q), &l);
    CHECK(l.n == 4, "form parse pairs %zu", l.n);
    CHECK(l.n == 4 && !strcmp(l.a[1].v, " ") && !strcmp(l.a[2].k, "c d") && !strcmp(l.a[3].k, "x") && !strcmp(l.a[3].v, ""),
        "form parse decodes %%20 and + and a bare name");
    dyn_sb_init(&b);
    dyn_sp_serialize(&l, &b);
    CHECK(b.n == strlen("a=1&b=+&c+d=e&x=") && !memcmp(b.p, "a=1&b=+&c+d=e&x=", b.n),
        "form serialize -> %.*s", (int)b.n, b.p ? b.p : "");
    dyn_sb_free(&b);
    dyn_sp_list_free(&l);
}

int main(void)
{
    test_utf8();
    test_punycode();
    test_uts46();
    test_url();
    test_form();
    printf("test_idna_core: %d checks, %ld utf8 cases, %d failed\n", checks, utf8_cases, fails);
    return fails ? 1 : 0;
}
