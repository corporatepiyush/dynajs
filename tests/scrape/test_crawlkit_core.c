/*
 * test_crawlkit_core.c -- src/core/crawlkit (the dyna:scrape robots.txt
 * parser and matcher, SSRF host classifier, URL resolver) as a standalone C
 * library. Its HTTP date, Retry-After and response-header helpers moved to
 * src/core/httpk (shared with dyna:http); their oracles live in
 * tests/http/test_httpk_core.c. No engine is linked: the
 * Crawl-delay number conversion is a callback, here strtod in the C locale,
 * which the test also uses to see exactly what text reaches the converter.
 *
 * External oracles:
 *  - RFC 9309 section 5.1 example file (foobot / barbot+bazbot / quxbot / *),
 *    section 2.2.3 ".php$" wildcard table, section 2.2.2 longest match and the
 *    "allow wins a tie" rule, section 2.2.1 product-token matching.
 *  - RFC 9309 2.2.1: every group matching the agent is combined (equally
 *    specific matches; several "*" groups when none matches); a merged
 *    Crawl-delay is the largest of the groups' delays.
 *  - RFC 9309 2.2.2 percent-encoding table (row 4: "/foo/bar/%62%61%7A"
 *    matches "/foo/bar/baz"): rules and paths share one normaliser that
 *    decodes %XX unless the octet is reserved, non-ASCII, a control or space,
 *    which stay encoded with upper-case hex; so %2F, %2A and %24 never act
 *    as '/', a wildcard or an anchor.
 *  - RFC 6890 special-purpose IPv4/IPv6 registries for the SSRF classifier;
 *    an IP literal is decided by its address alone (a public IPv6 literal
 *    has no dot, which once sent it to the single-label rule): refused are
 *    ::, ::1, fe80::/10, fc00::/7, 2001:db8::/32 (RFC 3849), ff00::/8
 *    multicast, 64:ff9b:1::/48 (RFC 8215 local-use NAT64), 2001::/23 (IETF
 *    protocol assignments, incl. Teredo with an embedded IPv4), and
 *    ::ffff:0:0/96 or 64:ff9b::/96 (RFC 6052) embedding a private IPv4
 *    (64:ff9b:0:0:0:1:10.0.0.1 is outside the /96, so it is public);
 *    the embedded IPv4 is a strict dotted quad (RFC 4291 2.2, WHATWG URL:
 *    a trailing "1" or "1111" group is hex, not an IPv4 number), and a
 *    bracketed host that is no valid IPv6 address is refused. IPv4 literals
 *    take the WHATWG URL IPv4 parser forms (hex, octal, 1-3 part numbers)
 *    and are judged by address too: 134744072 (8.8.8.8) is public, 167772161
 *    (10.0.0.1) is refused.
 *  - RFC 3986 section 5.4.1/5.4.2 reference-resolution examples (fragments
 *    are stripped by this resolver, so the "#s" rows compare without them).
 *  - RFC 9110 section 5.6.7 IMF-fixdate example (784111777) and section 5.6.2
 *    tchar set for header names.
 *  - sitemaps.org protocol: the urlset and sitemapindex examples, the entity
 *    escaping table (&amp; &lt; &gt; &quot; &apos;), the 50,000-URL limit;
 *    an empty <loc/> (no URL) is dropped like a blank <loc> </loc>.
 *    The scanner is checked over every string up to length 5 on a 17-letter
 *    alphabet, copied to an unterminated heap buffer (ASan).
 * Pinned current behaviour (not specification), each named FINDING:
 *  - Crawl-delay accepts only digits[.digits]: "0x10", "1e2", "+5", "inf"
 *    never reach the converter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-crawlkit.h"

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

static int atod_calls;
static char atod_seen[64];

static double c_atod(const char* buf, const char** end)
{
    char* e;
    double d = strtod(buf, &e);
    atod_calls++;
    snprintf(atod_seen, sizeof atod_seen, "%s", buf);
    *end = e;
    return d;
}

static rb_t* parse(const char* txt, const char* agent)
{
    rb_t* r = (rb_t*)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->delay = -1.0;
    if (rb_parse(r, txt, strlen(txt), agent, c_atod) < 0) {
        rb_free(r);
        return NULL;
    }
    return r;
}

static int allows(const rb_t* r, const char* p) { return rb_allows_path(r, p, strlen(p)); }

static const char RFC_EXAMPLE[] = "User-Agent: *\n"
                                  "Disallow: *.gif$\n"
                                  "Disallow: /example/\n"
                                  "Allow: /publications/\n"
                                  "\n"
                                  "User-Agent: foobot\n"
                                  "Disallow:/\n"
                                  "Allow:/example/page.html\n"
                                  "Allow:/example/allowed.gif\n"
                                  "\n"
                                  "User-Agent: barbot\n"
                                  "User-Agent: bazbot\n"
                                  "Disallow: /example/page.html\n"
                                  "\n"
                                  "User-Agent: quxbot\n"
                                  "\n"
                                  "EOF\n";

static void test_rfc9309_groups(void)
{
    rb_t* r = parse(RFC_EXAMPLE, "FooBot/1.2");
    CHECK(r && allows(r, "/example/page.html") && allows(r, "/example/allowed.gif"), "foobot: the two Allow lines win");
    CHECK(r && !allows(r, "/example/other.html") && !allows(r, "/"), "foobot: Disallow:/ covers the rest");
    rb_free(r);
    r = parse(RFC_EXAMPLE, "bazbot");
    CHECK(r && !allows(r, "/example/page.html") && allows(r, "/example/allowed.gif") && allows(r, "/x.gif"),
        "bazbot shares barbot's group and not the * group");
    rb_free(r);
    r = parse(RFC_EXAMPLE, "quxbot");
    CHECK(r && r->n == 0 && allows(r, "/example/page.html"), "quxbot: an empty group allows everything");
    rb_free(r);
    r = parse(RFC_EXAMPLE, "otherbot");
    CHECK(r && !allows(r, "/x.gif") && !allows(r, "/example/z") && allows(r, "/publications/a") && allows(r, "/q"),
        "an unknown agent falls back to the * group");
    rb_free(r);
    r = parse("User-agent: a\nDisallow: /x\n\nUser-agent: a\nDisallow: /y\n", "a");
    CHECK(r && r->n == 2 && !allows(r, "/x") && !allows(r, "/y"), "same-agent groups are combined (RFC 9309 2.2.1)");
    rb_free(r);
    r = parse("User-agent: A\nCrawl-delay: 2\nDisallow: /x\n\nUser-agent: *\nDisallow: /z\n\nUser-agent: b\nUser-agent: a\n"
              "Crawl-delay: 5\nAllow: /x/ok\n\nUser-agent: a\nCrawl-delay: 1\n",
        "a/2.0");
    CHECK(r && r->n == 2 && !allows(r, "/x") && allows(r, "/x/ok") && allows(r, "/z") && r->delay == 5.0,
        "three groups naming a are merged (largest delay), the * group is not");
    rb_free(r);
    r = parse("User-agent: *\nDisallow: /p\n\nUser-agent: c\nDisallow: /c\n\nUser-agent: *\nDisallow: /q\nCrawl-delay: 4\n", "other");
    CHECK(r && r->n == 2 && !allows(r, "/p") && !allows(r, "/q") && allows(r, "/c") && r->delay == 4.0,
        "with no match every * group is combined");
    rb_free(r);
    r = parse("User-agent: a/1.0\nDisallow: /exact\n\nUser-agent: a\nDisallow: /token\n", "a/1.0");
    CHECK(r && r->n == 1 && !allows(r, "/exact") && allows(r, "/token"), "a full-agent match outranks a product-token match");
    rb_free(r);
    r = parse("\xEF\xBB\xBFUSER-AGENT: *\nDISALLOW: /p # note\n", "x");
    CHECK(r && r->n == 1 && !allows(r, "/p") && allows(r, "/q"), "BOM skipped, keys case-insensitive, # comment cut");
    rb_free(r);
}

static void test_rfc9309_match(void)
{
    static const char* const php_no[] = { "/index.php", "/filename.php", "/folder/filename.php" };
    static const char* const php_yes[] = { "/filename.php?parameters", "/filename.php/", "/filename.php5", "/windows.PHP" };
    size_t i;
    rb_t* r = parse("User-agent: *\nDisallow: /*.php$\n", "x");
    for (i = 0; i < sizeof php_no / sizeof php_no[0]; i++)
        CHECK(r && !allows(r, php_no[i]), "/*.php$ matches %s", php_no[i]);
    for (i = 0; i < sizeof php_yes / sizeof php_yes[0]; i++)
        CHECK(r && allows(r, php_yes[i]), "/*.php$ does not match %s", php_yes[i]);
    rb_free(r);
    r = parse("User-agent: *\nAllow: /example/page/\nDisallow: /example/page/disallowed.gif\n", "x");
    CHECK(r && allows(r, "/example/page/") && !allows(r, "/example/page/disallowed.gif"), "longest match wins");
    rb_free(r);
    r = parse("User-agent: *\nDisallow: /same\nAllow: /same\n", "x");
    CHECK(r && allows(r, "/same"), "an Allow and a Disallow of equal length: Allow wins");
    rb_free(r);
    r = parse("User-agent: *\nDisallow: /u/\xE3\x83\x84\nDisallow: /q?baz=quz\nDisallow: /foo/bar/%62%61%7A\n", "x");
    CHECK(r && !allows(r, "/u/%E3%83%84") && !allows(r, "/u/\xE3\x83\x84"), "UTF-8 rule matches raw and encoded paths");
    CHECK(r && !allows(r, "/q?baz=quz"), "a query in the rule is matched literally");
    CHECK(r && !allows(r, "/foo/bar/baz") && !allows(r, "/foo/bar/%62a%7a"), "an encoded-unreserved rule matches the decoded path");
    rb_free(r);
    r = parse("User-agent: *\nDisallow: /foo/bar/baz\nDisallow: /u/%e3%83%84\nDisallow: /s%20p\nDisallow: /a%2Fb\n"
              "Disallow: /w%2A\nDisallow: /d%24\nDisallow: /t%7e\nDisallow: /m$x\n",
        "x");
    CHECK(r && !allows(r, "/foo/bar/%62%61%7A"), "a plain rule matches an encoded-unreserved path");
    CHECK(r && !allows(r, "/u/%E3%83%84") && !allows(r, "/u/\xE3\x83\x84"), "lower-case hex in a rule matches either form");
    CHECK(r && !allows(r, "/s%20p") && !allows(r, "/s p"), "%%20 and a raw space are one octet");
    CHECK(r && allows(r, "/a/b") && !allows(r, "/a%2Fb") && !allows(r, "/a%2fb"), "%%2F is not a path separator");
    CHECK(r && allows(r, "/wxyz") && !allows(r, "/w*") && !allows(r, "/w%2A"), "%%2A is a literal star, not a wildcard");
    CHECK(r && allows(r, "/d") && !allows(r, "/d$") && !allows(r, "/d$x"), "%%24 is a literal dollar, not an anchor");
    CHECK(r && !allows(r, "/t~") && !allows(r, "/t%7E"), "%%7E is the unreserved tilde");
    CHECK(r && !allows(r, "/m$x") && !allows(r, "/m%24x") && allows(r, "/m"), "a $ inside a rule is a literal dollar");
    rb_free(r);
}

static double delay_of(const char* v)
{
    char txt[160];
    rb_t* r;
    double d;
    snprintf(txt, sizeof txt, "User-agent: *\nCrawl-delay: %s\n", v);
    r = parse(txt, "x");
    d = r ? r->delay : -2.0;
    rb_free(r);
    return d;
}

static void test_crawl_delay(void)
{
    static const char* const refused[] = { "0x10", "1e2", "+5", "-1", "inf", "nan", "2x", ".", "1_0", "" };
    char forty[41];
    size_t i;
    atod_calls = 0;
    CHECK(delay_of("2.5") == 2.5 && atod_calls == 1 && !strcmp(atod_seen, "2.5"), "2.5 is handed to the converter");
    CHECK(delay_of("5.") == 5.0 && !strcmp(atod_seen, "5."), "5. is digits[.digits]");
    CHECK(delay_of(".5") == 0.5, ".5 is digits[.digits]");
    CHECK(delay_of("007") == 7.0, "leading zeros are decimal");
    CHECK(delay_of("4\t") == 4.0, "trailing blanks are trimmed");
    atod_calls = 0;
    for (i = 0; i < sizeof refused / sizeof refused[0]; i++)
        CHECK(delay_of(refused[i]) == -1.0, "Crawl-delay \"%s\" refused", refused[i]);
    CHECK(atod_calls == 0, "a refused value never reaches the converter (%d calls)", atod_calls);
    memset(forty, '1', 40);
    forty[40] = 0;
    CHECK(delay_of(forty) == 1111111111111111111111111111111111111111.0 && strlen(atod_seen) == 40,
        "a 40-digit value reaches the converter whole (1.111e39)");
    forty[20] = '.';
    CHECK(delay_of(forty) == 11111111111111111111.1111111111111111111 && strlen(atod_seen) == 40,
        "a 40-character fraction reaches the converter whole");
    {
        rb_t* r = parse("Crawl-delay: 3\nUser-agent: *\nCrawl-delay: 2x\nCrawl-delay: 3\n", "x");
        CHECK(r && r->delay == 3.0, "a delay before any group is dropped; a bad one does not clear a good one");
        rb_free(r);
    }
}

static void test_robots_limits(void)
{
    const size_t cap = 1500 * 20 + 64;
    char* big = (char*)malloc(cap);
    size_t o = 0;
    int i;
    rb_t* r;
    o += (size_t)snprintf(big + o, cap - o, "User-agent: *\n");
    for (i = 0; i < 1500; i++)
        o += (size_t)snprintf(big + o, cap - o, "Disallow: /r%d/\n", i);
    o += (size_t)snprintf(big + o, cap - o, "Sitemap: https://e.com/s.xml\n");
    r = parse(big, "x");
    CHECK(r && r->n == 1000, "rules are capped at 1000 (%zu)", r ? r->n : 0);
    CHECK(r && r->n_site == 1 && !strcmp(r->sitemaps[0], "https://e.com/s.xml"), "Sitemap is outside any group");
    CHECK(r && !allows(r, "/r999/") && allows(r, "/r1000/"), "the 1001st rule is not kept");
    rb_free(r);
    free(big);
    CHECK(rb_token_len("foobot/1.2", 10) == 6 && rb_token_len("*", 1) == 1, "product token stops at /");
}

static void tag(const char* joined, const char* agent, const char* want)
{
    char* got = fe_parse_robots_tag(joined, agent);
    CHECK(want ? (got && !strcmp(got, want)) : got == NULL, "X-Robots-Tag(%s, %s) -> %s, want %s", joined,
        agent ? agent : "NULL", got ? got : "NULL", want ? want : "NULL");
    free(got);
}

static void test_robots_tag(void)
{
    tag("noindex, NOFOLLOW", NULL, "noindex\nnofollow");
    tag("none", NULL, "noindex\nnofollow");
    tag("all", NULL, NULL);
    tag("noindex, Noindex,noindex", NULL, "noindex");
    tag("googlebot: noindex", "Googlebot/2.1", "noindex");
    tag("otherbot: noindex", "googlebot", NULL);
    tag("max-snippet: 20", "googlebot", "max-snippet: 20");
}

static void test_ssrf(void)
{
    static const char* const priv[] = { "127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1",
        "169.254.169.254", "100.64.0.1", "0.0.0.0", "192.0.0.8", "224.0.0.1", "255.255.255.255", "0x7f.1",
        "2130706433", "017700000001", "0177.0.0.1", "0xa.0.0.1", "0300.0250.1.1.", "0x7f000001", "127.1", "[::1]", "[::]", "[::ffff:192.168.0.1]",
        "[::ffff:7f00:1]", "[fe80::1]", "[fc00::1]", "[fd12:3456::1]", "user:pw@localhost:8080", "LOCALHOST",
        "localhost.localdomain", "printer.local", "db.internal", "nas.lan", "intranet", "", "10.0.0.1:443",
        "[2001:db8::1]", "[2001:DB8:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF]", "[64:ff9b::10.0.0.1]", "[64:ff9b::7f00:1]",
        "[64:FF9B::169.254.169.254]", "[::ffff:127.0.0.1]", "[::ffff:10.1.2.3]", "[fe80::1%25eth0]", "[::1]:8080",
        "[::ffff:0x7f.1]", "[::ffff:127.1]", "[::ffff:127.0.0.01]", "[::ffff:8.8.8.08]", "[::ffff:8.8.8.8x]", "[::ffff:8.8.8.8.8]",
        "[ff00::]", "[ff02::1]", "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]", "[64:ff9b:1::]",
        "[64:ff9b:1:ffff:ffff:ffff:ffff:ffff]", "[64:ff9b:1::8.8.8.8]", "[2001::]", "[2001:1ff:ffff:ffff:ffff:ffff:ffff:ffff]",
        "[2001:0:4136:e378:8000:63bf:3fff:fdd2]", "167772161", "[1:2:3:4:5:6:7:8:9]", "[2606:4700::11111]" };
    static const char* const pub[] = { "example.com", "EXAMPLE.COM.", "8.8.8.8", "172.32.0.1", "100.128.0.1",
        "11.0.0.1", "100.63.255.255", "010.0.0.1", "a.b.c.d.e", "local.example.com", "[2606:4700::1111]",
        "[2001:4860:4860::8888]", "[2001:db9::1]", "[2001:db7:ffff::1]", "[64:ff9b::8.8.8.8]", "[::ffff:8.8.8.8]", "[64:ff9b:0:0:0:1:10.0.0.1]",
        "[2a00:1450:4001:80b::200e]:443", "[feff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]",
        "[64:ff9b:0:ffff:ffff:ffff:ffff:ffff]", "[64:ff9b:2::]", "[2001:200::]", "[2003::1]", "[2000:ffff:ffff:ffff:ffff:ffff:ffff:ffff]", "134744072" };
    size_t i;
    for (i = 0; i < sizeof priv / sizeof priv[0]; i++)
        CHECK(fe_host_is_private(priv[i]), "%s is private", priv[i]);
    for (i = 0; i < sizeof pub / sizeof pub[0]; i++)
        CHECK(!fe_host_is_private(pub[i]), "%s is public", pub[i]);
    {
        const unsigned char cg_lo[4] = { 100, 64, 0, 0 }, cg_hi[4] = { 100, 127, 255, 255 }, cg_out[4] = { 100, 128, 0, 0 };
        unsigned char v6[16];
        CHECK(fe_ip4_private(cg_lo) && fe_ip4_private(cg_hi) && !fe_ip4_private(cg_out), "RFC 6598 100.64/10 bounds");
        memset(v6, 0, sizeof v6);
        v6[0] = 0xfe;
        v6[1] = 0xbf;
        CHECK(fe_ip6_private(v6), "fe80::/10 upper edge");
        v6[1] = 0xc0;
        CHECK(!fe_ip6_private(v6), "fec0:: is outside fe80::/10");
    }
}

static void res(const char* ref, const char* want)
{
    char out[256];
    long n = fe_resolve("http://a/b/c/d;p?q", ref, out, sizeof out);
    CHECK(want ? (n == (long)strlen(want) && !strcmp(out, want)) : n < 0, "resolve(%s) -> %s, want %s", ref,
        n >= 0 ? out : "(error)", want ? want : "(error)");
}

static void test_resolve(void)
{
    res("g:h", "g:h");
    res("g", "http://a/b/c/g");
    res("./g", "http://a/b/c/g");
    res("g/", "http://a/b/c/g/");
    res("/g", "http://a/g");
    res("//g", "http://g");
    res("?y", "http://a/b/c/d;p?y");
    res("g?y", "http://a/b/c/g?y");
    res("g#s", "http://a/b/c/g");
    res("g?y#s", "http://a/b/c/g?y");
    res(";x", "http://a/b/c/;x");
    res("g;x", "http://a/b/c/g;x");
    res(".", "http://a/b/c/");
    res("./", "http://a/b/c/");
    res("..", "http://a/b/");
    res("../", "http://a/b/");
    res("../g", "http://a/b/g");
    res("../..", "http://a/");
    res("../../", "http://a/");
    res("../../g", "http://a/g");
    res("../../../g", "http://a/g");
    res("../../../../g", "http://a/g");
    res("/./g", "http://a/g");
    res("/../g", "http://a/g");
    res("g.", "http://a/b/c/g.");
    res(".g", "http://a/b/c/.g");
    res("g..", "http://a/b/c/g..");
    res("..g", "http://a/b/c/..g");
    res("./../g", "http://a/b/g");
    res("./g/.", "http://a/b/c/g/");
    res("g/./h", "http://a/b/c/g/h");
    res("g/../h", "http://a/b/c/h");
    res("g;x=1/./y", "http://a/b/c/g;x=1/y");
    res("g;x=1/../y", "http://a/b/c/y");
    res("a b", NULL);
    {
        char small[12];
        CHECK(fe_resolve("http://a/b/c/d;p?q", "/a-long-path", small, sizeof small) < 0, "output too small is an error");
        CHECK(fe_resolve("ftp://a/b", "g", small, sizeof small) < 0, "a non-http base is refused");
    }
}

static void test_split_date(void)
{
    char host[64];
    const char* path = NULL;
    CHECK(fe_split("https://e.com:8443/a?b#c", host, sizeof host, &path) == 1 && !strcmp(host, "e.com:8443")
            && !strcmp(path, "/a?b#c"),
        "https split");
    CHECK(fe_split("http://e.com", host, sizeof host, &path) == 0 && !strcmp(path, "/"), "an empty path is /");
    CHECK(fe_split("http://e.com?q", host, sizeof host, &path) == 0 && !strcmp(host, "e.com") && !strcmp(path, "?q"),
        "the host stops at ?");
    CHECK(fe_split("ftp://e.com/", host, sizeof host, &path) < 0, "ftp refused");
    CHECK(fe_split("http://e .com/", host, sizeof host, &path) < 0, "a space in the host is refused");
    CHECK(fe_split("http:///x", host, sizeof host, &path) < 0, "an empty host is refused");
    CHECK(fe_split("http://abcdefgh/", host, 8, &path) < 0, "a host larger than the buffer is refused");
    CHECK(fe_scheme_is_https("https://x") && !fe_scheme_is_https("HTTPS://x"), "scheme test is exact");
}

static int sm(const char* src, size_t n, fe_sm_doc_t* d, size_t* off)
{
    char* heap = (char*)malloc(n ? n : 1);
    int rc;
    memcpy(heap, src, n);
    rc = fe_sm_scan(heap, n, d, off);
    free(heap);
    return rc;
}

static void sm_err(const char* src, int code, size_t off)
{
    fe_sm_doc_t d;
    size_t got = 0;
    int rc = sm(src, strlen(src), &d, &got);
    CHECK(rc == code && got == off && d.n == 0 && d.v == NULL, "sitemap error %s: rc=%d off=%zu", src, rc, got);
}

static void test_sitemap(void)
{
    static const char* const proto = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                     "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n"
                                     "   <url>\n      <loc>http://www.example.com/</loc>\n"
                                     "      <lastmod>2005-01-01</lastmod>\n      <changefreq>monthly</changefreq>\n"
                                     "      <priority>0.8</priority>\n   </url>\n"
                                     "   <url>\n      <loc>http://www.example.com/catalog?item=12&amp;desc=vacation_hawaii</loc>\n"
                                     "      <changefreq>weekly</changefreq>\n   </url>\n"
                                     "   <url>\n      <loc>http://www.example.com/%C3%BCmlat.php&amp;q=name</loc>\n   </url>\n"
                                     "</urlset>\n";
    static const char* const index = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                     "<sitemapindex xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n"
                                     "   <sitemap>\n      <loc>http://www.example.com/sitemap1.xml.gz</loc>\n"
                                     "      <lastmod>2004-10-01T18:23:17+00:00</lastmod>\n   </sitemap>\n"
                                     "   <sitemap>\n      <loc>http://www.example.com/sitemap2.xml.gz</loc>\n   </sitemap>\n"
                                     "</sitemapindex>\n";
    fe_sm_doc_t d;
    size_t off;
    char msg[160];
    int rc;
    rc = sm(proto, strlen(proto), &d, &off);
    CHECK(rc == FE_SM_OK && d.n == 3 && !d.is_index && strcmp(d.v[0], "http://www.example.com/") == 0
            && strcmp(d.v[1], "http://www.example.com/catalog?item=12&desc=vacation_hawaii") == 0
            && strcmp(d.v[2], "http://www.example.com/%C3%BCmlat.php&q=name") == 0,
        "sitemaps.org urlset example: rc=%d n=%zu", rc, d.n);
    fe_sm_doc_free(&d);
    rc = sm(index, strlen(index), &d, &off);
    CHECK(rc == FE_SM_OK && d.n == 2 && d.is_index && strcmp(d.v[1], "http://www.example.com/sitemap2.xml.gz") == 0,
        "sitemaps.org index example: rc=%d n=%zu idx=%d", rc, d.n, d.is_index);
    fe_sm_doc_free(&d);
    {
        static const char* const ent = "<SitemapIndex><LOC> a&lt;&gt;&quot;&apos;&#65;&#x42;&#xe9;&#x20AC;&#x1F600;&bogus;&#;&#x110000; </LoC>";
        rc = sm(ent, strlen(ent), &d, &off);
    }
    CHECK(rc == FE_SM_OK && d.n == 1 && d.is_index
            && strcmp(d.v[0], "a<>\"'AB\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80&bogus;&#;&#x110000;") == 0,
        "entities decode, unknown ones stay literal, case-insensitive names: rc=%d %s", rc, d.n ? d.v[0] : "");
    fe_sm_doc_free(&d);
    {
        static const char* const skip = "<urlset><url><loc> \t\r\n </loc></url><!-- <loc>x</loc> --><![CDATA[<loc>y</loc>]]><?pi <loc>z</loc>?><!DOCTYPE x></urlset>";
        rc = sm(skip, strlen(skip), &d, &off);
    }
    CHECK(rc == FE_SM_OK && d.n == 0, "blank loc, comments, CDATA, PIs and declarations yield nothing: rc=%d n=%zu", rc, d.n);
    fe_sm_doc_free(&d);
    {
        static const char* const selfclose = "<urlset><url><loc/></url><url><loc>https://e.com/a</loc></url><url><loc a='1' /></url></urlset>";
        rc = sm(selfclose, strlen(selfclose), &d, &off);
        CHECK(rc == FE_SM_OK && d.n == 1 && strcmp(d.v[0], "https://e.com/a") == 0,
            "an empty <loc/> yields no URL and does not swallow the next entry (sitemaps.org: a loc must be a URL beginning with the protocol; out-of-scope entries are dropped) (rc=%d n=%zu)", rc, d.n);
        fe_sm_doc_free(&d);
    }
    rc = sm("<urlset><loc>a</lox>b</LOC><sitemapindex>", 41, &d, &off);
    CHECK(rc == FE_SM_OK && d.n == 1 && !d.is_index && strcmp(d.v[0], "a</lox>b") == 0,
        "only </loc> closes a loc, and only the root element decides the index flag (rc=%d)", rc);
    fe_sm_doc_free(&d);
    rc = sm("<urlset><loc>a</loc><loc>b</loc>", 32, &d, &off);
    CHECK(rc == FE_SM_OK && d.n == 2 && strcmp(d.v[1], "b") == 0, "two locs in order");
    fe_sm_doc_free(&d);

    sm_err("<urlset><!-- x", FE_SM_E_COMMENT, 8);
    sm_err("<urlset><?pi", FE_SM_E_PI, 8);
    sm_err("<a><![CDATA[ x ]>", FE_SM_E_CDATA, 3);
    sm_err("<a><!DOCTYPE", FE_SM_E_DECL, 3);
    sm_err("<a><url", FE_SM_E_TAG, 3);
    sm_err("<a><url x='1'", FE_SM_E_TAG, 3);
    sm_err("<a><loc x='1'", FE_SM_E_LOC_TAG, 3);
    sm_err("<a><loc>x", FE_SM_E_LOC_OPEN, 3);
    sm_err("<a><loc>x</lo", FE_SM_E_LOC_OPEN, 3);
    sm_err("<a><loc>x</loc", FE_SM_E_LOC_CLOSE, 8);
    {
        char buf[2200];
        size_t k;
        for (k = 0; k < 2; k++) {
            size_t body = 2048 + k;
            memcpy(buf, "<loc>", 5);
            memset(buf + 5, 'a', body);
            memcpy(buf + 5 + body, "</loc>", 6);
            rc = sm(buf, 11 + body, &d, &off);
            CHECK(k == 0 ? (rc == FE_SM_OK && d.n == 1 && strlen(d.v[0]) == 2048) : (rc == FE_SM_E_LOC_OPEN && off == 0),
                "loc text budget is %d bytes (body %zu: rc=%d)", FE_SM_MAX_LOC_TEXT, body, rc);
            fe_sm_doc_free(&d);
        }
    }
    {
        size_t k, cap = 12 * (FE_SM_MAX_URLS + 1);
        char* big = (char*)malloc(cap);
        for (k = 0; k <= FE_SM_MAX_URLS; k++)
            memcpy(big + 12 * k, "<loc>u</loc>", 12);
        rc = fe_sm_scan(big, cap - 12, &d, &off);
        CHECK(rc == FE_SM_OK && d.n == FE_SM_MAX_URLS, "exactly %d entries are kept (rc=%d n=%zu)", FE_SM_MAX_URLS, rc, d.n);
        fe_sm_doc_free(&d);
        rc = fe_sm_scan(big, cap, &d, &off);
        CHECK(rc == FE_SM_E_TOO_MANY && d.n == 0 && d.v == NULL, "entry %d is refused and the doc is freed (rc=%d)", FE_SM_MAX_URLS + 1, rc);
        free(big);
    }
    fe_sm_format_error(FE_SM_E_LOC_OPEN, 17, msg, sizeof msg);
    CHECK(strcmp(msg, "Sitemap.parse: <loc> at byte 17 never closes (over the 2048-byte url budget?)") == 0, "%s", msg);
    fe_sm_format_error(FE_SM_E_TOO_MANY, 0, msg, sizeof msg);
    CHECK(strcmp(msg, "Sitemap.parse: more than 50000 <loc> entries") == 0, "%s", msg);
    fe_sm_format_error(FE_SM_E_COMMENT, 8, msg, sizeof msg);
    CHECK(strcmp(msg, "Sitemap.parse: unterminated comment at byte 8") == 0, "%s", msg);
    {
        static const char alph[] = "<>/!-?[]&;#lcoL x";
        enum { NA = sizeof alph - 1 };
        int len, k, bad = 0;
        long total = 0, c;
        for (len = 1; len <= 5; len++) {
            long combos = 1;
            for (k = 0; k < len; k++)
                combos *= NA;
            for (c = 0; c < combos; c++) {
                long q = c;
                char src[8];
                for (k = 0; k < len; k++) {
                    src[k] = alph[q % NA];
                    q /= NA;
                }
                rc = sm(src, (size_t)len, &d, &off);
                if ((rc != FE_SM_OK && (off >= (size_t)len || d.n)) || (rc == FE_SM_OK && d.n > 1))
                    bad++;
                fe_sm_doc_free(&d);
                total++;
            }
        }
        CHECK(bad == 0, "sitemap scanner stays inside %ld unterminated inputs and reports in-range offsets (%d bad)", total, bad);
    }
}

int main(void)
{
    simd_init();
    test_rfc9309_groups();
    test_rfc9309_match();
    test_crawl_delay();
    test_robots_limits();
    test_robots_tag();
    test_ssrf();
    test_resolve();
    test_split_date();
    test_sitemap();
    printf("test_crawlkit_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
