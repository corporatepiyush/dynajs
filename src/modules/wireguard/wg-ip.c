#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "wg-ip.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint8_t proto;
    wg_ip_proto_fn fn;
    void* ud;
} wg_hook_t;

typedef struct {
    wg_udp_t* s[256];
    unsigned n;
} wg_upage_t;

struct wg_udp {
    wg_ip_t* ip;
    int v6;
    uint16_t port;
    wg_udp_rx_fn rx;
    void* ud;
};

struct wg_ip {
    wg_ip_out_fn out;
    void* ud;
    wg_ip_clock_fn clock;
    void* clock_ud;
    wg_ipaddr_t addr[WG_IP_MAX_ADDRS];
    unsigned naddr;
    wg_hook_t hook[WG_IP_MAX_HOOKS];
    unsigned nhook;
    wg_upage_t* pg[2][256];
    uint16_t eph[2];
    uint16_t id;
    unsigned err_tokens;
    int trust;
    uint64_t err_ms;
    wg_ip_stats_t st;
    uint8_t tx[WG_IP_MTU];
};

static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

uint64_t wg_csum_acc(uint64_t acc, const uint8_t* p, size_t n)
{
    uint32_t w[4];
    while (n >= 16) {
        memcpy(w, p, 16);
        acc += (uint64_t)w[0] + w[1] + w[2] + w[3];
        p += 16;
        n -= 16;
    }
    while (n >= 4) {
        memcpy(w, p, 4);
        acc += w[0];
        p += 4;
        n -= 4;
    }
    if (n) {
        w[0] = 0;
        memcpy(w, p, n);
        acc += w[0];
    }
    return acc;
}

uint16_t wg_csum_fin(uint64_t acc)
{
    acc = (acc & 0xffffffffu) + (acc >> 32);
    acc = (acc & 0xffffffffu) + (acc >> 32);
    acc = (acc & 0xffffu) + (acc >> 16);
    acc = (acc & 0xffffu) + (acc >> 16);
    return (uint16_t)~acc;
}

uint64_t wg_csum_pseudo(const wg_ipaddr_t* src, const wg_ipaddr_t* dst, uint8_t proto, size_t len)
{
    uint8_t b[40];
    size_t al = src->v6 ? 16 : 4;
    memcpy(b, src->a, al);
    memcpy(b + al, dst->a, al);
    if (src->v6) {
        b[32] = (uint8_t)(len >> 24);
        b[33] = (uint8_t)(len >> 16);
        b[34] = (uint8_t)(len >> 8);
        b[35] = (uint8_t)len;
        b[36] = b[37] = b[38] = 0;
        b[39] = proto;
        return wg_csum_acc(0, b, 40);
    }
    b[8] = 0;
    b[9] = proto;
    put16(b + 10, (unsigned)len);
    return wg_csum_acc(0, b, 12);
}

