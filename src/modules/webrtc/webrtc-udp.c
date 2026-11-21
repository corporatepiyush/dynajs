#include "webrtc-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "../../dyna-aio.h"
#include "../../dyna-udp.h"

static void wrt_addr_from_sa(rtc_addr* a, const struct sockaddr* sa)
{
    memset(a, 0, sizeof(*a));
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* s4 = (const struct sockaddr_in*)(const void*)sa;
        a->fam = 4;
        a->port = ntohs(s4->sin_port);
        memcpy(a->ip, &s4->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* s6 = (const struct sockaddr_in6*)(const void*)sa;
        static const uint8_t v4m[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (!memcmp(&s6->sin6_addr, v4m, 12)) {
            a->fam = 4;
            memcpy(a->ip, (const uint8_t*)&s6->sin6_addr + 12, 4);
        } else {
            a->fam = 6;
            memcpy(a->ip, &s6->sin6_addr, 16);
        }
        a->port = ntohs(s6->sin6_port);
    }
}

static socklen_t wrt_sa_from_addr(struct sockaddr_storage* ss, const rtc_addr* a, int sock_fam)
{
    memset(ss, 0, sizeof(*ss));
    if (a->fam == 4 && sock_fam == 4) {
        struct sockaddr_in* s4 = (struct sockaddr_in*)ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(a->port);
        memcpy(&s4->sin_addr, a->ip, 4);
        return sizeof(*s4);
    } else {
        struct sockaddr_in6* s6 = (struct sockaddr_in6*)ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(a->port);
        if (a->fam == 4) {
            uint8_t* d = (uint8_t*)&s6->sin6_addr;
            d[10] = 0xff;
            d[11] = 0xff;
            memcpy(d + 12, a->ip, 4);
        } else
            memcpy(&s6->sin6_addr, a->ip, 16);
        return sizeof(*s6);
    }
}

#define WRT_UDP_NBUF 32u
#define WRT_UDP_BUFSZ 2048u

struct wrt_udpx {
    dyn_udpb_batch_t* b;
    wrt_udp_rx_fn rx;
    wrt_udp_end_fn end;
    void* ud;
    int fam;
    uint32_t held;
    uint32_t nfree;
    uint8_t dead;
    uint8_t closing;
    uint8_t* free[WRT_UDP_NBUF];
    uint8_t mem[WRT_UDP_NBUF][WRT_UDP_BUFSZ];
};

static void wrt_udpx_maybe_free(struct wrt_udpx* x)
{
    if (x->dead && !x->closing && x->held == 0)
        free(x);
}

static unsigned wrt_udpx_take(void* ud, dyn_udpb_msg_t* m, unsigned n)
{
    struct wrt_udpx* x = (struct wrt_udpx*)ud;
    unsigned k = 0;
    while (k < n && x->nfree) {
        m[k].buf = x->free[--x->nfree];
        m[k].cap = WRT_UDP_BUFSZ;
        k++;
    }
    x->held += k;
    return k;
}

static void wrt_udpx_put(struct wrt_udpx* x, dyn_udpb_msg_t* m, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        x->free[x->nfree++] = m[i].buf;
    x->held -= n;
}

static void wrt_udpx_batch(void* ud, dyn_udpb_msg_t* m, unsigned n)
{
    struct wrt_udpx* x = (struct wrt_udpx*)ud;
    unsigned i;
    for (i = 0; i < n && !x->dead; i++) {
        rtc_addr from;
        if (m[i].truncated || !m[i].len)
            continue;
        wrt_addr_from_sa(&from, (const struct sockaddr*)&m[i].addr);
        x->rx(x->ud, m[i].buf, m[i].len, &from);
    }
    wrt_udpx_put(x, m, n);
    wrt_udpx_maybe_free(x);
}

static void wrt_udpx_release(void* ud, dyn_udpb_msg_t* m, unsigned n)
{
    struct wrt_udpx* x = (struct wrt_udpx*)ud;
    wrt_udpx_put(x, m, n);
    wrt_udpx_maybe_free(x);
}

static void wrt_udpx_batch_end(void* ud)
{
    struct wrt_udpx* x = (struct wrt_udpx*)ud;
    if (!x->dead && x->end)
        x->end(x->ud);
}

static void wrt_udpx_error(void* ud, int err)
{
    (void)ud;
    (void)err;
}

static const dyn_udpb_ops_t wrt_udpx_ops = { wrt_udpx_take, wrt_udpx_batch, wrt_udpx_release, wrt_udpx_error, wrt_udpx_batch_end };

int wrt_udp_open(wrt_udp* u, struct dyn_aio* aio, const rtc_addr* bind_addr, wrt_udp_rx_fn rx, wrt_udp_end_fn end, void* ud)
{
    struct sockaddr_storage ss;
    socklen_t sl;
    rtc_addr bound;
    dyn_udpb_opts_t o;
    struct wrt_udpx* x;
    uint32_t i;
    u->x = NULL;
    x = (struct wrt_udpx*)malloc(sizeof(*x));
    if (!x)
        return -1;
    x->rx = rx;
    x->end = end;
    x->ud = ud;
    x->fam = bind_addr->fam;
    x->held = 0;
    x->dead = 0;
    x->closing = 0;
    x->nfree = WRT_UDP_NBUF;
    for (i = 0; i < WRT_UDP_NBUF; i++)
        x->free[i] = x->mem[i];
    sl = wrt_sa_from_addr(&ss, bind_addr, bind_addr->fam);
    memset(&o, 0, sizeof(o));
    o.flags = bind_addr->fam == 6 ? DYN_UDPB_V6ONLY : 0;
    o.rcvbuf = 4 << 20;
    o.sndbuf = 4 << 20;
    x->b = dyn_udpb_open(aio, (const struct sockaddr*)&ss, sl, &o, &wrt_udpx_ops, x);
    if (!x->b) {
        free(x);
        return -1;
    }
    sl = sizeof(ss);
    if (dyn_udpb_local(x->b, &ss, &sl) < 0) {
        x->dead = 1;
        x->closing = 1;
        dyn_udpb_close(x->b);
        x->closing = 0;
        wrt_udpx_maybe_free(x);
        return -1;
    }
    wrt_addr_from_sa(&bound, (struct sockaddr*)&ss);
    u->x = x;
    u->local = *bind_addr;
    u->local.port = bound.port;
    return 0;
}

int wrt_udp_send(wrt_udp* u, const rtc_addr* to, const uint8_t* p, size_t n)
{
    dyn_udpb_msg_t m;
    int err = 0;
    if (!u->x || (u->local.fam == 4 && to->fam != 4) || n > 65507)
        return -1;
    m.buf = (uint8_t*)(uintptr_t)p;
    m.cap = (uint32_t)n;
    m.len = (uint32_t)n;
    m.gso = 0;
    m.truncated = 0;
    m.reserved = 0;
    m.addr_len = wrt_sa_from_addr(&m.addr, to, u->local.fam);
    return dyn_udpb_send(u->x->b, &m, 1, &err) == 1 ? 0 : -1;
}

void wrt_udp_close(wrt_udp* u)
{
    struct wrt_udpx* x = u->x;
    if (!x)
        return;
    u->x = NULL;
    x->dead = 1;
    x->closing = 1;
    dyn_udpb_close(x->b);
    x->closing = 0;
    wrt_udpx_maybe_free(x);
}

int wrt_collect_addrs(rtc_addr* out, int cap)
{
    struct ifaddrs *ifa = NULL, *it;
    int n = 0, i;
    if (getifaddrs(&ifa) == 0) {
        for (it = ifa; it; it = it->ifa_next) {
            rtc_addr a;
            int dup = 0;
            if (!it->ifa_addr || !(it->ifa_flags & IFF_UP))
                continue;
            if (it->ifa_addr->sa_family != AF_INET && it->ifa_addr->sa_family != AF_INET6)
                continue;
            wrt_addr_from_sa(&a, it->ifa_addr);
            a.port = 0;
            if (!rtc_addr_host_ok(&a))
                continue;
            for (i = 0; i < n; i++)
                if (rtc_addr_ip_eq(&out[i], &a))
                    dup = 1;
            if (!dup && n < cap)
                out[n++] = a;
        }
        freeifaddrs(ifa);
    }
    return n;
}

int wrt_parse_server(const char* url, wrt_server* sv, const char** err)
{
    const char* p = url;
    const char *host, *he;
    char hbuf[256];
    char pbuf[8];
    unsigned long port;
    int secure = 0;
    struct addrinfo hints, *res = NULL;
    size_t hl;
    if (!strncmp(p, "stun:", 5)) {
        sv->turn = 0;
        p += 5;
    } else if (!strncmp(p, "turn:", 5)) {
        sv->turn = 1;
        p += 5;
    } else if (!strncmp(p, "stuns:", 6)) {
        sv->turn = 0;
        secure = 1;
        p += 6;
    } else if (!strncmp(p, "turns:", 6)) {
        sv->turn = 1;
        secure = 1;
        p += 6;
    } else {
        *err = "SyntaxError";
        return -1;
    }
    if (strpbrk(p, "/@#") || (!sv->turn && strchr(p, '?'))) {
        *err = "SyntaxError";
        return -1;
    }
    if (secure) {
        *err = "NotSupportedError";
        return -1;
    }
    if (*p == '[') {
        host = p + 1;
        he = strchr(host, ']');
        if (!he) {
            *err = "SyntaxError";
            return -1;
        }
        p = he + 1;
    } else {
        host = p;
        while (*p && *p != ':' && *p != '?')
            p++;
        he = p;
    }
    hl = (size_t)(he - host);
    if (!hl || hl >= sizeof(hbuf)) {
        *err = "SyntaxError";
        return -1;
    }
    memcpy(hbuf, host, hl);
    hbuf[hl] = 0;
    port = 3478;
    if (*p == ':') {
        char* e;
        port = strtoul(p + 1, &e, 10);
        if (e == p + 1 || port == 0 || port > 65535) {
            *err = "SyntaxError";
            return -1;
        }
        p = e;
    }
    if (*p == '?') {
        if (!sv->turn || (strcmp(p, "?transport=udp") && strcmp(p, "?transport=tcp"))) {
            *err = "SyntaxError";
            return -1;
        }
        sv->tcp = !strcmp(p, "?transport=tcp");
    } else if (*p) {
        *err = "SyntaxError";
        return -1;
    }
    if (rtc_addr_parse(hbuf, hl, (uint16_t)port, &sv->addr) < 0) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        {
            unsigned long v = port;
            int k = 0;
            char tmp[8];
            do {
                tmp[k++] = (char)('0' + v % 10);
                v /= 10;
            } while (v && k < 7);
            for (hl = 0; hl < (size_t)k; hl++)
                pbuf[hl] = tmp[k - 1 - (int)hl];
            pbuf[k] = 0;
        }
        if (getaddrinfo(hbuf, pbuf, &hints, &res) != 0 || !res) {
            if (res)
                freeaddrinfo(res);
            sv->unresolved = 1;
            memset(&sv->addr, 0, sizeof(sv->addr));
            return 0;
        }
        wrt_addr_from_sa(&sv->addr, res->ai_addr);
        freeaddrinfo(res);
    }
    return 0;
}

#endif
