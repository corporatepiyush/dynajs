/*
 * test_wg_glue.c -- dyna:wireguard S13e glue (src/modules/wireguard/wg-glue.c): device <-> userspace IP/TCP stack and
 * device <-> tun, one timer fd per device (kqueue EVFILT_TIMER / timerfd) armed to the earlier of the device and TCP
 * deadlines, keyed ISN (RFC 6528 with SipHash-2-4), deferred release. Real UDP on 127.0.0.1, the real aio loop and pool.
 *  1. SipHash-2-4 reference vectors (Aumasson-Bernstein, key 00..0f, messages 00..n-1 for n = 0, 8, 15, 63).
 *  2. ISN = M + F(tuple, key): M ticks every 4 us (RFC 6528 3); F changes with every tuple field, address family, key.
 *  3. Two devices, userspace stacks 10.9.0.1 / 10.9.0.2: TCP connect through the tunnel, 512 KiB echoed, FIN both ways;
 *     ISNs come from the keyed function; inbound packets are checksum-verified on the pool and the stack skips them;
 *     timers fire from the timer fd (no polling).
 *  4. UDP echo through the tunnel, v4.
 *  5. Tun mode over a socketpair (no root): a packet written by the "kernel" side reaches the far stack, the reply comes
 *     back out of the tun fd; verify is off in tun mode.
 *  6. wg_glue_release from inside a TCP data callback: closed at once, freed after the dispatch unwinds (ASan: no UAF).
 *  7. wg_glue_release with pool jobs in flight: the device closes at once (its UDP port is free again), the free waits
 *     (timer fd) until the jobs drained. 64 blocker jobs hold every pool thread for 300 ms, so the X25519 jobs cannot run:
 *     after 100 ms the glue must still be waiting; a free from the first timer fire would spin wg_dev_free's nested aio loop
 *     inside the dispatch until the blockers end.
 *  8. TCP-only deadline: a SYN into a stack that drops it (no local address) is retransmitted by the 1 s RTO while the
 *     device deadline (keepalive, rekey) is over 5 s away; the timer fd must be armed from the TCP deadline too.
 *  9. Seeded properties (tests/lib/dyn-fuzzkit.h; DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales, DYN_FUZZ_ONLY picks):
 *     glue-siphash vs an independent SipHash-2-4; glue-isn: M step and one-field F sensitivity on random tuples;
 *     glue-release: either side released at a random point (in a data callback, mid-transfer, jobs in flight).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../../src/dyna-aio.h"
#include "../../src/modules/wireguard/wg-glue.h"
#include "../lib/dyn-fuzzkit.h"

static int fails, checks;
#define CHECK(c, msg)                                          \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            fails++;                                           \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
        }                                                      \
    } while (0)

static dyn_aio_t* aio;

static double wall_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void run_until(const int* flag, double max_s)
{
    double t0 = wall_s();
    while (!*flag && wall_s() - t0 < max_s)
        dyn_aio_run_us(aio, 2000);
}

static void t_siphash(void)
{
    static const uint64_t want[] = { 0x726fdb47dd0e0e31ULL, 0x93f5f5799a932462ULL, 0xa129ca6149be45e5ULL, 0x958a324ceb064572ULL };
    static const size_t len[] = { 0, 8, 15, 63 };
    uint8_t k[16], m[64];
    unsigned i, bad = 0;
    for (i = 0; i < 16; i++)
        k[i] = (uint8_t)i;
    for (i = 0; i < 64; i++)
        m[i] = (uint8_t)i;
    for (i = 0; i < 4; i++)
        if (wg_siphash24(k, m, len[i]) != want[i]) {
            printf("  siphash len %zu: %016llx\n", len[i], (unsigned long long)wg_siphash24(k, m, len[i]));
            bad++;
        }
    CHECK(!bad, "SipHash-2-4 reference vectors (lengths 0, 8, 15, 63)");
}

static wg_ipaddr_t a4(uint8_t d)
{
    wg_ipaddr_t r;
    memset(&r, 0, sizeof r);
    r.a[0] = 10;
    r.a[1] = 9;
    r.a[3] = d;
    return r;
}

static void t_isn(void)
{
    uint8_t k[16], k2[16];
    wg_ipaddr_t l = a4(1), r = a4(2), l6 = a4(1), r2 = a4(3);
    uint32_t x;
    memset(k, 7, sizeof k);
    memset(k2, 8, sizeof k2);
    l6.v6 = 1;
    x = wg_isn_rfc6528(k, 1000000, &l, 4000, &r, 80);
    CHECK(wg_isn_rfc6528(k, 1000004, &l, 4000, &r, 80) == x + 1 && wg_isn_rfc6528(k, 2000000, &l, 4000, &r, 80) == x + 250000, "M: +1 every 4 us (250000 per second)");
    CHECK(wg_isn_rfc6528(k, 1000000, &l, 4001, &r, 80) != x && wg_isn_rfc6528(k, 1000000, &l, 4000, &r, 81) != x && wg_isn_rfc6528(k, 1000000, &l, 4000, &r2, 80) != x &&
              wg_isn_rfc6528(k, 1000000, &r, 4000, &r, 80) != x,
          "F changes with every port and address");
    CHECK(wg_isn_rfc6528(k, 1000000, &l6, 4000, &r, 80) != x && wg_isn_rfc6528(k2, 1000000, &l, 4000, &r, 80) != x, "F changes with the address family and the key");
}

typedef struct {
    wg_glue_t* g;
    uint8_t pub[32];
    uint16_t port;
} gnode_t;

static void gnode(gnode_t* n, wg_tun_t* tun)
{
    uint8_t priv[32];
    wg_keygen(priv, n->pub);
    n->g = wg_glue_new(aio, priv, "127.0.0.1", 0, tun);
    n->port = n->g ? (uint16_t)wg_dev_port(wg_glue_dev(n->g)) : 0;
}

static void ready_wait(wg_dpeer_t* p)
{
    int k;
    for (k = 0; k < 2000 && !wg_dev_peer_ready(p); k++)
        dyn_aio_run_us(aio, 1000);
}

static wg_dpeer_t* plink(gnode_t* a, gnode_t* b, uint8_t b_ip, int endpoint)
{
    struct sockaddr_in s;
    wg_ipaddr_t x = a4(b_ip);
    wg_dpeer_t* p = wg_dev_add_peer(wg_glue_dev(a->g), b->pub, NULL, 0, NULL, NULL);
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s.sin_port = htons(b->port);
    if (endpoint)
        wg_dev_set_endpoint(p, (struct sockaddr*)&s, sizeof s);
    wg_dev_allow(wg_glue_dev(a->g), p, 0, x.a, 32);
    ready_wait(p);
    return p;
}

static void addr(gnode_t* n, uint8_t d)
{
    wg_ipaddr_t x = a4(d);
    wg_ip_add_addr(wg_glue_ip(n->g), &x);
}

enum { BULK = 512 * 1024 };
static uint8_t tx_buf[BULK];
static size_t cl_sent, cl_got, sv_got;
static int cl_closed, sv_eofd, release_in_cb, released;
static wg_glue_t* release_target;
static int freed_flag;
static void on_freed(void* ud)
{
    *(int*)ud = 1;
}

static void cl_pump(void* ud, wg_tconn_t* c)
{
    (void)ud;
    while (cl_sent < BULK) {
        size_t w = wg_tcp_write(c, tx_buf + cl_sent, BULK - cl_sent);
        if (!w)
            break;
        cl_sent += w;
    }
    if (cl_sent == BULK)
        wg_tcp_shutdown(c);
}

static void cl_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    (void)ud;
    (void)c;
    if (memcmp(d, tx_buf + cl_got, n))
        fails += 1000;
    cl_got += n;
}

static void cl_close(void* ud, wg_tconn_t* c, int err)
{
    (void)ud;
    (void)c;
    (void)err;
    cl_closed = 1;
}

static void sv_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    (void)ud;
    sv_got += n;
    if (release_in_cb && release_target) {
        wg_glue_release(release_target, on_freed, &freed_flag);
        release_target = NULL;
        released = 1;
        return;
    }
    wg_tcp_write(c, d, n);
}

static void sv_eof(void* ud, wg_tconn_t* c)
{
    (void)ud;
    sv_eofd = 1;
    wg_tcp_shutdown(c);
}

static const wg_tcp_cb_t CL = { cl_pump, cl_data, NULL, cl_pump, cl_close };
static const wg_tcp_cb_t SV = { NULL, sv_data, sv_eof, NULL, NULL };

static int udp_echoed;
static void on_udp(void* ud, wg_udp_t* s, const wg_ipaddr_t* src, uint16_t sport, const uint8_t* d, size_t n)
{
    int echo = (int)(size_t)ud;
    if (echo)
        wg_udp_sendto(s, src, sport, d, n);
    else if (n == 5 && !memcmp(d, "hello", 5))
        udp_echoed = 1;
}

static void t_tunnel(void)
{
    gnode_t A, B;
    wg_ipaddr_t dst = a4(2);
    wg_tconn_t* c;
    wg_tcp_info_t inf;
    wg_glue_stats_t ga, gb;
    wg_dev_stats_t db;
    wg_ip_stats_t ib;
    wg_udp_t *ua, *ub;
    size_t i;
    uint8_t key[16];
    gnode(&A, NULL);
    gnode(&B, NULL);
    CHECK(A.g && B.g && A.port && B.port, "two glued devices with userspace stacks");
    addr(&A, 1);
    addr(&B, 2);
    plink(&A, &B, 2, 1);
    plink(&B, &A, 1, 0);
    memset(key, 0x42, sizeof key);
    wg_glue_set_isn_key(A.g, key);
    for (i = 0; i < BULK; i++)
        tx_buf[i] = (uint8_t)(i * 2654435761u >> 13);
    wg_tcp_listen(wg_glue_tcp(B.g), 0, 80, 16, &SV, NULL);
    c = wg_tcp_connect(wg_glue_tcp(A.g), &dst, 80, &CL, NULL);
    wg_glue_kick(A.g);
    {
        wg_ipaddr_t la, ra;
        uint16_t lp, rp;
        struct timespec ts;
        uint64_t us;
        uint32_t lo, hi;
        wg_tcp_endpoints(c, &la, &lp, &ra, &rp);
        wg_tcp_info(c, &inf);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
        hi = wg_isn_rfc6528(key, us, &la, lp, &ra, rp);
        lo = wg_isn_rfc6528(key, us - 2000000, &la, lp, &ra, rp);
        CHECK(inf.iss - lo <= hi - lo, "connect ISN comes from the keyed RFC 6528 function (within the last 2 s of M)");
    }
    run_until(&cl_closed, 20);
    wg_glue_stats(A.g, &ga);
    wg_glue_stats(B.g, &gb);
    wg_dev_stats(wg_glue_dev(B.g), &db);
    wg_ip_stats(wg_glue_ip(B.g), &ib);
    CHECK(cl_sent == BULK && sv_got == BULK && cl_got == BULK && sv_eofd && cl_closed, "512 KiB echoed through the tunnel over TCP, FIN both ways");
    CHECK(ga.isn_calls >= 1 && gb.isn_calls >= 1, "both ISNs (active and passive) from the keyed function");
    CHECK(db.csum_verified > 100 && ib.csum_skipped == db.csum_verified, "inbound segments checksum-verified on the pool; the stack skipped exactly those");
    CHECK(ga.timer_fires > 0 && gb.timer_fires > 0, "timers driven by the timer fd");
    ub = wg_udp_bind(wg_glue_ip(B.g), 0, 53, on_udp, (void*)1);
    ua = wg_udp_bind(wg_glue_ip(A.g), 0, 0, on_udp, (void*)0);
    wg_udp_sendto(ua, &dst, 53, (const uint8_t*)"hello", 5);
    wg_glue_kick(A.g);
    run_until(&udp_echoed, 5);
    CHECK(udp_echoed, "UDP echo through the tunnel");
    (void)ub;
    wg_glue_free(A.g);
    wg_glue_free(B.g);
}

static int fcntl_closed(int fd)
{
    return fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

static void t_tun_mode(void)
{
    gnode_t B, C;
    int sp[2], got = 0, k;
    uint8_t p[64], r[128];
    wg_ipaddr_t s = a4(3), d = a4(2);
    wg_glue_stats_t gc;
    wg_dev_stats_t dc;
    wg_tun_t* tun;
    uint16_t c;
    ssize_t n = -1;
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
    tun = wg_tun_from_fd(sp[0], WG_TUN_FRAME_NONE, "wg9", 1420, NULL);
    gnode(&C, tun);
    gnode(&B, NULL);
    CHECK(C.g && B.g && !wg_glue_ip(C.g), "tun-mode device has no userspace stack");
    addr(&B, 2);
    plink(&C, &B, 2, 1);
    plink(&B, &C, 3, 0);
    wg_udp_bind(wg_glue_ip(B.g), 0, 53, on_udp, (void*)1);
    memset(p, 0, sizeof p);
    p[0] = 0x45;
    p[3] = 33;
    p[8] = 64;
    p[9] = 17;
    memcpy(p + 12, s.a, 4);
    memcpy(p + 16, d.a, 4);
    c = wg_csum_fin(wg_csum_acc(0, p, 20));
    memcpy(p + 10, &c, 2);
    p[20] = 0x30;
    p[21] = 0x39;
    p[23] = 53;
    p[25] = 13;
    memcpy(p + 28, "hello", 5);
    c = wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&s, &d, 17, 13), p + 20, 13));
    memcpy(p + 26, &c, 2);
    send(sp[1], p, 33, 0);
    for (k = 0; k < 3000 && !got; k++) {
        dyn_aio_run_us(aio, 1000);
        n = recv(sp[1], r, sizeof r, MSG_DONTWAIT);
        got = n > 0;
    }
    wg_glue_stats(C.g, &gc);
    wg_dev_stats(wg_glue_dev(C.g), &dc);
    CHECK(got && n == 33 && !memcmp(r + 12, d.a, 4) && !memcmp(r + 16, s.a, 4) && !memcmp(r + 28, "hello", 5), "tun -> tunnel -> far stack echo -> tunnel -> tun");
    CHECK(gc.tun_rx == 1 && gc.tun_tx == 1 && dc.csum_verified == 0, "tun counters; no pool verify in tun mode (the kernel checks)");
    wg_glue_free(C.g);
    CHECK(fcntl_closed(sp[0]), "glue free closes the tun fd");
    close(sp[1]);
    wg_glue_free(B.g);
}

static void t_release_in_cb(void)
{
    gnode_t A, B;
    wg_ipaddr_t dst = a4(2);
    int k;
    gnode(&A, NULL);
    gnode(&B, NULL);
    addr(&A, 1);
    addr(&B, 2);
    plink(&A, &B, 2, 1);
    plink(&B, &A, 1, 0);
    cl_sent = cl_got = sv_got = 0;
    cl_closed = sv_eofd = 0;
    release_in_cb = 1;
    release_target = B.g;
    wg_tcp_listen(wg_glue_tcp(B.g), 0, 80, 16, &SV, NULL);
    wg_tcp_connect(wg_glue_tcp(A.g), &dst, 80, &CL, NULL);
    wg_glue_kick(A.g);
    run_until(&released, 10);
    CHECK(released && wg_glue_closed(B.g), "release from inside a data callback: closed at once");
    run_until(&freed_flag, 5);
    for (k = 0; k < 50; k++)
        dyn_aio_run_us(aio, 1000);
    CHECK(freed_flag, "freed from the timer fd after the dispatch unwound, freed callback ran (ASan: no use after free)");
    release_in_cb = 0;
    wg_glue_free(A.g);
}

static int udp_port_free(uint16_t port)
{
    struct sockaddr_in s;
    int fd = socket(AF_INET, SOCK_DGRAM, 0), ok;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s.sin_port = htons(port);
    ok = fd >= 0 && bind(fd, (struct sockaddr*)&s, sizeof s) == 0;
    if (fd >= 0)
        close(fd);
    return ok;
}

static double block_until;
static int blockers_done;
static void block_work(void* a)
{
    (void)a;
    while (wall_s() < block_until)
        ;
}

static void block_done(void* a)
{
    (void)a;
    blockers_done++;
}

static void t_release_jobs(void)
{
    gnode_t A;
    uint8_t priv[32], pub[32];
    int i;
    double t0;
    gnode(&A, NULL);
    CHECK(!udp_port_free(A.port), "device holds its UDP port");
    block_until = wall_s() + 0.3;
    for (i = 0; i < 64; i++)
        if (dyn_aio_offload_cpu(aio, block_work, block_done, NULL) < 0)
            blockers_done++;
    for (i = 0; i < 64; i++) {
        wg_keygen(priv, pub);
        wg_dev_add_peer(wg_glue_dev(A.g), pub, NULL, 0, NULL, NULL);
    }
    CHECK(wg_dev_pending(wg_glue_dev(A.g)) > 0, "release with add_peer jobs in flight");
    freed_flag = 0;
    wg_glue_release(A.g, on_freed, &freed_flag);
    CHECK(wg_glue_closed(A.g) && udp_port_free(A.port), "closed at once: the UDP port is free again before the free");
    t0 = wall_s();
    while (wall_s() - t0 < 0.1)
        dyn_aio_run_us(aio, 1000);
    CHECK(!freed_flag && wall_s() < block_until, "100 ms of timer fires with the jobs still queued behind the blockers: the free waits");
    run_until(&freed_flag, 5);
    CHECK(freed_flag, "freed once the pool jobs drained, never by a nested loop inside a dispatch");
    while (blockers_done < 64)
        dyn_aio_run_us(aio, 1000);
}

static void t_tcp_deadline(void)
{
    gnode_t A, B;
    wg_ipaddr_t dst = a4(2);
    wg_tconn_t* c;
    wg_tcp_stats_t ts;
    double t0;
    gnode(&A, NULL);
    gnode(&B, NULL);
    addr(&A, 1);
    plink(&A, &B, 2, 1);
    plink(&B, &A, 1, 0);
    c = wg_tcp_connect(wg_glue_tcp(A.g), &dst, 80, &CL, NULL);
    wg_glue_kick(A.g);
    t0 = wall_s();
    do {
        dyn_aio_run_us(aio, 5000);
        wg_tcp_stats(wg_glue_tcp(A.g), &ts);
    } while (!ts.rto_fired && wall_s() - t0 < 2.5);
    CHECK(c && ts.rto_fired >= 1 && wg_dev_next_deadline(wg_glue_dev(A.g)) > (uint64_t)(wall_s() * 1000) + 5000, "SYN retransmitted by the RTO from the timer fd while the device deadline is over 5 s away");
    wg_glue_free(A.g);
    wg_glue_free(B.g);
}

static uint64_t n_rotl(uint64_t x, int b)
{
    return x << b | x >> (64 - b);
}

static uint64_t n_le64(const uint8_t* p, size_t n)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t n_siphash(const uint8_t k[16], const uint8_t* m, size_t n)
{
    uint64_t k0 = n_le64(k, 8), k1 = n_le64(k + 8, 8), v[4], b;
    size_t i;
    int r;
    v[0] = k0 ^ 0x736f6d6570736575ULL;
    v[1] = k1 ^ 0x646f72616e646f6dULL;
    v[2] = k0 ^ 0x6c7967656e657261ULL;
    v[3] = k1 ^ 0x7465646279746573ULL;
    for (i = 0; i <= n / 8; i++) {
        size_t left = n - i * 8;
        b = i < n / 8 ? n_le64(m + i * 8, 8) : n_le64(m + i * 8, left) | (uint64_t)(n & 0xff) << 56;
        v[3] ^= b;
        for (r = 0; r < 2; r++) {
            v[0] += v[1], v[1] = n_rotl(v[1], 13), v[1] ^= v[0], v[0] = n_rotl(v[0], 32);
            v[2] += v[3], v[3] = n_rotl(v[3], 16), v[3] ^= v[2];
            v[0] += v[3], v[3] = n_rotl(v[3], 21), v[3] ^= v[0];
            v[2] += v[1], v[1] = n_rotl(v[1], 17), v[1] ^= v[2], v[2] = n_rotl(v[2], 32);
        }
        v[0] ^= b;
    }
    v[2] ^= 0xff;
    for (r = 0; r < 4; r++) {
        v[0] += v[1], v[1] = n_rotl(v[1], 13), v[1] ^= v[0], v[0] = n_rotl(v[0], 32);
        v[2] += v[3], v[3] = n_rotl(v[3], 16), v[3] ^= v[2];
        v[0] += v[3], v[3] = n_rotl(v[3], 21), v[3] ^= v[0];
        v[2] += v[1], v[1] = n_rotl(v[1], 17), v[1] ^= v[2], v[2] = n_rotl(v[2], 32);
    }
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}

static void p_glue_siphash(void)
{
    fk_rng_t r;
    uint8_t k[16], m[300];
    unsigned i, n, bad = 0;
    if (!fk_only("glue-siphash"))
        return;
    r = fk_stream("glue-siphash");
    n = (unsigned)fk_iters(40000);
    for (i = 0; i < n; i++) {
        size_t len = fk_size(&r, sizeof m);
        fk_bytes(&r, k, sizeof k);
        fk_bytes(&r, m, len);
        bad += wg_siphash24(k, m, len) != n_siphash(k, m, len);
    }
    CHECK(bad == 0, "glue-siphash: random keys and 0..300-byte messages equal an independent SipHash-2-4");
}

static wg_ipaddr_t rnd_ip(fk_rng_t* r, int v6)
{
    wg_ipaddr_t a;
    memset(&a, 0, sizeof a);
    a.v6 = v6;
    fk_bytes(r, a.a, v6 ? 16 : 4);
    return a;
}

static void p_glue_isn(void)
{
    fk_rng_t r;
    unsigned i, n, mbad = 0, coll = 0;
    if (!fk_only("glue-isn"))
        return;
    r = fk_stream("glue-isn");
    n = (unsigned)fk_iters(20000);
    for (i = 0; i < n; i++) {
        uint8_t k[16], k2[16];
        int v6 = fk_chance(&r, 40);
        wg_ipaddr_t la = rnd_ip(&r, v6), ra = rnd_ip(&r, v6), lb = la, rb = ra;
        uint16_t lp = (uint16_t)fk_next(&r), rp = (uint16_t)fk_next(&r), lp2 = lp, rp2 = rp;
        uint64_t t = fk_next(&r) >> (fk_below(&r, 40) + 1), d = fk_chance(&r, 50) ? fk_below(&r, 64) : fk_next(&r) >> 20;
        uint32_t x, y;
        fk_bytes(&r, k, 16);
        x = wg_isn_rfc6528(k, t, &la, lp, &ra, rp);
        memcpy(k2, k, 16);
        mbad += wg_isn_rfc6528(k, t + d, &la, lp, &ra, rp) - x != (uint32_t)((t + d) / 4 - t / 4);
        switch (fk_below(&r, 6)) {
        case 0:
            lb.a[fk_below(&r, v6 ? 16 : 4)] ^= (uint8_t)(1 + fk_below(&r, 255));
            break;
        case 1:
            rb.a[fk_below(&r, v6 ? 16 : 4)] ^= (uint8_t)(1 + fk_below(&r, 255));
            break;
        case 2:
            lp2 = (uint16_t)(lp + 1 + fk_below(&r, 65535));
            break;
        case 3:
            rp2 = (uint16_t)(rp + 1 + fk_below(&r, 65535));
            break;
        case 4:
            k2[fk_below(&r, 16)] ^= (uint8_t)(1u << fk_below(&r, 8));
            break;
        default:
            lb.v6 = rb.v6 = !v6;
            break;
        }
        y = wg_isn_rfc6528(k2, t, &lb, lp2, &rb, rp2);
        coll += y == x;
    }
    CHECK(mbad == 0, "glue-isn: ISN(t+d) - ISN(t) == floor((t+d)/4) - floor(t/4) for random tuples, keys and times (M ticks every 4 us, F is time-free)");
    CHECK(coll <= 2, "glue-isn: changing any one tuple field, the family or one key bit changes F (at most 2 collisions)");
}

static int freed_count;
static void on_freed_n(void* ud)
{
    (void)ud;
    freed_count++;
}

static void p_glue_release(void)
{
    fk_rng_t r;
    unsigned i, n, bad = 0;
    if (!fk_only("glue-release"))
        return;
    r = fk_stream("glue-release");
    n = (unsigned)fk_iters(6);
    for (i = 0; i < n; i++) {
        gnode_t A, B;
        wg_ipaddr_t dst = a4(2);
        unsigned how = fk_below(&r, 3), jobs = fk_below(&r, 17), j;
        int which = fk_chance(&r, 50), k;
        gnode_t* victim;
        double t0;
        gnode(&A, NULL);
        gnode(&B, NULL);
        if (!A.g || !B.g) {
            bad++;
            continue;
        }
        addr(&A, 1);
        addr(&B, 2);
        plink(&A, &B, 2, 1);
        plink(&B, &A, 1, 0);
        victim = which ? &B : &A;
        cl_sent = cl_got = sv_got = 0;
        cl_closed = sv_eofd = released = 0;
        freed_count = 0;
        freed_flag = 0;
        release_in_cb = how == 0 && which;
        release_target = release_in_cb ? B.g : NULL;
        wg_tcp_listen(wg_glue_tcp(B.g), 0, 80, 16, &SV, NULL);
        wg_tcp_connect(wg_glue_tcp(A.g), &dst, 80, &CL, NULL);
        wg_glue_kick(A.g);
        if (release_in_cb) {
            run_until(&released, 10);
            if (!released) {
                bad++;
                release_in_cb = 0;
                wg_glue_free(A.g);
                wg_glue_free(B.g);
                continue;
            }
        } else {
            t0 = wall_s();
            while (wall_s() - t0 < (double)fk_below(&r, 60) / 1000.0)
                dyn_aio_run_us(aio, 1000);
            if (how == 2)
                for (j = 0; j < jobs; j++) {
                    uint8_t priv[32], pub[32];
                    wg_keygen(priv, pub);
                    wg_dev_add_peer(wg_glue_dev(victim->g), pub, NULL, 0, NULL, NULL);
                }
            wg_glue_release(victim->g, on_freed_n, NULL);
        }
        bad += !wg_glue_closed(victim->g);
        bad += !udp_port_free(victim->port);
        t0 = wall_s();
        while ((release_in_cb ? !freed_flag : !freed_count) && wall_s() - t0 < 5)
            dyn_aio_run_us(aio, 1000);
        for (k = 0; k < 30; k++)
            dyn_aio_run_us(aio, 1000);
        bad += release_in_cb ? !freed_flag : freed_count != 1;
        release_in_cb = 0;
        release_target = NULL;
        wg_glue_free(which ? A.g : B.g);
    }
    CHECK(bad == 0, "glue-release: release of either side at a random moment (inside a data callback, mid-transfer, with 0..16 add_peer jobs in flight) closes at once, frees the UDP port, and frees exactly once");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    aio = dyn_aio_new(256, 0);
    fk_init("test_wg_glue");
    if (fk_legacy()) {
        t_siphash();
        t_isn();
        t_tunnel();
        t_tun_mode();
        t_release_in_cb();
        t_release_jobs();
        t_tcp_deadline();
    }
    p_glue_siphash();
    p_glue_isn();
    p_glue_release();
    dyn_aio_free(aio);
    printf("test_wg_glue: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails ? 1 : 0;
}