static uint64_t mono_ms(void* ud)
{
    struct timespec ts;
    (void)ud;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

wg_ip_t* wg_ip_new(wg_ip_out_fn out, void* ud)
{
    wg_ip_t* ip = (wg_ip_t*)calloc(1, sizeof *ip);
    if (!ip)
        return NULL;
    ip->out = out;
    ip->ud = ud;
    ip->clock = mono_ms;
    ip->eph[0] = ip->eph[1] = WG_IP_EPH_LO;
    ip->err_tokens = WG_IP_ERR_BURST;
    ip->err_ms = mono_ms(NULL);
    return ip;
}

void wg_ip_free(wg_ip_t* ip)
{
    unsigned f, i, k;
    if (!ip)
        return;
    for (f = 0; f < 2; f++)
        for (i = 0; i < 256; i++) {
            if (!ip->pg[f][i])
                continue;
            for (k = 0; k < 256; k++)
                free(ip->pg[f][i]->s[k]);
            free(ip->pg[f][i]);
        }
    free(ip);
}

void wg_ip_set_clock(wg_ip_t* ip, wg_ip_clock_fn fn, void* ud)
{
    ip->clock = fn ? fn : mono_ms;
    ip->clock_ud = ud;
    ip->err_ms = ip->clock(ud);
}

int wg_ip_add_addr(wg_ip_t* ip, const wg_ipaddr_t* a)
{
    if (ip->naddr == WG_IP_MAX_ADDRS)
        return -1;
    ip->addr[ip->naddr] = *a;
    if (!a->v6)
        memset(ip->addr[ip->naddr].a + 4, 0, 12);
    ip->naddr++;
    return 0;
}

const wg_ipaddr_t* wg_ip_src_for(const wg_ip_t* ip, int v6)
{
    unsigned i;
    for (i = 0; i < ip->naddr; i++)
        if (ip->addr[i].v6 == v6)
            return &ip->addr[i];
    return NULL;
}

int wg_ip_set_proto(wg_ip_t* ip, uint8_t proto, wg_ip_proto_fn fn, void* ud)
{
    unsigned i;
    for (i = 0; i < ip->nhook; i++)
        if (ip->hook[i].proto == proto)
            break;
    if (i == WG_IP_MAX_HOOKS)
        return -1;
    ip->hook[i].proto = proto;
    ip->hook[i].fn = fn;
    ip->hook[i].ud = ud;
    if (i == ip->nhook)
        ip->nhook++;
    return 0;
}

void wg_ip_stats(const wg_ip_t* ip, wg_ip_stats_t* st)
{
    *st = ip->st;
}

static int is_local(const wg_ip_t* ip, const wg_ipaddr_t* a)
{
    unsigned i;
    for (i = 0; i < ip->naddr; i++)
        if (ip->addr[i].v6 == a->v6 && memcmp(ip->addr[i].a, a->a, a->v6 ? 16 : 4) == 0)
            return 1;
    return 0;
}

static int src_ok(const wg_ipaddr_t* a)
{
    static const uint8_t z[16];
    if (!a->v6)
        return a->a[0] != 0 && a->a[0] != 127 && a->a[0] < 224;
    if (a->a[0] == 0xff || memcmp(a->a, z, 16) == 0)
        return 0;
    return !(memcmp(a->a, z, 15) == 0 && a->a[15] == 1);
}

static int emit(wg_ip_t* ip, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, uint8_t proto, size_t n)
{
    uint8_t* h = ip->tx;
    if (!src || src->v6 != dst->v6) {
        ip->st.tx_fail++;
        return -1;
    }
    if (dst->v6) {
        memset(h, 0, WG_IP_HDR6);
        h[0] = 0x60;
        put16(h + 4, (unsigned)n);
        h[6] = proto;
        h[7] = WG_IP_TTL;
        memcpy(h + 8, src->a, 16);
        memcpy(h + 24, dst->a, 16);
        n += WG_IP_HDR6;
    } else {
        uint16_t c;
        n += WG_IP_HDR4;
        h[0] = 0x45;
        h[1] = 0;
        put16(h + 2, (unsigned)n);
        put16(h + 4, ip->id++);
        put16(h + 6, 0x4000);
        h[8] = WG_IP_TTL;
        h[9] = proto;
        h[10] = h[11] = 0;
        memcpy(h + 12, src->a, 4);
        memcpy(h + 16, dst->a, 4);
        c = wg_csum_fin(wg_csum_acc(0, h, WG_IP_HDR4));
        memcpy(h + 10, &c, 2);
    }
    if (ip->out(ip->ud, h, n) < 0) {
        ip->st.tx_fail++;
        return -1;
    }
    ip->st.tx++;
    return 0;
}

static size_t hdr_len(int v6)
{
    return v6 ? WG_IP_HDR6 : WG_IP_HDR4;
}

static void l4_csum(uint8_t* l4, size_t n, size_t at, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, uint8_t proto, int pseudo, int zero_ff)
{
    uint16_t c;
    l4[at] = l4[at + 1] = 0;
    c = wg_csum_fin(wg_csum_acc(pseudo ? wg_csum_pseudo(src, dst, proto, n) : 0, l4, n));
    if (zero_ff && !c)
        c = 0xffff;
    memcpy(l4 + at, &c, 2);
}

int wg_ip_output(wg_ip_t* ip, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, uint8_t proto, const uint8_t* l4, size_t n)
{
    size_t hl = hdr_len(dst->v6);
    if (n > WG_IP_MTU - hl) {
        ip->st.tx_fail++;
        return -1;
    }
    memcpy(ip->tx + hl, l4, n);
    return emit(ip, src ? src : wg_ip_src_for(ip, dst->v6), dst, proto, n);
}

static int err_allowed(wg_ip_t* ip)
{
    uint64_t now = ip->clock(ip->clock_ud);
    uint64_t add = now > ip->err_ms ? (now - ip->err_ms) / WG_IP_ERR_EVERY_MS : 0;
    if (add) {
        ip->err_tokens = add + ip->err_tokens >= WG_IP_ERR_BURST ? WG_IP_ERR_BURST : (unsigned)(add + ip->err_tokens);
        ip->err_ms += add * WG_IP_ERR_EVERY_MS;
    }
    if (!ip->err_tokens) {
        ip->st.unreach_limited++;
        return 0;
    }
    ip->err_tokens--;
    return 1;
}

static void port_unreach(wg_ip_t* ip, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* pkt, size_t pn)
{
    size_t hl = hdr_len(src->v6), cap = (src->v6 ? WG_IP_ERR6_MAX : WG_IP_ERR4_MAX) - hl - 8, q = pn < cap ? pn : cap;
    uint8_t* m = ip->tx + hl;
    if (!err_allowed(ip))
        return;
    m[0] = src->v6 ? 1 : 3;
    m[1] = src->v6 ? 4 : 3;
    memset(m + 4, 0, 4);
    memcpy(m + 8, pkt, q);
    l4_csum(m, q + 8, 2, dst, src, WG_PROTO_ICMP6, src->v6, 0);
    if (emit(ip, dst, src, src->v6 ? WG_PROTO_ICMP6 : WG_PROTO_ICMP, q + 8) == 0)
        ip->st.unreach_sent++;
}

static wg_udp_t* port_get(const wg_ip_t* ip, int v6, uint16_t port)
{
    const wg_upage_t* g = ip->pg[v6][port >> 8];
    return g ? g->s[port & 255] : NULL;
}

static void udp_in(wg_ip_t* ip, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* l4, size_t n, const uint8_t* pkt, size_t pn)
{
    size_t ul;
    wg_udp_t* s;
    if (n < WG_UDP_HDR || (ul = be16(l4 + 4)) < WG_UDP_HDR || ul > n) {
        ip->st.rx_bad++;
        return;
    }
    if ((src->v6 || l4[6] || l4[7]) && (!(l4[6] | l4[7]) || (!wg_ip_take_trust(ip) && wg_csum_fin(wg_csum_acc(wg_csum_pseudo(src, dst, WG_PROTO_UDP, ul), l4, ul))))) {
        ip->st.rx_csum++;
        return;
    }
    s = port_get(ip, src->v6, be16(l4 + 2));
    if (!s) {
        ip->st.udp_no_port++;
        port_unreach(ip, src, dst, pkt, pn);
        return;
    }
    ip->st.udp_rx++;
    s->rx(s->ud, s, src, be16(l4), l4 + WG_UDP_HDR, ul - WG_UDP_HDR);
}

static int call_hook(wg_ip_t* ip, uint8_t proto, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* l4, size_t n)
{
    unsigned i;
    for (i = 0; i < ip->nhook; i++)
        if (ip->hook[i].proto == proto && ip->hook[i].fn) {
            ip->hook[i].fn(ip->hook[i].ud, src, dst, l4, n);
            return 1;
        }
    return 0;
}

static void icmp_in(wg_ip_t* ip, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* l4, size_t n)
{
    int v6 = src->v6;
    uint8_t proto = v6 ? WG_PROTO_ICMP6 : WG_PROTO_ICMP;
    size_t hl = hdr_len(v6);
    if (n < 8 || (!wg_ip_take_trust(ip) && wg_csum_fin(wg_csum_acc(v6 ? wg_csum_pseudo(src, dst, proto, n) : 0, l4, n)))) {
        ip->st.rx_csum++;
        return;
    }
    if (l4[0] == (v6 ? 128 : 8) && l4[1] == 0) {
        uint8_t* m = ip->tx + hl;
        if (n > WG_IP_MTU - hl) {
            ip->st.rx_bad++;
            return;
        }
        memcpy(m, l4, n);
        m[0] = v6 ? 129 : 0;
        l4_csum(m, n, 2, dst, src, proto, v6, 0);
        if (emit(ip, dst, src, proto, n) == 0)
            ip->st.echo++;
        return;
    }
    if (!call_hook(ip, proto, src, dst, l4, n))
        ip->st.rx_proto++;
}

static void deliver(wg_ip_t* ip, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, uint8_t proto, const uint8_t* l4, size_t n, const uint8_t* pkt, size_t pn)
{
    if (!src_ok(src)) {
        ip->st.rx_bad++;
        return;
    }
    if (!is_local(ip, dst)) {
        ip->st.rx_not_local++;
        return;
    }
    if (proto == WG_PROTO_UDP)
        udp_in(ip, src, dst, l4, n, pkt, pn);
    else if (proto == (src->v6 ? WG_PROTO_ICMP6 : WG_PROTO_ICMP))
        icmp_in(ip, src, dst, l4, n);
    else if (!call_hook(ip, proto, src, dst, l4, n))
        ip->st.rx_proto++;
}

static void in4(wg_ip_t* ip, const uint8_t* p, size_t n)
{
    size_t hl, tot;
    wg_ipaddr_t s, d;
    if (n < WG_IP_HDR4 || (hl = (size_t)(p[0] & 15) * 4) < WG_IP_HDR4 || hl > n || (tot = be16(p + 2)) < hl || tot > n) {
        ip->st.rx_bad++;
        return;
    }
    if (!ip->trust && wg_csum_fin(wg_csum_acc(0, p, hl))) {
        ip->st.rx_csum++;
        return;
    }
    if (be16(p + 6) & 0x3fff) {
        ip->st.rx_frag++;
        return;
    }
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    memcpy(s.a, p + 12, 4);
    memcpy(d.a, p + 16, 4);
    deliver(ip, &s, &d, p[9], p + hl, tot - hl, p, tot);
}

static int opts_ok(const uint8_t* o, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (o[i] == 0) {
            i++;
            continue;
        }
        if (i + 2 > n || i + 2 + o[i + 1] > n)
            return 0;
        if (o[i] != 1 && (o[i] >> 6))
            return 0;
        i += 2 + (size_t)o[i + 1];
    }
    return 1;
}

