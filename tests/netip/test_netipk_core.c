/*
 * test_netipk_core.c -- src/core/netipk (the dyna:netip address kernels:
 * parse, format, prefix parse/contains/mask, compare, classify) as a
 * standalone C library.
 *
 * Oracles are independent of the code. Parsing is checked against libc
 * inet_pton (AF_INET when '.' comes before any ':', else AF_INET6) over
 * every hand-picked edge string and 200k random strings drawn from the
 * address alphabet; both must agree on accept/reject and on the 16 bytes.
 * Darwin's inet_pton takes "01.2.3.4"; dyna:netip refuses a leading zero in
 * any dotted field (octal ambiguity, as Go's net/netip does), so the oracle
 * adds exactly that rule on top of inet_pton. Darwin also takes a 16-bit
 * group of five or more hex digits when the extras are leading zeros
 * ("00000::"); RFC 4291 allows at most four, as dyna:netip does.
 * Formatting is checked against an RFC 5952 writer here (lowercase, no
 * leading zeros, the longest run of two or more zero groups compressed,
 * first on a tie, ::ffff:a.b.c.d for IPv4-mapped), then re-parsed through
 * inet_pton. Prefixes are checked against a bit-by-bit compare and mask,
 * compare against memcmp ordering with IPv4 first, and classify against
 * the RFC 1918 / 3927 / 4193 / 4291 / 5771 tables written as CIDR lists,
 * applied after unmapping ::ffff:0:0/96.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "dyn-netipk.h"

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

static unsigned long long rs = 0x9e3779b97f4a7c15ull;
static unsigned rnd(unsigned n)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs % n);
}

static int oracle_parse(const char* s, NetAddr* a)
{
    char buf[128];
    const char* d = strchr(s, '.');
    const char* c = strchr(s, ':');
    size_t n = strlen(s);
    if (n >= sizeof(buf) || strchr(s, '%'))
        return -1;
    memcpy(buf, s, n + 1);
    memset(a->b, 0, 16);
    if (c) {
        int run = 0;
        const char* f;
        for (f = s; *f; f++) {
            run = (*f == ':' || *f == '.') ? 0 : run + 1;
            if (run > 4)
                return -1;
        }
    }
    if (d) {
        const char* f = strrchr(s, ':');
        f = f ? f + 1 : s;
        for (; *f; f++)
            if (*f == '0' && (f == s || f[-1] == '.' || f[-1] == ':') && f[1] >= '0' && f[1] <= '9')
                return -1;
    }
    if (d && (!c || d < c)) {
        a->is4 = 1;
        return inet_pton(AF_INET, buf, a->b) == 1 ? 0 : -1;
    }
    if (c) {
        a->is4 = 0;
        return inet_pton(AF_INET6, buf, a->b) == 1 ? 0 : -1;
    }
    return -1;
}

static void rfc5952(const NetAddr* a, char* out)
{
    unsigned g[8];
    int i, bs = -1, bl = 0;
    char* o = out;
    if (a->is4) {
        sprintf(out, "%u.%u.%u.%u", a->b[0], a->b[1], a->b[2], a->b[3]);
        return;
    }
    for (i = 0; i < 10; i++)
        if (a->b[i])
            break;
    if (i == 10 && a->b[10] == 0xff && a->b[11] == 0xff) {
        sprintf(out, "::ffff:%u.%u.%u.%u", a->b[12], a->b[13], a->b[14], a->b[15]);
        return;
    }
    for (i = 0; i < 8; i++)
        g[i] = (unsigned)a->b[2 * i] << 8 | a->b[2 * i + 1];
    for (i = 0; i < 8; i++) {
        int j = i;
        while (j < 8 && g[j] == 0)
            j++;
        if (j - i >= 2 && j - i > bl) {
            bl = j - i;
            bs = i;
        }
    }
    for (i = 0; i < 8; i++) {
        if (i == bs) {
            o += sprintf(o, "::");
            i += bl - 1;
            continue;
        }
        if (i > 0 && i != bs + bl)
            *o++ = ':';
        o += sprintf(o, "%x", g[i]);
    }
    *o = 0;
}

static void check_one(const char* s)
{
    NetAddr a, b;
    int r = netipk_parse_addr(s, strlen(s), &a);
    int w = oracle_parse(s, &b);
    CHECK((r == 0) == (w == 0), "parse accept '%s' got %d want %d", s, r, w);
    if (r == 0 && w == 0) {
        char f[64], g[64];
        size_t n;
        NetAddr c;
        CHECK(a.is4 == b.is4 && memcmp(a.b, b.b, a.is4 ? 4 : 16) == 0, "parse bytes '%s'", s);
        n = netipk_format_addr(&a, f);
        f[n] = 0;
        rfc5952(&a, g);
        CHECK(strcmp(f, g) == 0, "format '%s' got '%s' want '%s'", s, f, g);
        CHECK(oracle_parse(f, &c) == 0 && memcmp(c.b, a.b, 16) == 0, "reparse '%s'", f);
    }
}

static void rand_addr(NetAddr* a)
{
    int i, k = (int)rnd(6);
    a->is4 = rnd(3) == 0;
    memset(a->b, 0, 16);
    for (i = 0; i < (a->is4 ? 4 : 16); i++)
        a->b[i] = rnd(3) ? 0 : (uint8_t)rnd(256);
    if (!a->is4 && k == 0) {
        memset(a->b, 0, 10);
        a->b[10] = a->b[11] = 0xff;
    }
}

static int bit(const NetAddr* a, int i) { return a->b[i / 8] >> (7 - i % 8) & 1; }

typedef struct {
    const char* cidr;
    int cls;
} rule_t;

static const rule_t RULES[] = {
    { "127.0.0.0/8", CLS_LOOPBACK }, { "::1/128", CLS_LOOPBACK },
    { "10.0.0.0/8", CLS_PRIVATE }, { "172.16.0.0/12", CLS_PRIVATE }, { "192.168.0.0/16", CLS_PRIVATE }, { "fc00::/7", CLS_PRIVATE },
    { "224.0.0.0/4", CLS_MULTICAST }, { "ff00::/8", CLS_MULTICAST },
    { "169.254.0.0/16", CLS_LL_UNICAST }, { "fe80::/10", CLS_LL_UNICAST },
    { "224.0.0.0/24", CLS_LL_MULTICAST }, { "ff02::/16", CLS_LL_MULTICAST }, { "ff12::/16", CLS_LL_MULTICAST },
    { "ff22::/16", CLS_LL_MULTICAST }, { "ff32::/16", CLS_LL_MULTICAST }, { "ff42::/16", CLS_LL_MULTICAST },
    { "ff52::/16", CLS_LL_MULTICAST }, { "ff62::/16", CLS_LL_MULTICAST }, { "ff72::/16", CLS_LL_MULTICAST },
    { "ff82::/16", CLS_LL_MULTICAST }, { "ff92::/16", CLS_LL_MULTICAST }, { "ffa2::/16", CLS_LL_MULTICAST },
    { "ffb2::/16", CLS_LL_MULTICAST }, { "ffc2::/16", CLS_LL_MULTICAST }, { "ffd2::/16", CLS_LL_MULTICAST },
    { "ffe2::/16", CLS_LL_MULTICAST }, { "fff2::/16", CLS_LL_MULTICAST },
};

static int in_cidr(const char* cidr, const NetAddr* a)
{
    char buf[64];
    const char* sl = strchr(cidr, '/');
    NetAddr p;
    int bits = atoi(sl + 1), i;
    memcpy(buf, cidr, (size_t)(sl - cidr));
    buf[sl - cidr] = 0;
    oracle_parse(buf, &p);
    if (p.is4 != a->is4)
        return 0;
    for (i = 0; i < bits; i++)
        if (bit(&p, i) != bit(a, i))
            return 0;
    return 1;
}

static int oracle_class(const NetAddr* a0, int cls)
{
    NetAddr a = *a0;
    size_t i;
    int zero = 1;
    for (i = 0; i < (a0->is4 ? 4u : 16u); i++)
        zero &= a0->b[i] == 0;
    if (cls == CLS_UNSPECIFIED)
        return zero;
    if (!a.is4 && in_cidr("::ffff:0.0.0.0/96", &a)) {
        a.is4 = 1;
        memmove(a.b, a.b + 12, 4);
        memset(a.b + 4, 0, 12);
    }
    if (cls == CLS_GLOBAL_UNICAST) {
        int unspec = 1, bcast = a.is4;
        for (i = 0; i < (a.is4 ? 4u : 16u); i++) {
            unspec &= a.b[i] == 0;
            bcast &= a.b[i] == 255;
        }
        return !unspec && !bcast && !oracle_class(&a, CLS_LOOPBACK) && !oracle_class(&a, CLS_MULTICAST) && !oracle_class(&a, CLS_LL_UNICAST);
    }
    for (i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++)
        if (RULES[i].cls == cls && in_cidr(RULES[i].cidr, &a))
            return 1;
    return 0;
}

int main(void)
{
    static const char* EDGE[] = {
        "0.0.0.0", "255.255.255.255", "1.2.3.4", "01.2.3.4", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.4 ", " 1.2.3.4",
        "1..2.3", ".1.2.3", "1.2.3.", "1.2.3.04", "100.200.250.255", "1234.1.1.1", "", ".", ":", "::", ":::", "::1", "1::",
        "1::2::3", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "1:2:3:4:5:6::7:8",
        "12345::", "fffff::1", "::ffff:1.2.3.4", "::1.2.3.4", "1:2:3:4:5:6:1.2.3.4", "1:2:3:4:5:6:7:1.2.3.4",
        "1::1.2.3.4", "1.2.3.4::", "::ffff:1.2.3", "::ffff:01.2.3.4", "fe80::1%eth0", "FE80::ABCD", "0:0:0:0:0:0:0:0",
        "0:0:1:0:0:0:1:0", "1:0:0:1:0:0:1:1", "2001:db8::", "2001:db8:0:0:1:0:0:1", ":1::", "1:", "1::2:", "::ffff:255.255.255.255",
        "a:b:c:d:e:f:a:b", "0000:0000::", "00000::", "g::1", "1:2:3:4:5:6:7:8::", "::1:2:3:4:5:6:7:8", "::0:1:2:3:4:5:6",
    };
    static const char AL[] = "0123456789abcdefABCDEFg:::...%";
    size_t i;
    int k;

    for (i = 0; i < sizeof(EDGE) / sizeof(EDGE[0]); i++)
        check_one(EDGE[i]);
    for (k = 0; k < 200000; k++) {
        char s[48];
        int n = 1 + (int)rnd(40), j;
        for (j = 0; j < n; j++)
            s[j] = AL[rnd(sizeof(AL) - 1)];
        s[n] = 0;
        check_one(s);
    }
    for (k = 0; k < 100000; k++) {
        NetAddr a, b, p;
        char f[64], s[80];
        size_t n;
        int bits, got, want, i2, maxb, c, w;
        rand_addr(&a);
        n = netipk_format_addr(&a, f);
        f[n] = 0;
        check_one(f);
        maxb = a.is4 ? 32 : 128;
        bits = (int)rnd((unsigned)maxb + 1);
        b = a;
        for (i2 = 0; i2 < 3; i2++)
            if (rnd(2))
                b.b[rnd(a.is4 ? 4 : 16)] ^= (uint8_t)(1u << rnd(8));
        if (rnd(10) == 0)
            b.is4 = !b.is4;
        snprintf(s, sizeof(s), "%s/%d", f, bits);
        got = netipk_parse_prefix(s, strlen(s), &p, &i2);
        CHECK(got == 0 && i2 == bits && memcmp(p.b, a.b, 16) == 0, "prefix parse '%s'", s);
        want = a.is4 == b.is4;
        for (i2 = 0; want && i2 < bits; i2++)
            want = bit(&a, i2) == bit(&b, i2);
        CHECK(netipk_prefix_contains(&a, bits, &b) == want, "contains %s vs flip", s);
        CHECK(netipk_prefix_contains(&a, bits, &a) == 1, "contains self %s", s);
        p = a;
        netipk_mask(&p, bits);
        for (i2 = 0; i2 < maxb; i2++)
            if (bit(&p, i2) != (i2 < bits ? bit(&a, i2) : 0))
                break;
        CHECK(i2 == maxb, "mask %s bit %d", s, i2);
        for (i2 = maxb; i2 < 128; i2++)
            if (bit(&p, i2))
                break;
        CHECK(i2 == 128, "mask spill %s", s);
        c = netipk_compare(&a, &b);
        if (a.is4 != b.is4)
            w = a.is4 ? -1 : 1;
        else {
            w = memcmp(a.b, b.b, a.is4 ? 4 : 16);
            w = (w > 0) - (w < 0);
        }
        CHECK(c == w && netipk_compare(&b, &a) == -w, "compare %s", s);
        for (i2 = CLS_LOOPBACK; i2 <= CLS_LL_MULTICAST; i2++)
            CHECK(netipk_class(&a, i2) == oracle_class(&a, i2), "class %d of %s", i2, f);
    }
    {
        static const char* BADP[] = { "1.2.3.4/33", "1.2.3.4/", "1.2.3.4/01", "1.2.3.4/-1", "::/129", "::/1000", "1.2.3.4", "/8", "1.2.3/8", "::/1x", "1.2.3.4/8/8" };
        NetAddr p;
        int bits;
        for (i = 0; i < sizeof(BADP) / sizeof(BADP[0]); i++)
            CHECK(netipk_parse_prefix(BADP[i], strlen(BADP[i]), &p, &bits) != 0, "prefix reject '%s'", BADP[i]);
        CHECK(netipk_parse_prefix("::/0", 4, &p, &bits) == 0 && bits == 0, "prefix ::/0");
        CHECK(netipk_class(&p, 99) == 0, "unknown class");
    }
    printf("%s test_netipk_core: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
