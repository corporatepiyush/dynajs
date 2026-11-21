#include "netipk.h"
#include "../dyn-ctype.h"

static int parse_ipv4(const char* s, size_t n, uint8_t out[4])
{
    size_t i = 0;
    int field;

    for (field = 0; field < 4; field++) {
        unsigned val = 0;
        int digits = 0;
        size_t start = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            val = val * 10u + (unsigned)(s[i] - '0');
            digits++;
            if (digits > 3 || val > 255)
                return -1;
            i++;
        }
        if (digits == 0)
            return -1;
        if (digits > 1 && s[start] == '0')
            return -1;
        out[field] = (uint8_t)val;
        if (field < 3) {
            if (i >= n || s[i] != '.')
                return -1;
            i++;
        }
    }
    return (i == n) ? 0 : -1;
}

static int parse_ipv6(const char* s, size_t n, uint8_t out[16])
{
    int i;
    int ellipsis = -1;

    for (i = 0; i < 16; i++)
        out[i] = 0;

    if (n >= 2 && s[0] == ':' && s[1] == ':') {
        ellipsis = 0;
        s += 2;
        n -= 2;
        if (n == 0)
            return 0;
    }

    i = 0;
    while (i < 16) {
        uint32_t acc = 0;
        size_t off = 0;

        while (off < n) {
            int d = dyn_hex_digit((unsigned char)s[off]);
            if (d < 0)
                break;
            acc = (acc << 4) + (uint32_t)d;
            off++;
            if (off > 4)
                return -1;
            if (acc > 0xffff)
                return -1;
        }
        if (off == 0)
            return -1;

        if (off < n && s[off] == '.') {
            if (ellipsis < 0 && i != 12)
                return -1;
            if (i + 4 > 16)
                return -1;
            if (parse_ipv4(s, n, out + i) != 0)
                return -1;
            i += 4;
            n = 0;
            break;
        }

        out[i] = (uint8_t)(acc >> 8);
        out[i + 1] = (uint8_t)(acc & 0xff);
        i += 2;

        s += off;
        n -= off;
        if (n == 0)
            break;

        if (s[0] != ':')
            return -1;
        if (n == 1)
            return -1;
        s += 1;
        n -= 1;

        if (s[0] == ':') {
            if (ellipsis >= 0)
                return -1;
            ellipsis = i;
            s += 1;
            n -= 1;
            if (n == 0)
                break;
        }
    }

    if (n != 0)
        return -1;

    if (i < 16) {
        int nfill = 16 - i;
        int j;
        if (ellipsis < 0)
            return -1;
        for (j = i - 1; j >= ellipsis; j--)
            out[j + nfill] = out[j];
        for (j = ellipsis + nfill - 1; j >= ellipsis; j--)
            out[j] = 0;
    } else if (ellipsis >= 0) {
        return -1;
    }
    return 0;
}

int netipk_parse_addr(const char* s, size_t n, NetAddr* a)
{
    size_t i;

    memset(a->b, 0, sizeof(a->b));
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            a->is4 = 1;
            return parse_ipv4(s, n, a->b);
        }
        if (c == ':') {
            a->is4 = 0;
            return parse_ipv6(s, n, a->b);
        }
        if (c == '%')
            return -1;
    }
    return -1;
}

int netipk_parse_prefix(const char* s, size_t n, NetAddr* a, int* bits)
{
    long slash = -1;
    const char* bs;
    size_t bn, i;
    int v, maxb;

    for (i = n; i-- > 0;) {
        if (s[i] == '/') {
            slash = (long)i;
            break;
        }
    }
    if (slash < 0)
        return -1;
    if (netipk_parse_addr(s, (size_t)slash, a) != 0)
        return -1;

    bs = s + slash + 1;
    bn = n - (size_t)slash - 1;
    if (bn == 0 || bn > 3)
        return -1;
    if (bn > 1 && (bs[0] < '1' || bs[0] > '9'))
        return -1;
    v = 0;
    for (i = 0; i < bn; i++) {
        if (bs[i] < '0' || bs[i] > '9')
            return -1;
        v = v * 10 + (bs[i] - '0');
    }
    maxb = a->is4 ? 32 : 128;
    if (v > maxb)
        return -1;
    *bits = v;
    return 0;
}