static void in6(wg_ip_t* ip, const uint8_t* p, size_t n)
{
    size_t off = WG_IP_HDR6, end;
    unsigned k;
    uint8_t nh;
    wg_ipaddr_t s, d;
    if (n < WG_IP_HDR6 || (end = WG_IP_HDR6 + (size_t)be16(p + 4)) > n) {
        ip->st.rx_bad++;
        return;
    }
    nh = p[6];
    for (k = 0; nh == 0 || nh == 43 || nh == 60; k++) {
        size_t l;
        if ((nh == 0 && k) || k == WG_IP_EXT_MAX || off + 8 > end || off + (l = ((size_t)p[off + 1] + 1) * 8) > end || (nh == 43 ? p[off + 3] != 0 : !opts_ok(p + off + 2, l - 2))) {
            ip->st.rx_bad++;
            return;
        }
        nh = p[off];
        off += l;
    }
    if (nh == 44) {
        ip->st.rx_frag++;
        return;
    }
    s.v6 = d.v6 = 1;
    memcpy(s.a, p + 8, 16);
    memcpy(d.a, p + 24, 16);
    deliver(ip, &s, &d, nh, p + off, end - off, p, end);
}

static void input(wg_ip_t* ip, const uint8_t* pkt, size_t n, int trust)
{
    int o = ip->trust;
    ip->trust = trust;
    ip->st.rx++;
    if (n && pkt[0] >> 4 == 4)
        in4(ip, pkt, n);
    else if (n && pkt[0] >> 4 == 6)
        in6(ip, pkt, n);
    else
        ip->st.rx_bad++;
    ip->trust = o;
}

