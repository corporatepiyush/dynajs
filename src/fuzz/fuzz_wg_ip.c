#include "modules/wireguard/wg-ip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void fz_die(int line)
{
    fprintf(stderr, "fuzz_wg_ip: oracle failed at line %d\n", line);
    abort();
}

enum { E_BAD, E_CSUM, E_FRAG, E_NOTLOCAL, E_PROTO, E_UDP, E_NOPORT, E_ECHO, E_HOOK, E_N };
static const uint8_t L4A[4] = { 10, 0, 0, 2 }, L6A[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
static unsigned fz_hooks, fz_udps;
static size_t fz_udp_n;
static const uint8_t* fz_udp_p;

static uint16_t nb16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static int nsum_ok(const uint8_t* p, size_t n, uint32_t s)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2)
        s += (uint32_t)nb16(p + i);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return s == 0xffff;
}

static uint32_t npseudo(int v6, const uint8_t* s, const uint8_t* d, uint8_t proto, size_t len)
{
    uint32_t r = 0;
    size_t i, al = v6 ? 16 : 4;
    for (i = 0; i < al; i += 2)
        r += (uint32_t)nb16(s + i) + nb16(d + i);
    return r + proto + (uint32_t)(len & 0xffff) + (uint32_t)(len >> 16);
}

static int nsrc_ok(int v6, const uint8_t* a)
{
    static const uint8_t z[16];
    if (!v6)
        return !(a[0] == 0 || a[0] == 127 || a[0] >= 224);
    return !(a[0] == 0xff || !memcmp(a, z, 16) || (!memcmp(a, z, 15) && a[15] == 1));
}

static int nl4(int v6, const uint8_t* s, const uint8_t* d, uint8_t proto, const uint8_t* l4, size_t n)
{
    if (!nsrc_ok(v6, s))
        return E_BAD;
    if (memcmp(d, v6 ? L6A : L4A, v6 ? 16 : 4))
        return E_NOTLOCAL;
    if (proto == 17) {
        size_t ul;
        if (n < 8 || (ul = nb16(l4 + 4)) < 8 || ul > n)
            return E_BAD;
        if (nb16(l4 + 6) == 0 ? v6 : !nsum_ok(l4, ul, npseudo(v6, s, d, 17, ul)))
            return E_CSUM;
        return nb16(l4 + 2) == 53 ? E_UDP : E_NOPORT;
    }
    if (proto == (v6 ? 58 : 1)) {
        if (n < 8 || !nsum_ok(l4, n, v6 ? npseudo(1, s, d, 58, n) : 0))
            return E_CSUM;
        if (l4[0] == (v6 ? 128 : 8) && l4[1] == 0)
            return n > WG_IP_MTU - (v6 ? 40u : 20u) ? E_BAD : E_ECHO;
        return E_PROTO;
    }
    return proto == 6 ? E_HOOK : E_PROTO;
}

static int nopts(const uint8_t* o, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (o[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= n || i + 2 + (size_t)o[i + 1] > n || (o[i] != 1 && (o[i] & 0xc0)))
            return 0;
        i += 2 + o[i + 1];
    }
    return 1;
}

static int classify(const uint8_t* p, size_t n, size_t* l4o, size_t* l4n)
{
    if (!n)
        return E_BAD;
    if (p[0] >> 4 == 4) {
        size_t hl, tot;
        if (n < 20 || (hl = (size_t)(p[0] & 15) * 4) < 20 || hl > n || (tot = nb16(p + 2)) < hl || tot > n)
            return E_BAD;
        if (!nsum_ok(p, hl, 0))
            return E_CSUM;
        if ((p[6] & 0x3f) | p[7])
            return E_FRAG;
        *l4o = hl;
        *l4n = tot - hl;
        return nl4(0, p + 12, p + 16, p[9], p + hl, tot - hl);
    }
    if (p[0] >> 4 == 6) {
        size_t off = 40, end;
        unsigned k = 0;
        uint8_t nh;
        if (n < 40 || (end = 40 + (size_t)nb16(p + 4)) > n)
            return E_BAD;
        nh = p[6];
        while (nh == 0 || nh == 43 || nh == 60) {
            size_t l;
            if ((nh == 0 && k) || k == 8 || off + 8 > end)
                return E_BAD;
            l = 8 * ((size_t)p[off + 1] + 1);
            if (off + l > end || (nh == 43 ? p[off + 3] != 0 : !nopts(p + off + 2, l - 2)))
                return E_BAD;
            nh = p[off];
            off += l;
            k++;
        }
        if (nh == 44)
            return E_FRAG;
        *l4o = off;
        *l4n = end - off;
        return nl4(1, p + 8, p + 24, nh, p + off, end - off);
    }
    return E_BAD;
}

static uint64_t stat_of(const wg_ip_stats_t* s, int e)
{
    switch (e) {
    case E_BAD: return s->rx_bad;
    case E_CSUM: return s->rx_csum;
    case E_FRAG: return s->rx_frag;
    case E_NOTLOCAL: return s->rx_not_local;
    case E_PROTO: return s->rx_proto;
    case E_UDP: return s->udp_rx;
    case E_NOPORT: return s->udp_no_port;
    case E_ECHO: return s->echo;
    default: return fz_hooks;
    }
}

static void check_csum(const uint8_t* p, size_t n, unsigned cut)
{
    size_t c = n ? ((size_t)cut * 2) % (n + 1) & ~(size_t)1 : 0;
    uint16_t got = wg_csum_fin(wg_csum_acc(wg_csum_acc(0, p, c), p + c, n - c));
    uint8_t w[2];
    int ok;
    memcpy(w, &got, 2);
    ok = nsum_ok(p, n, (uint32_t)w[0] << 8 | w[1]);
    if (!ok && !(w[0] == 0 && w[1] == 0 && nsum_ok(p, n, 0xffff)))
        fz_die(__LINE__);
}

