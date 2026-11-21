/*
 * test_fmtcheck_core.c -- src/core/fmtcheck (the dyna:schema format
 * assertions and RFC 3986 reference helpers) as a standalone C library.
 *
 * Expected values come from the specifications, not from this code:
 *  - RFC 3339 section 5.8 examples (date-time) and its leap-second /
 *    February-29 rules (section 5.7 and Appendix C).
 *  - RFC 3986 section 5.2.4 remove_dot_segments worked examples, and
 *    section 3 component split of "foo://example.com:8042/over/there?name=ferret#nose".
 *  - RFC 4291 section 2.2 IPv6 text forms; RFC 2673/dotted-quad IPv4 with no
 *    leading zeros (JSON Schema 2020-12 format "ipv4").
 *  - RFC 4122 section 3 UUID example; RFC 6901 section 5 JSON Pointer examples
 *    and the relative-json-pointer draft examples ("0", "1/0", "2#").
 *  - RFC 1123 hostname label rules (63-octet labels, no leading/trailing "-").
 * The IDNA hook is exercised with a counting callback: hostname calls it only
 * for "xn--" labels, idn-hostname for non-ASCII input, and NULL means "not
 * checked" (the build without dyna:url).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-fmtcheck.h"

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

static int fm(const char* fmt, const char* s) { return dyn_fm_check(fmt, s, strlen(s), NULL); }

static void yes(const char* fmt, const char* s) { CHECK(fm(fmt, s) == 1, "%s accepts \"%s\"", fmt, s); }
static void no(const char* fmt, const char* s) { CHECK(fm(fmt, s) == 0, "%s refuses \"%s\"", fmt, s); }

static void test_datetime(void)
{
    yes("date-time", "1985-04-12T23:20:50.52Z");
    yes("date-time", "1996-12-19T16:39:57-08:00");
    yes("date-time", "1990-12-31T23:59:60Z");
    yes("date-time", "1990-12-31T15:59:60-08:00");
    yes("date-time", "1937-01-01T12:00:27.87+00:20");
    no("date-time", "1985-04-12 23:20:50Z");
    yes("date", "2000-02-29");
    no("date", "1900-02-29");
    no("date", "2021-04-31");
    yes("time", "23:20:50Z");
    no("time", "24:00:00Z");
    yes("duration", "P3Y6M4DT12H30M5S");
    yes("duration", "P4W");
    no("duration", "P");
    no("duration", "PT");
}

static void test_net(void)
{
    yes("ipv4", "192.168.0.1");
    no("ipv4", "192.168.00.1");
    no("ipv4", "256.0.0.1");
    yes("ipv6", "2001:DB8:0:0:8:800:200C:417A");
    yes("ipv6", "2001:DB8::8:800:200C:417A");
    yes("ipv6", "::1");
    yes("ipv6", "::");
    yes("ipv6", "::13.1.68.3");
    yes("ipv6", "::FFFF:129.144.52.38");
    no("ipv6", "1::2::3");
    no("ipv6", "12345::");
    yes("hostname", "www.example.com");
    no("hostname", "-bad.example");
    no("hostname", "bad-.example");
    {
        char l63[80], l64[80];
        memset(l63, 'a', 63);
        memcpy(l63 + 63, ".com", 5);
        memset(l64, 'a', 64);
        memcpy(l64 + 64, ".com", 5);
        yes("hostname", l63);
        no("hostname", l64);
    }
    yes("email", "joe.bloggs@example.com");
    yes("email", "\"joe bloggs\"@example.com");
    yes("email", "joe@[127.0.0.1]");
    yes("email", "joe@[IPv6:::1]");
    no("email", "joe..bloggs@example.com");
    no("email", ".joe@example.com");
    no("email", "joe@");
}

static int idna_calls, idna_answer;
static int idna_hook(const char* s, size_t n)
{
    (void)s;
    (void)n;
    idna_calls++;
    return idna_answer;
}

static void test_idna_hook(void)
{
    const char* a = "xn--bcher-kva.de";
    const char* u = "b\xc3\xbc"
                    "cher.de";
    idna_calls = 0;
    idna_answer = 0;
    CHECK(dyn_fm_check("hostname", "www.example.com", 15, idna_hook) == 1 && idna_calls == 0,
        "an all-LDH hostname never reaches the IDNA hook");
    CHECK(dyn_fm_check("hostname", a, strlen(a), idna_hook) == 0 && idna_calls == 1,
        "an xn-- label is handed to the IDNA hook and its refusal stands");
    CHECK(dyn_fm_check("hostname", a, strlen(a), NULL) == 1, "with no hook an xn-- label is not checked");
    CHECK(dyn_fm_check("idn-hostname", u, strlen(u), idna_hook) == 0 && idna_calls == 2,
        "a non-ASCII idn-hostname is decided by the hook");
    CHECK(dyn_fm_check("idn-hostname", u, strlen(u), NULL) == 1, "with no hook a non-ASCII idn-hostname is accepted");
    idna_answer = 1;
    CHECK(dyn_fm_check("idn-email", "j\xc3\xb6"
                                    "e@xn--bcher-kva.de",
              20, idna_hook)
                == 1
            && idna_calls == 3,
        "idn-email hands an xn-- domain label to the hook");
}

static void test_ids(void)
{
    yes("uuid", "f81d4fae-7dec-11d0-a765-00a0c91e6bf6");
    no("uuid", "f81d4fae7dec11d0a76500a0c91e6bf6");
    no("uuid", "f81d4fae-7dec-11d0-a765-00a0c91e6bfg");
    yes("json-pointer", "");
    yes("json-pointer", "/foo/0");
    yes("json-pointer", "/a~1b");
    yes("json-pointer", "/m~0n");
    no("json-pointer", "/a~2b");
    no("json-pointer", "foo");
    yes("relative-json-pointer", "0");
    yes("relative-json-pointer", "1/0");
    yes("relative-json-pointer", "2#");
    no("relative-json-pointer", "01/a");
    no("relative-json-pointer", "/a");
    yes("uri", "foo://example.com:8042/over/there?name=ferret#nose");
    yes("uri", "urn:example:animal:ferret:nose");
    no("uri", "//example.com/path");
    yes("uri-reference", "//example.com/path");
    no("uri", "http://exa mple.com/");
    yes("iri", "http://\xc3\xa9xample.com/");
    no("uri", "http://\xc3\xa9xample.com/");
    yes("uri-template", "http://example.com/{user}/{+path}{?q,lang}");
    no("uri-template", "http://example.com/{user");
    CHECK(fm("regex", "a+") == DYN_FM_UNKNOWN, "regex is left to the binding");
    CHECK(fm("no-such-format", "x") == DYN_FM_UNKNOWN, "an unknown format is reported as unknown");
}

static void dots(const char* in, const char* want)
{
    char buf[128];
    size_t n = strlen(in), o;
    memcpy(buf, in, n + 1);
    o = dyn_uri_remove_dots(buf, n);
    CHECK(o == strlen(want) && !memcmp(buf, want, o), "remove_dot_segments(%s) -> %.*s, want %s", in, (int)o, buf, want);
}

static void test_uri(void)
{
    dyn_uri_t u;
    const char* s = "foo://example.com:8042/over/there?name=ferret#nose";
    dots("/a/b/c/./../../g", "/a/g");
    dots("mid/content=5/../6", "mid/6");
    dots("/a/b/c/.", "/a/b/c/");
    dots("/a/b/c/..", "/a/b/");
    dots("../g", "g");
    dyn_uri_parse(s, &u);
    CHECK(u.has_scheme && u.sl == 3 && !memcmp(u.scheme, "foo", 3), "RFC 3986 scheme");
    CHECK(u.has_auth && u.al == 16 && !memcmp(u.auth, "example.com:8042", 16), "RFC 3986 authority");
    CHECK(u.pl == 11 && !memcmp(u.path, "/over/there", 11), "RFC 3986 path");
    CHECK(u.has_query && u.ql == 11 && !memcmp(u.query, "name=ferret", 11), "RFC 3986 query");
    CHECK(u.has_frag && u.fl == 4 && !memcmp(u.frag, "nose", 4), "RFC 3986 fragment");
    dyn_uri_parse("urn:example:animal:ferret:nose", &u);
    CHECK(u.has_scheme && !u.has_auth && u.pl == 26, "RFC 3986 urn has no authority");
}

int main(void)
{
    test_datetime();
    test_net();
    test_idna_hook();
    test_ids();
    test_uri();
    printf("test_fmtcheck_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