void wg_ip_input(wg_ip_t* ip, const uint8_t* pkt, size_t n)
{
    input(ip, pkt, n, 0);
}

void wg_ip_input_trusted(wg_ip_t* ip, const uint8_t* pkt, size_t n)
{
    input(ip, pkt, n, 1);
}

int wg_ip_take_trust(wg_ip_t* ip)
{
    if (!ip->trust)
        return 0;
    ip->st.csum_skipped++;
    return 1;
}

int wg_ip_verify(const uint8_t* p, size_t n)
{
    wg_ipaddr_t s, d;
    const uint8_t* l4;
    size_t ln;
    uint8_t proto;
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    if (n >= WG_IP_HDR4 && p[0] == 0x45) {
        if (be16(p + 2) != n || (be16(p + 6) & 0x3fff) || wg_csum_fin(wg_csum_acc(0, p, WG_IP_HDR4)))
            return 0;
        memcpy(s.a, p + 12, 4);
        memcpy(d.a, p + 16, 4);
        proto = p[9];
        l4 = p + WG_IP_HDR4;
        ln = n - WG_IP_HDR4;
    } else if (n >= WG_IP_HDR6 && p[0] >> 4 == 6) {
        if ((size_t)be16(p + 4) + WG_IP_HDR6 != n)
            return 0;
        s.v6 = d.v6 = 1;
        memcpy(s.a, p + 8, 16);
        memcpy(d.a, p + 24, 16);
        proto = p[6];
        l4 = p + WG_IP_HDR6;
        ln = n - WG_IP_HDR6;
    } else
        return 0;
    switch (proto) {
    case WG_PROTO_TCP:
        if (ln < 20)
            return 0;
        break;
    case WG_PROTO_UDP:
        if (ln < WG_UDP_HDR || be16(l4 + 4) != ln || !(l4[6] | l4[7]))
            return 0;
        break;
    case WG_PROTO_ICMP:
        if (s.v6 || ln < 8)
            return 0;
        return !wg_csum_fin(wg_csum_acc(0, l4, ln));
    case WG_PROTO_ICMP6:
        if (!s.v6 || ln < 8)
            return 0;
        break;
    default:
        return 0;
    }
    return !wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&s, &d, proto, ln), l4, ln));
}

