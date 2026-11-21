#include "crawlkit.h"

int fe_ip4_private(const unsigned char b[4])
{
    if (b[0] == 0)
        return 1;
    if (b[0] == 10)
        return 1;
    if (b[0] == 100 && (b[1] & 0xc0) == 64)
        return 1;
    if (b[0] == 127)
        return 1;
    if (b[0] == 169 && b[1] == 254)
        return 1;
    if (b[0] == 172 && (b[1] & 0xf0) == 16)
        return 1;
    if (b[0] == 192 && b[1] == 168)
        return 1;
    if (b[0] == 192 && b[1] == 0 && b[2] == 0)
        return 1;
    if (b[0] >= 224)
        return 1;
    return 0;
}

static int fe_ip4_val(const char** pp, unsigned long long* out)
{
    const char* p = *pp;
    unsigned long long v = 0;
    int base = 10, any = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && p[1] >= '0' && p[1] <= '9') {
        base = 8;
        p += 1;
    }
    for (;;) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        any = 1;
        v = v * (unsigned long long)base + (unsigned long long)d;
        if (v > 0xffffffffULL)
            return 0;
        p++;
    }
    if (!any)
        return 0;
    *pp = p;
    *out = v;
    return 1;
}

static int fe_ip4_parse(const char* s, unsigned char b[4])
{
    unsigned long long v[4], acc;
    const char* p = s;
    int n = 0, i;
    while (n < 4) {
        if (!fe_ip4_val(&p, &v[n]))
            return 0;
        n++;
        if (*p == '.') {
            p++;
            continue;
        }
        break;
    }
    if (*p == '.') {
        p++;
        if (*p != '\0')
            return 0;
    } else if (*p != '\0') {
        return 0;
    }
    if (n == 4) {
        for (i = 0; i < 4; i++)
            if (v[i] > 255ULL)
                return 0;
        acc = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    } else {
        unsigned long long lim = 1ULL << (8 * (4 - n + 1));
        for (i = 0; i < n - 1; i++)
            if (v[i] > 255ULL)
                return 0;
        if (v[n - 1] >= lim)
            return 0;
        acc = v[n - 1];
        for (i = n - 2; i >= 0; i--)
            acc |= v[i] << (8 * (3 - i));
    }
    b[0] = (unsigned char)((acc >> 24) & 0xff);
    b[1] = (unsigned char)((acc >> 16) & 0xff);
    b[2] = (unsigned char)((acc >> 8) & 0xff);
    b[3] = (unsigned char)(acc & 0xff);
    return 1;
}

static int fe_hex4(const char** pp, unsigned* out)
{
    const char* p = *pp;
    unsigned v = 0;
    int k = 0;
    while (k < 4) {
        int c = p[k], d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            break;
        v = (v << 4) | (unsigned)d;
        k++;
    }
    if (!k)
        return 0;
    *pp = p + k;
    *out = v;
    return 1;
}

static int fe_ip6_v4tail(const char* p, unsigned char v4[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        unsigned v = 0;
        int d = 0;
        if (k && *p++ != '.')
            return 0;
        if (*p == '0' && p[1] >= '0' && p[1] <= '9')
            return 0;
        while (*p >= '0' && *p <= '9' && d < 4) {
            v = v * 10 + (unsigned)(*p++ - '0');
            d++;
        }
        if (!d || v > 255)
            return 0;
        v4[k] = (unsigned char)v;
    }
    return *p == '\0';
}

