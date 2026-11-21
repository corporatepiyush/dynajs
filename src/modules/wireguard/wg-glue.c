#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-glue.h"
#include "../../dyna-aio.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/timerfd.h>
#endif

#include "../../core/dyn-common.h"

enum { GLUE_TUN_BATCH = 64 };

struct wg_glue {
    dyn_aio_t* aio;
    wg_dev_t* dev;
    wg_ip_t* ip;
    wg_tcp_t* tcp;
    wg_tun_t* tun;
    int tfd, armed, closed, dying;
    void (*freed)(void*);
    void* freed_ud;
    uint64_t armed_at;
    uint8_t isn_key[16];
    wg_glue_stats_t st;
};

static uint64_t rotl(uint64_t x, int b)
{
    return (x << b) | (x >> (64 - b));
}

static uint64_t le64(const uint8_t* p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

#define SIPROUND                                                         \
    do {                                                                 \
        v0 += v1;                                                        \
        v1 = rotl(v1, 13);                                               \
        v1 ^= v0;                                                        \
        v0 = rotl(v0, 32);                                               \
        v2 += v3;                                                        \
        v3 = rotl(v3, 16);                                               \
        v3 ^= v2;                                                        \
        v0 += v3;                                                        \
        v3 = rotl(v3, 21);                                               \
        v3 ^= v0;                                                        \
        v2 += v1;                                                        \
        v1 = rotl(v1, 17);                                               \
        v1 ^= v2;                                                        \
        v2 = rotl(v2, 32);                                               \
    } while (0)

uint64_t wg_siphash24(const uint8_t key[16], const uint8_t* m, size_t n)
{
    uint64_t k0 = le64(key), k1 = le64(key + 8), b = (uint64_t)n << 56, x;
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1, v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    size_t i, end = n & ~(size_t)7;
    for (i = 0; i < end; i += 8) {
        x = le64(m + i);
        v3 ^= x;
        SIPROUND;
        SIPROUND;
        v0 ^= x;
    }
    for (i = 0; i < (n & 7); i++)
        b |= (uint64_t)m[end + i] << (8 * i);
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

uint32_t wg_isn_rfc6528(const uint8_t key[16], uint64_t now_us, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp)
{
    uint8_t m[37];
    m[0] = (uint8_t)la->v6;
    memcpy(m + 1, la->a, 16);
    memcpy(m + 17, ra->a, 16);
    m[33] = (uint8_t)(lp >> 8);
    m[34] = (uint8_t)lp;
    m[35] = (uint8_t)(rp >> 8);
    m[36] = (uint8_t)rp;
    return (uint32_t)(now_us >> 2) + (uint32_t)wg_siphash24(key, m, sizeof m);
}

static uint64_t mono_ms(void* ud)
{
    struct timespec ts;
    (void)ud;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t glue_isn(void* ud, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp)
{
    wg_glue_t* g = (wg_glue_t*)ud;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g->st.isn_calls++;
    return wg_isn_rfc6528(g->isn_key, (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u, la, lp, ra, rp);
}

static void timer_set(wg_glue_t* g, uint64_t delay_ms)
{
#if defined(__APPLE__)
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, (intptr_t)delay_ms, NULL);
    kevent(g->tfd, &ev, 1, NULL, 0, NULL);
#elif defined(__linux__)
    struct itimerspec it;
    memset(&it, 0, sizeof it);
    if (!delay_ms)
        it.it_value.tv_nsec = 1;
    else {
        it.it_value.tv_sec = (time_t)(delay_ms / 1000u);
        it.it_value.tv_nsec = (long)(delay_ms % 1000u) * 1000000L;
    }
    timerfd_settime(g->tfd, 0, &it, NULL);
#else
    (void)g;
    (void)delay_ms;
#endif
}

static void arm(wg_glue_t* g, int now_too)
{
    uint64_t now = mono_ms(NULL), a = wg_dev_next_deadline(g->dev), b = g->tcp ? wg_tcp_next_deadline(g->tcp) : 0, due;
    if (g->closed)
        return;
    due = a && b ? (a < b ? a : b) : (a ? a : b);
    if (now_too)
        due = now;
    if (!due)
        return;
    if (g->armed && g->armed_at <= due)
        return;
    g->armed = 1;
    g->armed_at = due;
    timer_set(g, due > now ? due - now : 0);
}

static void on_timer(dyn_aio_t* aio, int fd, void* ud)
{
    wg_glue_t* g = (wg_glue_t*)ud;
#if defined(__APPLE__)
    struct kevent ev;
    struct timespec z = { 0, 0 };
    while (kevent(fd, NULL, 0, &ev, 1, &z) > 0)
        ;
#elif defined(__linux__)
    uint64_t v;
    while (read(fd, &v, sizeof v) == (ssize_t)sizeof v)
        ;
#endif
    (void)aio;
    g->armed = 0;
    if (g->dying && wg_dev_pending(g->dev)) {
        g->armed = 1;
        timer_set(g, 1);
        return;
    }
    if (g->dying) {
        void (*fn)(void*) = g->freed;
        void* fud = g->freed_ud;
        wg_glue_free(g);
        if (fn)
            fn(fud);
        return;
    }
    if (g->closed)
        return;
    g->st.timer_fires++;
    wg_dev_tick(g->dev);
    if (g->tcp)
        wg_tcp_tick(g->tcp);
    wg_dev_flush(g->dev);
    arm(g, 0);
}

static int ip_out(void* ud, const uint8_t* p, size_t n)
{
    wg_glue_t* g = (wg_glue_t*)ud;
    if (g->closed)
        return -1;
    if (wg_dev_send(g->dev, p, n) < 0)
        return -1;
    arm(g, 1);
    return 0;
}

static void dev_rx(void* ud, wg_dpeer_t* p, const uint8_t* ip, size_t n, int verified)
{
    wg_glue_t* g = (wg_glue_t*)ud;
    (void)p;
    if (g->closed)
        return;
    if (g->tun) {
        if (wg_tun_write(g->tun, ip, n) == 0)
            g->st.tun_tx++;
        else
            g->st.tun_drop++;
        return;
    }
    if (verified)
        wg_ip_input_trusted(g->ip, ip, n);
    else
        wg_ip_input(g->ip, ip, n);
}

static void tun_pkt(void* ud, const uint8_t* ip, size_t n)
{
    wg_glue_t* g = (wg_glue_t*)ud;
    if (wg_dev_send(g->dev, ip, n) == 0)
        g->st.tun_rx++;
    else
        g->st.tun_drop++;
}

static void on_tun(dyn_aio_t* aio, int fd, void* ud)
{
    wg_glue_t* g = (wg_glue_t*)ud;
    (void)aio;
    (void)fd;
    if (g->closed)
        return;
    wg_tun_read(g->tun, GLUE_TUN_BATCH, tun_pkt, g);
    wg_dev_flush(g->dev);
    arm(g, 0);
}

wg_glue_t* wg_glue_new(struct dyn_aio* aio, const uint8_t priv[WG_KEY], const char* host, uint16_t port, wg_tun_t* tun)
{
    wg_static_t s;
    if (wg_static_init(&s, priv) < 0)
        return NULL;
    return wg_glue_new_static(aio, &s, host, port, tun);
}

wg_glue_t* wg_glue_new_static(struct dyn_aio* aio, wg_static_t* s, const char* host, uint16_t port, wg_tun_t* tun)
{
    wg_glue_t* g = (wg_glue_t*)calloc(1, sizeof *g);
    if (!g) {
        wg_static_free(s);
        return NULL;
    }
    g->aio = aio;
    g->tfd = -1;
    g->tun = tun;
    wg_random(g->isn_key, sizeof g->isn_key);
#if defined(__APPLE__)
    g->tfd = kqueue();
    if (g->tfd >= 0)
        dyn_fd_set_cloexec(g->tfd);
#elif defined(__linux__)
    g->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
#endif
    if (g->tfd < 0) {
        wg_static_free(s);
        goto fail;
    }
    if (!(g->dev = wg_dev_new_static(aio, s, host, port, dev_rx, g)))
        goto fail;
    if (!tun) {
        if (!(g->ip = wg_ip_new(ip_out, g)) || !(g->tcp = wg_tcp_new(g->ip, mono_ms, NULL)))
            goto fail;
        wg_ip_set_clock(g->ip, mono_ms, NULL);
        wg_tcp_set_isn(g->tcp, glue_isn, g);
        wg_dev_set_verify(g->dev, 1);
    } else if (dyn_aio_watch_fd(aio, wg_tun_fd(tun), on_tun, g) < 0)
        goto fail;
    if (dyn_aio_watch_fd(aio, g->tfd, on_timer, g) < 0) {
        if (tun)
            dyn_aio_unwatch_fd(aio, wg_tun_fd(tun));
        goto fail;
    }
    return g;
fail:
    wg_tcp_free(g->tcp);
    wg_ip_free(g->ip);
    wg_dev_free(g->dev);
    if (g->tfd >= 0)
        close(g->tfd);
    free(g);
    return NULL;
}

void wg_glue_close(wg_glue_t* g)
{
    if (!g || g->closed)
        return;
    g->closed = 1;
    if (g->tun)
        dyn_aio_unwatch_fd(g->aio, wg_tun_fd(g->tun));
    wg_dev_close(g->dev);
}

int wg_glue_closed(const wg_glue_t* g)
{
    return g->closed;
}

void wg_glue_release(wg_glue_t* g, void (*freed)(void* ud), void* ud)
{
    if (!g || g->dying)
        return;
    wg_glue_close(g);
    g->dying = 1;
    g->freed = freed;
    g->freed_ud = ud;
    g->armed = 1;
    timer_set(g, 0);
}

void wg_glue_free(wg_glue_t* g)
{
    if (!g)
        return;
    wg_glue_close(g);
    dyn_aio_unwatch_fd(g->aio, g->tfd);
    close(g->tfd);
    wg_tcp_free(g->tcp);
    wg_dev_free(g->dev);
    wg_ip_free(g->ip);
    wg_tun_close(g->tun);
    wg_wipe(g->isn_key, sizeof g->isn_key);
    free(g);
}

wg_dev_t* wg_glue_dev(const wg_glue_t* g)
{
    return g->dev;
}

wg_ip_t* wg_glue_ip(const wg_glue_t* g)
{
    return g->ip;
}

wg_tcp_t* wg_glue_tcp(const wg_glue_t* g)
{
    return g->tcp;
}

wg_tun_t* wg_glue_tun(const wg_glue_t* g)
{
    return g->tun;
}

void wg_glue_kick(wg_glue_t* g)
{
    if (g->closed)
        return;
    wg_dev_flush(g->dev);
    arm(g, 0);
}

void wg_glue_set_isn_key(wg_glue_t* g, const uint8_t key[16])
{
    memcpy(g->isn_key, key, 16);
}

void wg_glue_stats(const wg_glue_t* g, wg_glue_stats_t* st)
{
    *st = g->st;
}

#endif