wg_udp_t* wg_udp_bind(wg_ip_t* ip, int v6, uint16_t port, wg_udp_rx_fn rx, void* ud)
{
    wg_udp_t* s;
    wg_upage_t** g;
    v6 = !!v6;
    if (!port) {
        unsigned i, span = 65536u - WG_IP_EPH_LO;
        for (i = 0; i < span; i++) {
            uint16_t c = ip->eph[v6];
            ip->eph[v6] = (uint16_t)(c == 65535 ? WG_IP_EPH_LO : c + 1);
            if (!port_get(ip, v6, c)) {
                port = c;
                break;
            }
        }
        if (!port)
            return NULL;
    } else if (port_get(ip, v6, port))
        return NULL;
    g = &ip->pg[v6][port >> 8];
    if (!*g && !(*g = (wg_upage_t*)calloc(1, sizeof **g)))
        return NULL;
    if (!(s = (wg_udp_t*)calloc(1, sizeof *s))) {
        if (!(*g)->n) {
            free(*g);
            *g = NULL;
        }
        return NULL;
    }
    s->ip = ip;
    s->v6 = v6;
    s->port = port;
    s->rx = rx;
    s->ud = ud;
    (*g)->s[port & 255] = s;
    (*g)->n++;
    return s;
}

uint16_t wg_udp_port(const wg_udp_t* s)
{
    return s->port;
}

int wg_udp_sendto(wg_udp_t* s, const wg_ipaddr_t* dst, uint16_t dport, const uint8_t* data, size_t n)
{
    wg_ip_t* ip = s->ip;
    const wg_ipaddr_t* src = wg_ip_src_for(ip, s->v6);
    size_t hl = hdr_len(s->v6);
    uint8_t* u = ip->tx + hl;
    if (!src || n > WG_IP_MTU - hl - WG_UDP_HDR) {
        ip->st.tx_fail++;
        return -1;
    }
    put16(u, s->port);
    put16(u + 2, dport);
    put16(u + 4, (unsigned)(n + WG_UDP_HDR));
    memcpy(u + WG_UDP_HDR, data, n);
    l4_csum(u, n + WG_UDP_HDR, 6, src, dst, WG_PROTO_UDP, 1, 1);
    return emit(ip, src, dst, WG_PROTO_UDP, n + WG_UDP_HDR);
}

void wg_udp_close(wg_udp_t* s)
{
    wg_upage_t** g;
    if (!s)
        return;
    g = &s->ip->pg[s->v6][s->port >> 8];
    (*g)->s[s->port & 255] = NULL;
    if (!--(*g)->n) {
        free(*g);
        *g = NULL;
    }
    free(s);
}