static int fe_ip6_parse(const char* s, unsigned char b[16])
{
    unsigned g[8];
    int n = 0, dc = -1, i, fill;
    const char* p = s;
    memset(g, 0, sizeof g);
    if (*p == ':') {
        if (p[1] != ':')
            return 0;
        dc = 0;
        p += 2;
        if (*p == '\0') {
            memset(b, 0, 16);
            return 1;
        }
    }
    while (*p) {
        if (n >= 8)
            return 0;
        if (*p == ':') {
            if (p[1] == ':') {
                if (dc >= 0)
                    return 0;
                dc = n;
                p += 2;
                if (*p == '\0')
                    break;
                continue;
            }
            if (n == 0)
                return 0;
            p++;
            if (*p == '\0')
                return 0;
            continue;
        }
        {
            unsigned char v4[4];
            if (strchr(p, '.') && fe_ip6_v4tail(p, v4)) {
                if (n > 6)
                    return 0;
                g[n++] = ((unsigned)v4[0] << 8) | v4[1];
                g[n++] = ((unsigned)v4[2] << 8) | v4[3];
                break;
            }
        }
        if (!fe_hex4(&p, &g[n]))
            return 0;
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')
            || (*p >= 'A' && *p <= 'F'))
            return 0;
        n++;
    }
    if (dc < 0) {
        if (n != 8)
            return 0;
        for (i = 0; i < 8; i++) {
            b[2 * i] = (unsigned char)(g[i] >> 8);
            b[2 * i + 1] = (unsigned char)(g[i] & 0xff);
        }
        return 1;
    }
    if (n > 7)
        return 0;
    fill = 8 - n;
    for (i = 0; i < dc; i++) {
        b[2 * i] = (unsigned char)(g[i] >> 8);
        b[2 * i + 1] = (unsigned char)(g[i] & 0xff);
    }
    memset(b + 2 * dc, 0, (size_t)(2 * fill));
    for (i = dc; i < n; i++) {
        int j = i + fill;
        b[2 * j] = (unsigned char)(g[i] >> 8);
        b[2 * j + 1] = (unsigned char)(g[i] & 0xff);
    }
    return 1;
}

int fe_ip6_private(const unsigned char* b)
{
    static const unsigned char z12[12] = { 0 };
    int i;
    for (i = 0; i < 16; i++)
        if (b[i])
            break;
    if (i == 16)
        return 1;
    if (i == 15 && b[15] == 1)
        return 1;
    if (memcmp(b, z12, 10) == 0 && b[10] == 0xff && b[11] == 0xff)
        return fe_ip4_private(b + 12);
    if (memcmp(b, z12, 12) == 0)
        return fe_ip4_private(b + 12);
    if (memcmp(b, "\x00\x64\xff\x9b", 4) == 0 && memcmp(b + 4, z12, 8) == 0)
        return fe_ip4_private(b + 12);
    if (memcmp(b, "\x20\x01\x0d\xb8", 4) == 0)
        return 1;
    if (memcmp(b, "\x00\x64\xff\x9b\x00\x01", 6) == 0)
        return 1;
    if (b[0] == 0x20 && b[1] == 0x01 && (b[2] & 0xfe) == 0)
        return 1;
    if (b[0] == 0xff)
        return 1;
    if ((b[0] & 0xfe) == 0xfc)
        return 1;
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
        return 1;
    return 0;
}

static int fe_host_literal_private(const char* h)
{
    unsigned char b[4], b6[16];
    if (fe_ip4_parse(h, b))
        return fe_ip4_private(b);
    if (fe_ip6_parse(h, b6))
        return fe_ip6_private(b6);
    return -1;
}

static void fe_bare_host(const char* auth, char* out, size_t cap)
{
    const char* h = auth;
    const char* q;
    char* p;
    size_t n;
    for (q = auth; *q; q++)
        if (*q == '@')
            h = q + 1;
    n = strlen(h);
    if (n && h[0] == '[') {
        const char* cl = strchr(h, ']');
        if (cl) {
            h++;
            n = (size_t)(cl - h);
        }
    } else {
        const char* c = strchr(h, ':');
        if (c)
            n = (size_t)(c - h);
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(out, h, n);
    out[n] = 0;
    for (p = out; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
}

int fe_host_is_private(const char* host)
{
    static const char* const internal_sfx[] = { ".local", ".internal", ".lan" };
    char bare[300];
    size_t n, i;
    int lit;
    fe_bare_host(host, bare, sizeof bare);
    n = strlen(bare);
    if (n > 0 && bare[n - 1] == '.') {
        bare[n - 1] = '\0';
        n--;
    }
    for (i = 0; i < n; i++)
        if (bare[i] >= 'A' && bare[i] <= 'Z')
            bare[i] = (char)(bare[i] + 32);
    if (bare[0] == '\0')
        return 1;
    lit = fe_host_literal_private(bare);
    if (lit >= 0)
        return lit;
    if (strchr(bare, ':'))
        return 1;
    if (!strcmp(bare, "localhost") || !strcmp(bare, "localhost.localdomain"))
        return 1;
    if (!strchr(bare, '.'))
        return 1;
    for (i = 0; i < countof(internal_sfx); i++) {
        size_t k = strlen(internal_sfx[i]);
        if (n > k && !strcmp(bare + n - k, internal_sfx[i]))
            return 1;
    }
    return 0;
}
