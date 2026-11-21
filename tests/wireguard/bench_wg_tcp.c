/*
 * bench_wg_tcp.c -- dyna:wireguard S13c2 TCP cost on the loop thread (two stacks wired back to back, no loss).
 *  bulk: CPU ns per 1380/1360 B data segment end to end (sender + receiver + ACKs + checksums), CPU Gbit/s, v4 and v6.
 *  rr: 64 B request/response round trips, nodelay, CPU ns per round trip.
 *  pool: main-thread CPU per empty dyn_pool job (64 per batch), the floor any offload of TCP would pay per segment.
 *  conns: N = 16..65536 idle established connections: bytes per endpoint (RSS), handshake CPU, tick cost at N;
 *      then 10 open/transfer/close cycles of 16384 connections with TIME_WAIT expiry, RSS after each (plateau).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "../../src/core/dyn-pool.h"
#include "../../src/modules/wireguard/wg-tcp.h"

static double tcpu(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static double twall(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static double rss_mb(void)
{
#ifdef __APPLE__
    mach_task_basic_info_data_t i;
    mach_msg_type_number_t c = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &c) == KERN_SUCCESS)
        return (double)i.resident_size / 1048576.0;
    return 0;
#else
    long a = 0, b = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &a, &b) != 2)
            b = 0;
        fclose(f);
    }
    return (double)b * 4096.0 / 1048576.0;
#endif
}

enum { QN = 32768 };
typedef struct {
    int to;
    size_t n;
    uint8_t b[WG_IP_MTU];
} pk_t;
static pk_t* q;
static unsigned qh, qt;
static uint64_t now_ms = 1000;
static wg_ip_t* IP[2];
static wg_tcp_t* T[2];
static size_t sink;

static uint64_t clk(void* ud)
{
    (void)ud;
    return now_ms;
}

static int out(void* ud, const uint8_t* p, size_t n)
{
    pk_t* k;
    if (qt - qh >= QN)
        return -1;
    k = &q[qt++ % QN];
    k->to = 1 - (int)(size_t)ud;
    k->n = n;
    memcpy(k->b, p, n);
    return 0;
}

static void pump(void)
{
    while (qh != qt) {
        pk_t* k = &q[qh++ % QN];
        wg_ip_input(IP[k->to], k->b, k->n);
    }
}

static void advance(uint64_t ms)
{
    now_ms += ms;
    wg_tcp_tick(T[0]);
    wg_tcp_tick(T[1]);
    pump();
}

static const uint8_t A4[4] = { 10, 0, 0, 1 }, B4[4] = { 10, 0, 0, 2 };
static const uint8_t A6[16] = { 0xfd, [15] = 1 }, B6[16] = { 0xfd, [15] = 2 };

static wg_ipaddr_t ad(int v6, const uint8_t* a)
{
    wg_ipaddr_t r;
    memset(&r, 0, sizeof r);
    r.v6 = v6;
    memcpy(r.a, a, v6 ? 16 : 4);
    return r;
}

static void stacks(void)
{
    int i;
    qh = qt = 0;
    for (i = 0; i < 2; i++) {
        wg_ipaddr_t x = ad(0, i ? B4 : A4), y = ad(1, i ? B6 : A6);
        IP[i] = wg_ip_new(out, (void*)(size_t)i);
        wg_ip_set_clock(IP[i], clk, NULL);
        wg_ip_add_addr(IP[i], &x);
        wg_ip_add_addr(IP[i], &y);
        T[i] = wg_tcp_new(IP[i], clk, NULL);
    }
}

static void stacks_free(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        wg_tcp_free(T[i]);
        wg_ip_free(IP[i]);
    }
}

static uint8_t blob[65536];
static uint64_t to_send, rcvd, opened, closed, rr_left;

static void cl_fill(void* ud, wg_tconn_t* c)
{
    (void)ud;
    while (to_send) {
        size_t w = wg_tcp_write(c, blob, to_send < sizeof blob ? (size_t)to_send : sizeof blob);
        if (!w)
            return;
        to_send -= w;
    }
}

static void cl_open(void* ud, wg_tconn_t* c)
{
    opened++;
    cl_fill(ud, c);
}

static void cl_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    (void)ud;
    sink += d[0];
    rcvd += n;
    if (rr_left && rcvd % 64 == 0 && --rr_left)
        wg_tcp_write(c, blob, 64);
}

static void cl_close(void* ud, wg_tconn_t* c, int err)
{
    (void)ud;
    (void)c;
    (void)err;
    closed++;
}

static void sv_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    (void)ud;
    sink += d[n - 1];
    if (rr_left)
        wg_tcp_write(c, d, n);
    else
        rcvd += n;
}

static void sv_eof(void* ud, wg_tconn_t* c)
{
    (void)ud;
    wg_tcp_shutdown(c);
}

static const wg_tcp_cb_t CL = { cl_open, cl_data, NULL, cl_fill, cl_close };
static const wg_tcp_cb_t SV = { NULL, sv_data, sv_eof, NULL, cl_close };

static void b_bulk(int v6)
{
    wg_ipaddr_t dst = ad(v6, v6 ? B6 : B4);
    wg_tcp_stats_t s0, s1;
    wg_tcp_info_t inf;
    wg_tconn_t* c;
    uint64_t total = 512ull << 20;
    double c0, w0, r0, rpk = 0;
    stacks();
    wg_tcp_listen(T[1], v6, 80, 16, &SV, NULL);
    r0 = rss_mb();
    to_send = total;
    rcvd = 0;
    c0 = tcpu();
    w0 = twall();
    c = wg_tcp_connect(T[0], &dst, 80, &CL, NULL);
    while (rcvd < total) {
        pump();
        if (rss_mb() > rpk)
            rpk = rss_mb();
        if (rcvd < total)
            advance(1);
    }
    c0 = tcpu() - c0;
    w0 = twall() - w0;
    wg_tcp_stats(T[0], &s0);
    wg_tcp_stats(T[1], &s1);
    wg_tcp_info(c, &inf);
    printf("bulk v%d: %llu MiB in %llu data segs (mss %u), %.0f ns/seg CPU end to end, %.2f Gbit/s CPU, wall %.0f ms, acks %llu, rexmit %llu, RSS +%.1f MB peak\n", v6 ? 6 : 4,
           (unsigned long long)(total >> 20), (unsigned long long)s0.segs_out, inf.mss, c0 / (double)s0.segs_out, (double)total * 8 / c0, w0 / 1e6,
           (unsigned long long)s1.segs_out, (unsigned long long)s0.rexmit_segs, rpk - r0);
    stacks_free();
}

static void b_rr(void)
{
    wg_ipaddr_t dst = ad(0, B4);
    wg_tconn_t* c;
    unsigned n = 200000;
    double c0;
    stacks();
    wg_tcp_listen(T[1], 0, 80, 16, &SV, NULL);
    to_send = 0;
    rcvd = 0;
    opened = 0;
    c = wg_tcp_connect(T[0], &dst, 80, &CL, NULL);
    pump();
    wg_tcp_set_nodelay(c, 1);
    rr_left = n;
    c0 = tcpu();
    wg_tcp_write(c, blob, 64);
    while (rr_left) {
        pump();
        if (rr_left)
            advance(1);
    }
    printf("rr 64 B v4 nodelay: %.0f ns/round trip CPU (both stacks)\n", (tcpu() - c0) / n);
    stacks_free();
}

static void nop(void* a)
{
    (void)a;
}

static void b_pool(void)
{
    dyn_pool_t* p = dyn_pool_new(4, 4096);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    unsigned r, i, rounds = 2000;
    double c0 = tcpu(), w0 = twall();
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < 64; i++)
            dyn_pool_submit_cpu(ch, nop, nop, NULL);
        dyn_pool_drain(ch);
    }
    printf("pool handoff (empty job, 64 per batch): main CPU %.0f ns/job, wall %.0f ns/job\n", (tcpu() - c0) / (rounds * 64.0), (twall() - w0) / (rounds * 64.0));
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
}

static wg_tconn_t** cs;

static void open_n(unsigned n)
{
    wg_ipaddr_t dst = ad(0, B4);
    unsigned i;
    for (i = 0; i < n; i++) {
        cs[i] = wg_tcp_connect(T[0], &dst, (uint16_t)(80 + (i >> 14)), &CL, NULL);
        if ((i & 255) == 255)
            pump();
    }
    pump();
}

static void close_n(unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        if (cs[i])
            wg_tcp_shutdown(cs[i]);
        if ((i & 255) == 255)
            pump();
    }
    pump();
    for (i = 0; i < 2 * WG_TCP_MSL_MS / 1000 + 2; i++)
        advance(1000);
}

static void b_conns(void)
{
    static const unsigned ns[] = { 16, 256, 4096, 16384, 65536 };
    unsigned k, i, cyc;
    wg_tcp_stats_t s0, s1;
    cs = (wg_tconn_t**)calloc(65536, sizeof *cs);
    for (k = 0; k < sizeof ns / sizeof ns[0]; k++) {
        unsigned n = ns[k], reps = 2000;
        double r0, r1, c0, ct;
        stacks();
        for (i = 0; i < 4; i++)
            wg_tcp_listen(T[1], 0, (uint16_t)(80 + i), 65535, &SV, NULL);
        to_send = 0;
        opened = 0;
        r0 = rss_mb();
        c0 = tcpu();
        open_n(n);
        c0 = tcpu() - c0;
        r1 = rss_mb();
        wg_tcp_stats(T[0], &s0);
        wg_tcp_stats(T[1], &s1);
        ct = tcpu();
        for (i = 0; i < reps; i++) {
            now_ms += 1;
            wg_tcp_tick(T[0]);
            wg_tcp_tick(T[1]);
            sink += (size_t)wg_tcp_next_deadline(T[0]);
        }
        ct = (tcpu() - ct) / reps;
        printf("conns %5u: open %llu, live %llu+%llu, %6.0f B/endpoint (RSS +%.1f MB), handshake %.0f ns/conn CPU, idle tick %.0f ns\n", n, (unsigned long long)opened,
               (unsigned long long)s0.live, (unsigned long long)s1.live, (r1 - r0) * 1048576.0 / (2.0 * n), r1 - r0, c0 / n, ct);
        close_n(n);
        wg_tcp_stats(T[0], &s0);
        if (s0.live)
            printf("  live after close %llu\n", (unsigned long long)s0.live);
        stacks_free();
    }
    stacks();
    for (i = 0; i < 4; i++)
        wg_tcp_listen(T[1], 0, (uint16_t)(80 + i), 65535, &SV, NULL);
    printf("open/transfer/close 16384 conns x10, RSS:");
    for (cyc = 0; cyc < 10; cyc++) {
        to_send = 0;
        open_n(16384);
        for (i = 0; i < 16384; i++) {
            if (cs[i])
                wg_tcp_write(cs[i], blob, 4096);
            if ((i & 255) == 255)
                pump();
        }
        pump();
        advance(250);
        close_n(16384);
        wg_tcp_stats(T[0], &s0);
        wg_tcp_stats(T[1], &s1);
        printf(" %.1f%s", rss_mb(), s0.live || s1.live ? "(live!)" : "");
    }
    printf(" MB; rexmit segs %llu+%llu, tx_fail %llu+%llu\n", (unsigned long long)s0.rexmit_segs, (unsigned long long)s1.rexmit_segs, (unsigned long long)s0.tx_fail, (unsigned long long)s1.tx_fail);
    stacks_free();
    free(cs);
}

int main(int argc, char** argv)
{
    const char* m = argc > 1 ? argv[1] : "all";
    q = (pk_t*)malloc(sizeof *q * QN);
    memset(blob, 0x5a, sizeof blob);
    printf("rss start %.1f MB\n", rss_mb());
    if (!strcmp(m, "all") || !strcmp(m, "bulk")) {
        b_bulk(0);
        b_bulk(1);
    }
    if (!strcmp(m, "all") || !strcmp(m, "rr"))
        b_rr();
    if (!strcmp(m, "all") || !strcmp(m, "pool"))
        b_pool();
    if (!strcmp(m, "all") || !strcmp(m, "conns"))
        b_conns();
    printf("peak-ish rss %.1f MB (sink %zu)\n", rss_mb(), sink & 1);
    free(q);
    return 0;
}
