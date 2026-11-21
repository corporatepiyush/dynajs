#include "rtc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

int rtc_addr_parse(const char* s, size_t n, uint16_t port, rtc_addr* out)
{
    char buf[64];
    size_t i;
    memset(out, 0, sizeof(*out));
    if (n == 0 || n >= sizeof(buf))
        return -1;
    if (s[0] == '[' && s[n - 1] == ']') {
        s++;
        n -= 2;
    }
    for (i = 0; i < n; i++)
        if (s[i] == '%')
            break;
    memcpy(buf, s, i);
    buf[i] = 0;
    if (inet_pton(AF_INET, buf, out->ip) == 1) {
        out->fam = 4;
        out->port = port;
        return 0;
    }
    if (inet_pton(AF_INET6, buf, out->ip) == 1) {
        out->fam = 6;
        out->port = port;
        return 0;
    }
    return -1;
}

size_t rtc_addr_fmt(const rtc_addr* a, char* buf, size_t cap)
{
    if (cap == 0)
        return 0;
    buf[0] = 0;
    if (!inet_ntop(a->fam == 4 ? AF_INET : AF_INET6, a->ip, buf, (socklen_t)cap))
        return 0;
    return strlen(buf);
}

int rtc_addr_is_loopback(const rtc_addr* a)
{
    static const uint8_t lo6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    if (a->fam == 4)
        return a->ip[0] == 127;
    return memcmp(a->ip, lo6, 16) == 0;
}

int rtc_addr_is_link_local(const rtc_addr* a)
{
    if (a->fam == 4)
        return a->ip[0] == 169 && a->ip[1] == 254;
    return a->ip[0] == 0xfe && (a->ip[1] & 0xc0) == 0x80;
}

int rtc_addr_host_ok(const rtc_addr* a)
{
    static const uint8_t z12[12] = { 0 };
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (rtc_addr_is_loopback(a) || rtc_addr_is_link_local(a))
        return 0;
    if (a->fam == 4)
        return a->ip[0] != 0;
    if (a->ip[0] == 0xfe && (a->ip[1] & 0xc0) == 0xc0)
        return 0;
    return memcmp(a->ip, z12, 12) != 0 && memcmp(a->ip, mapped, 12) != 0;
}

static uint32_t rtc_rotl32(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

#define RTC_QR(a, b, c, d)            \
    a += b, d = rtc_rotl32(d ^ a, 16); \
    c += d, b = rtc_rotl32(b ^ c, 12); \
    a += b, d = rtc_rotl32(d ^ a, 8);  \
    c += d, b = rtc_rotl32(b ^ c, 7)

void rtc_chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t out[64])
{
    uint32_t in[16], x[16];
    int i;
    in[0] = 0x61707865u;
    in[1] = 0x3320646eu;
    in[2] = 0x79622d32u;
    in[3] = 0x6b206574u;
    for (i = 0; i < 8; i++)
        in[4 + i] = key[i];
    in[12] = counter;
    in[13] = nonce[0];
    in[14] = nonce[1];
    in[15] = nonce[2];
    memcpy(x, in, sizeof(x));
    for (i = 0; i < 10; i++) {
        RTC_QR(x[0], x[4], x[8], x[12]);
        RTC_QR(x[1], x[5], x[9], x[13]);
        RTC_QR(x[2], x[6], x[10], x[14]);
        RTC_QR(x[3], x[7], x[11], x[15]);
        RTC_QR(x[0], x[5], x[10], x[15]);
        RTC_QR(x[1], x[6], x[11], x[12]);
        RTC_QR(x[2], x[7], x[8], x[13]);
        RTC_QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        out[4 * i] = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}

static void rtc_rng_refill(rtc_rng* r)
{
    uint32_t nonce[3];
    int i;
    nonce[0] = (uint32_t)(r->ctr >> 32);
    nonce[1] = 0;
    nonce[2] = 0;
    rtc_chacha20_block(r->key, (uint32_t)r->ctr, nonce, r->buf);
    r->ctr++;
    for (i = 0; i < 8; i++)
        r->key[i] = (uint32_t)r->buf[4 * i] | ((uint32_t)r->buf[4 * i + 1] << 8) | ((uint32_t)r->buf[4 * i + 2] << 16)
            | ((uint32_t)r->buf[4 * i + 3] << 24);
    r->pos = 32;
}

void rtc_rng_seed(rtc_rng* r, const uint8_t seed[32])
{
    int i;
    for (i = 0; i < 8; i++)
        r->key[i] = (uint32_t)seed[4 * i] | ((uint32_t)seed[4 * i + 1] << 8) | ((uint32_t)seed[4 * i + 2] << 16)
            | ((uint32_t)seed[4 * i + 3] << 24);
    r->ctr = 0;
    r->pos = 64;
}

void rtc_rng_bytes(rtc_rng* r, uint8_t* out, size_t n)
{
    while (n > 0) {
        size_t k;
        if (r->pos >= 64)
            rtc_rng_refill(r);
        k = 64 - r->pos;
        if (k > n)
            k = n;
        memcpy(out, r->buf + r->pos, k);
        memset(r->buf + r->pos, 0, k);
        r->pos += (uint32_t)k;
        out += k;
        n -= k;
    }
}

uint64_t rtc_rng_next(rtc_rng* r)
{
    uint8_t b[8];
    uint64_t v = 0;
    int i;
    rtc_rng_bytes(r, b, 8);
    for (i = 0; i < 8; i++)
        v = (v << 8) | b[i];
    return v;
}

uint32_t rtc_rng_u32(rtc_rng* r)
{
    return (uint32_t)(rtc_rng_next(r) >> 32);
}

void rtc_rng_alnum(rtc_rng* r, char* out, size_t n)
{
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i < n; i++)
        out[i] = al[rtc_rng_next(r) >> 58];
    out[n] = 0;
}

size_t rtc_hex_upper_colon(const uint8_t* p, size_t n, char* out, size_t cap)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t i, o = 0;
    if (cap < n * 3 || n == 0) {
        if (cap)
            out[0] = 0;
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[o++] = hx[p[i] >> 4];
        out[o++] = hx[p[i] & 15];
        out[o++] = i + 1 < n ? ':' : 0;
    }
    return n * 3 - 1;
}

static int rtc_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int rtc_hex_colon_parse(const char* s, size_t n, uint8_t* out, size_t cap)
{
    size_t i = 0;
    int k = 0;
    while (i < n) {
        int h, l;
        if (i + 2 > n || (size_t)k >= cap)
            return -1;
        h = rtc_hexval(s[i]);
        l = rtc_hexval(s[i + 1]);
        if (h < 0 || l < 0)
            return -1;
        out[k++] = (uint8_t)(h * 16 + l);
        i += 2;
        if (i < n) {
            if (s[i] != ':')
                return -1;
            i++;
            if (i == n)
                return -1;
        }
    }
    return k;
}