static wg_ip_t* fz_ip;
static uint64_t fz_ms;
static volatile size_t fz_rx;

static uint64_t fz_clock(void* ud)
{
    (void)ud;
    return fz_ms;
}

static int fz_out(void* ud, const uint8_t* p, size_t n)
{
    (void)ud;
    if (n < 20 || n > WG_IP_MTU)
        fz_die(__LINE__);
    if (p[0] >> 4 == 4 && (wg_csum_fin(wg_csum_acc(0, p, 20)) || ((size_t)p[2] << 8 | p[3]) != n))
        fz_die(__LINE__);
    if (p[0] >> 4 == 6 && ((size_t)p[4] << 8 | p[5]) + 40 != n)
        fz_die(__LINE__);
    return 0;
}

static void fz_udp(void* ud, wg_udp_t* s, const wg_ipaddr_t* src, uint16_t sport, const uint8_t* d, size_t n)
{
    (void)ud;
    (void)src;
    fz_udps++;
    fz_udp_n = n;
    fz_udp_p = d;
    if (n)
        fz_rx += d[0] + d[n - 1];
    wg_udp_sendto(s, src, sport, d, n > 64 ? 64 : n);
}

static void fz_tcp(void* ud, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* l4, size_t n)
{
    (void)ud;
    (void)src;
    fz_hooks++;
    (void)dst;
    if (n)
        fz_rx += l4[n - 1];
}

static void fz_setup(void)
{
    wg_ipaddr_t a;
    memset(&a, 0, sizeof a);
    fz_ip = wg_ip_new(fz_out, NULL);
    wg_ip_set_clock(fz_ip, fz_clock, NULL);
    a.a[0] = 10;
    a.a[3] = 2;
    wg_ip_add_addr(fz_ip, &a);
    memset(&a, 0, sizeof a);
    a.v6 = 1;
    a.a[0] = 0xfd;
    a.a[15] = 2;
    wg_ip_add_addr(fz_ip, &a);
    wg_udp_bind(fz_ip, 0, 53, fz_udp, NULL);
    wg_udp_bind(fz_ip, 1, 53, fz_udp, NULL);
    wg_ip_set_proto(fz_ip, WG_PROTO_TCP, fz_tcp, NULL);
}

static void fix(uint8_t* p, size_t n)
{
    wg_ipaddr_t s, d;
    size_t hl, off;
    uint8_t proto;
    uint16_t c;
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    if (p[0] >> 4 == 4) {
        hl = (size_t)(p[0] & 15) * 4;
        if (hl < 20 || hl > n)
            return;
        p[2] = (uint8_t)(n >> 8);
        p[3] = (uint8_t)n;
        p[10] = p[11] = 0;
        c = wg_csum_fin(wg_csum_acc(0, p, hl));
        memcpy(p + 10, &c, 2);
        memcpy(s.a, p + 12, 4);
        memcpy(d.a, p + 16, 4);
        proto = p[9];
        off = hl;
    } else {
        if (n < 40)
            return;
        p[4] = (uint8_t)((n - 40) >> 8);
        p[5] = (uint8_t)(n - 40);
        s.v6 = d.v6 = 1;
        memcpy(s.a, p + 8, 16);
        memcpy(d.a, p + 24, 16);
        proto = p[6];
        off = 40;
    }
    if ((proto == 17 && n - off >= 8) || ((proto == 1 || proto == 58) && n - off >= 4)) {
        size_t at = proto == 17 ? 6 : 2, l = n - off;
        if (proto == 17) {
            p[off + 4] = (uint8_t)(l >> 8);
            p[off + 5] = (uint8_t)l;
        }
        p[off + at] = p[off + at + 1] = 0;
        c = wg_csum_fin(wg_csum_acc(proto == 1 ? 0 : wg_csum_pseudo(&s, &d, proto, l), p + off, l));
        memcpy(p + off + at, &c, 2);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    uint8_t b[WG_IP_MTU];
    if (!fz_ip)
        fz_setup();
    if (!size)
        return 0;
    fz_ms += data[0];
    if (size - 1 > sizeof b)
        size = sizeof b + 1;
    memcpy(b, data + 1, size - 1);
    if (data[0] & 1 && size > 1)
        fix(b, size - 1);
    {
        wg_ip_stats_t s0, s1;
        uint64_t before[E_N];
        size_t l4o = 0, l4n = 0;
        unsigned udps = fz_udps;
        int want = classify(b, size - 1, &l4o, &l4n), e;
        wg_ip_stats(fz_ip, &s0);
        for (e = 0; e < E_N; e++)
            before[e] = stat_of(&s0, e);
        fz_udp_p = NULL;
        wg_ip_input(fz_ip, b, size - 1);
        wg_ip_stats(fz_ip, &s1);
        if (s1.rx != s0.rx + 1)
            fz_die(__LINE__);
        for (e = 0; e < E_N; e++)
            if (stat_of(&s1, e) != before[e] + (e == want))
                fz_die(__LINE__);
        if ((want == E_UDP) != (fz_udps == udps + 1) || (want == E_UDP && (fz_udp_n != (size_t)nb16(b + l4o + 4) - 8 || fz_udp_p != b + l4o + 8)))
            fz_die(__LINE__);
        check_csum(b, size - 1, data[0]);
    }
    return 0;
}
