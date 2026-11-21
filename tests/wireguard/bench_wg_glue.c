/*
 * bench_wg_glue.c -- dyna:wireguard S13e device+stack glue (src/modules/wireguard/wg-glue.c), release build, macOS/Linux.
 * Both devices share one loop thread (so "main-thread" CPU covers both ends); AEAD and checksums run on the pool.
 *   isn    : ns per RFC 6528 ISN (SipHash-2-4 over the 4-tuple + clock), the cost added to every SYN / SYN-ACK.
 *   verify : ns per 1400 B UDP packet in wg_ip_input (checksum on the loop thread) vs wg_ip_input_trusted (verified on
 *            the pool by the decrypt batch), and the pool-side wg_ip_verify cost that moved off the loop.
 *   tcp    : 32 MiB A->B over N = 1..256 concurrent tunnelled TCP connections: MB/s, loop-thread CPU us per MiB,
 *            RSS growth per connection after setup and at the end, then 3 repeats at N = 64 (plateau check).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "dyna-aio.h"
#include "modules/wireguard/wg-glue.h"

static double tcpu(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
static double twall(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
static double rss_mb(void)
{
#ifdef __APPLE__
    mach_task_basic_info_data_t i;
    mach_msg_type_number_t c = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &c) == KERN_SUCCESS ? i.resident_size / 1048576.0 : 0;
#else
    long a = 0, b = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &a, &b) != 2)
            b = 0;
        fclose(f);
    }
    return b * 4096.0 / 1048576.0;
#endif
}

static volatile uint64_t sink;
static dyn_aio_t* aio;

static wg_ipaddr_t a4(uint8_t d)
{
    wg_ipaddr_t r;
    memset(&r, 0, sizeof r);
    r.a[0] = 10;
    r.a[1] = 9;
    r.a[3] = d;
    return r;
}

static void bench_isn(void)
{
    uint8_t k[16];
    wg_ipaddr_t l = a4(1), r = a4(2);
    int i, N = 2000000;
    double t0;
    uint32_t acc = 0;
    memset(k, 3, sizeof k);
    t0 = twall();
    for (i = 0; i < N; i++)
        acc += wg_isn_rfc6528(k, (uint64_t)i * 7, &l, (uint16_t)(40000 + (i & 1023)), &r, 80);
    sink += acc;
    printf("isn: %.1f ns per RFC 6528 ISN (SipHash-2-4, 37-byte tuple)\n", (twall() - t0) / N);
}

static int out_drop(void* ud, const uint8_t* p, size_t n)
{
    (void)ud;
    sink += n + p[0];
    return 0;
}

static void on_udp(void* ud, wg_udp_t* s, const wg_ipaddr_t* src, uint16_t sport, const uint8_t* d, size_t n)
{
    (void)ud;
    (void)s;
    (void)src;
    (void)sport;
    sink += n + d[0];
}

static void bench_verify(void)
{
    wg_ip_t* ip = wg_ip_new(out_drop, NULL);
    wg_ipaddr_t s = a4(2), d = a4(1);
    uint8_t p[1400];
    uint16_t c;
    int i, N = 1000000;
    double t0, plain, trusted, pool;
    size_t ul = sizeof p - 20;
    wg_ip_add_addr(ip, &d);
    wg_udp_bind(ip, 0, 53, on_udp, NULL);
    memset(p, 0x5a, sizeof p);
    memset(p, 0, 28);
    p[0] = 0x45;
    p[2] = (uint8_t)(sizeof p >> 8);
    p[3] = (uint8_t)sizeof p;
    p[8] = 64;
    p[9] = 17;
    memcpy(p + 12, s.a, 4);
    memcpy(p + 16, d.a, 4);
    c = wg_csum_fin(wg_csum_acc(0, p, 20));
    memcpy(p + 10, &c, 2);
    p[20] = 0x30;
    p[23] = 53;
    p[24] = (uint8_t)(ul >> 8);
    p[25] = (uint8_t)ul;
    c = wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&s, &d, 17, ul), p + 20, ul));
    memcpy(p + 26, &c, 2);
    if (!wg_ip_verify(p, sizeof p))
        printf("verify: packet build broken\n");
    t0 = tcpu();
    for (i = 0; i < N; i++)
        wg_ip_input(ip, p, sizeof p);
    plain = (tcpu() - t0) / N;
    t0 = tcpu();
    for (i = 0; i < N; i++)
        wg_ip_input_trusted(ip, p, sizeof p);
    trusted = (tcpu() - t0) / N;
    t0 = tcpu();
    for (i = 0; i < N; i++)
        sink += (uint64_t)wg_ip_verify(p, sizeof p);
    pool = (tcpu() - t0) / N;
    printf("verify: 1400 B UDP, loop thread %.1f ns/pkt untrusted vs %.1f ns/pkt trusted (saves %.1f ns, %.0f%%); wg_ip_verify on the pool %.1f ns/pkt\n", plain, trusted, plain - trusted, 100 * (plain - trusted) / plain, pool);
    wg_ip_free(ip);
}

enum { TOTAL = 32 << 20, MAXC = 256 };
static uint8_t txb[65536];
static size_t sent[MAXC], want_each;
static uint64_t got_total;
static int closed_n, open_n;

static void c_pump(void* ud, wg_tconn_t* c)
{
    size_t i = (size_t)ud;
    while (sent[i] < want_each) {
        size_t k = want_each - sent[i] > sizeof txb ? sizeof txb : want_each - sent[i];
        size_t w = wg_tcp_write(c, txb, k);
        if (!w)
            return;
        sent[i] += w;
    }
    wg_tcp_shutdown(c);
}

static void c_open(void* ud, wg_tconn_t* c)
{
    open_n++;
    c_pump(ud, c);
}

static void c_close(void* ud, wg_tconn_t* c, int err)
{
    (void)ud;
    (void)c;
    (void)err;
    closed_n++;
}

static void s_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    (void)ud;
    (void)c;
    (void)d;
    got_total += n;
}

static void s_eof(void* ud, wg_tconn_t* c)
{
    (void)ud;
    wg_tcp_shutdown(c);
}

static const wg_tcp_cb_t CB = { c_open, NULL, NULL, c_pump, c_close };
static const wg_tcp_cb_t SB = { NULL, s_data, s_eof, NULL, NULL };

static wg_glue_t* node(uint8_t d, uint8_t pub[32])
{
    uint8_t priv[32];
    wg_glue_t* g;
    wg_ipaddr_t x = a4(d);
    wg_keygen(priv, pub);
    g = wg_glue_new(aio, priv, "127.0.0.1", 0, NULL);
    wg_ip_add_addr(wg_glue_ip(g), &x);
    return g;
}

static void plink(wg_glue_t* a, const uint8_t* bpub, uint16_t bport, uint8_t b_ip)
{
    struct sockaddr_in s;
    wg_ipaddr_t x = a4(b_ip);
    wg_dpeer_t* p = wg_dev_add_peer(wg_glue_dev(a), bpub, NULL, 0, NULL, NULL);
    int k;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s.sin_port = htons(bport);
    if (bport)
        wg_dev_set_endpoint(p, (struct sockaddr*)&s, sizeof s);
    wg_dev_allow(wg_glue_dev(a), p, 0, x.a, 32);
    for (k = 0; k < 2000 && !wg_dev_peer_ready(p); k++)
        dyn_aio_run_us(aio, 1000);
}

static void bench_tcp(int n, const char* tag)
{
    uint8_t pa[32], pb[32];
    wg_glue_t *A = node(1, pa), *B = node(2, pb);
    wg_ipaddr_t dst = a4(2);
    double r0, r1, r2, c0, w0, wall, cpu;
    int i;
    plink(A, pb, (uint16_t)wg_dev_port(wg_glue_dev(B)), 2);
    plink(B, pa, 0, 1);
    wg_tcp_listen(wg_glue_tcp(B), 0, 80, 1024, &SB, NULL);
    want_each = TOTAL / (size_t)n;
    got_total = 0;
    closed_n = open_n = 0;
    memset(sent, 0, sizeof sent);
    r0 = rss_mb();
    c0 = tcpu();
    w0 = twall();
    for (i = 0; i < n; i++)
        wg_tcp_connect(wg_glue_tcp(A), &dst, 80, &CB, (void*)(size_t)i);
    wg_glue_kick(A);
    while (open_n < n && twall() - w0 < 20e9)
        dyn_aio_run_us(aio, 1000);
    r1 = rss_mb();
    while (closed_n < n && twall() - w0 < 60e9)
        dyn_aio_run_us(aio, 1000);
    wall = twall() - w0;
    cpu = tcpu() - c0;
    r2 = rss_mb();
    printf("tcp%s N=%3d: %.1f MB/s, loop thread %.0f us/MiB (both ends), delivered %s, RSS +%.2f MB at open (%.0f KB/conn), +%.2f MB at end\n", tag, n, (double)got_total / 1048576.0 / (wall / 1e9), cpu / 1e3 / ((double)got_total / 1048576.0),
        got_total == (uint64_t)want_each * (uint64_t)n ? "all" : "PARTIAL", r1 - r0, (r1 - r0) * 1024 / n, r2 - r0);
    {
        wg_tcp_stats_t ta, tb;
        wg_dev_stats_t da, db;
        wg_tcp_stats(wg_glue_tcp(A), &ta);
        wg_tcp_stats(wg_glue_tcp(B), &tb);
        wg_dev_stats(wg_glue_dev(A), &da);
        wg_dev_stats(wg_glue_dev(B), &db);
        printf("        A: segs %llu, rexmit %llu (rto %llu, fast %llu, sack %llu), dev tx_drop %llu; B: ooo %llu, ooo_drop %llu, dev rx_drop %llu, jobs %llu, inline %llu\n", (unsigned long long)ta.segs_out, (unsigned long long)ta.rexmit_segs, (unsigned long long)ta.rto_fired, (unsigned long long)ta.fast_rexmit,
            (unsigned long long)ta.sack_rexmit, (unsigned long long)da.tx_drop, (unsigned long long)tb.ooo_segs, (unsigned long long)tb.ooo_drop, (unsigned long long)db.rx_drop, (unsigned long long)db.jobs_offloaded, (unsigned long long)db.inline_ops);
    }
    wg_glue_free(A);
    wg_glue_free(B);
}

int main(int argc, char** argv)
{
    const char* only = argc > 1 ? argv[1] : "all";
    struct rusage u;
    int n;
    setvbuf(stdout, NULL, _IONBF, 0);
    aio = dyn_aio_new(1024, 0);
    memset(txb, 0x6b, sizeof txb);
    if (!strcmp(only, "all") || !strcmp(only, "isn"))
        bench_isn();
    if (!strcmp(only, "all") || !strcmp(only, "verify"))
        bench_verify();
    if (!strcmp(only, "all") || !strcmp(only, "tcp")) {
        for (n = 1; n <= MAXC; n *= 4)
            bench_tcp(n, "");
        for (n = 0; n < 3; n++) {
            bench_tcp(64, " repeat");
            printf("  RSS now %.1f MB\n", rss_mb());
        }
    }
    dyn_aio_free(aio);
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    printf("peak RSS %.1f MB\n", u.ru_maxrss / 1048576.0);
#else
    printf("peak RSS %.1f MB\n", u.ru_maxrss / 1024.0);
#endif
    return 0;
}
