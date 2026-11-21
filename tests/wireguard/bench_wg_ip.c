/*
 * bench_wg_ip.c -- dyna:wireguard S13c1 IP layer cost on the loop thread, and the pool handoff it would replace.
 *  rx: ns per wg_ip_input (validate, header + UDP checksum, port lookup, deliver) for v4/v6 UDP at 64..1420 B and
 *      ICMP echo (reply built and checksummed); checksum alone in GB/s.
 *  pool: main-thread CPU and wall per job for an empty job on dyn_pool (64 per batch, drained), the floor of offload.
 *  ports: bind N = 16..65535 fixed v4 ports: bytes per socket (RSS), lookup ns via input; 10 bind/unbind cycles of
 *      65535 ports, RSS after each (plateau).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "../../src/core/dyn-pool.h"
#include "../../src/modules/wireguard/wg-ip.h"

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

static size_t outs, sink;
static int out(void* ud, const uint8_t* p, size_t n)
{
    (void)ud;
    outs += n + p[0];
    return 0;
}

static void on_udp(void* ud, wg_udp_t* s, const wg_ipaddr_t* src, uint16_t sport, const uint8_t* d, size_t n)
{
    (void)ud;
    (void)s;
    (void)src;
    (void)sport;
    sink += n ? d[n - 1] : 1;
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

static wg_ip_t* stack(void)
{
    wg_ip_t* ip = wg_ip_new(out, NULL);
    wg_ipaddr_t x = ad(0, A4), y = ad(1, A6);
    wg_ip_add_addr(ip, &x);
    wg_ip_add_addr(ip, &y);
    return ip;
}

static uint8_t pkt[1500];
static size_t pkt_n;
static int cap_out(void* ud, const uint8_t* p, size_t n)
{
    (void)ud;
    memcpy(pkt, p, n);
    pkt_n = n;
    return 0;
}

static size_t build(int v6, int icmp, size_t total, uint16_t dport)
{
    wg_ip_t* g = wg_ip_new(cap_out, NULL);
    wg_ipaddr_t s = ad(v6, v6 ? B6 : B4), d = ad(v6, v6 ? A6 : A4);
    uint8_t body[1500];
    size_t hl = v6 ? 40 : 20, n;
    memset(body, 0x3c, sizeof body);
    wg_ip_add_addr(g, &s);
    if (icmp) {
        uint16_t c;
        n = total - hl;
        body[0] = v6 ? 128 : 8;
        body[1] = body[2] = body[3] = 0;
        c = wg_csum_fin(wg_csum_acc(v6 ? wg_csum_pseudo(&s, &d, 58, n) : 0, body, n));
        memcpy(body + 2, &c, 2);
        wg_ip_output(g, NULL, &d, v6 ? 58 : 1, body, n);
    } else {
        wg_udp_t* u = wg_udp_bind(g, v6, 4000, on_udp, NULL);
        wg_udp_sendto(u, &d, dport, body, total - hl - 8);
    }
    wg_ip_free(g);
    return pkt_n;
}

static void b_rx(void)
{
    static const size_t sz[] = { 64, 256, 576, 1024, 1420 };
    wg_ip_t* ip = stack();
    unsigned i, k, v6, icmp, iters = 400000;
    uint8_t buf[1500];
    double c0, w0;
    wg_udp_bind(ip, 0, 53, on_udp, NULL);
    wg_udp_bind(ip, 1, 53, on_udp, NULL);
    for (icmp = 0; icmp < 2; icmp++)
        for (v6 = 0; v6 < 2; v6++)
            for (k = 0; k < sizeof sz / sizeof sz[0]; k++) {
                size_t n = build((int)v6, (int)icmp, sz[k], 53);
                memcpy(buf, pkt, n);
                c0 = tcpu();
                for (i = 0; i < iters; i++)
                    wg_ip_input(ip, buf, n);
                printf("rx %s%s %4zu B: %6.1f ns/pkt\n", icmp ? "echo" : "udp ", v6 ? "6" : "4", n, (tcpu() - c0) / iters);
            }
    {
        uint64_t acc = 0;
        c0 = tcpu();
        for (i = 0; i < iters * 4; i++)
            acc += wg_csum_fin(wg_csum_acc(i, buf, 1420));
        w0 = tcpu() - c0;
        printf("checksum 1420 B: %.1f ns, %.2f GB/s (%llu)\n", w0 / (iters * 4), 1420.0 * iters * 4 / w0, (unsigned long long)(acc & 1));
    }
    wg_ip_free(ip);
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

static void b_ports(void)
{
    static const unsigned ns[] = { 16, 256, 4096, 16384, 65535 };
    static wg_udp_t* s[65536];
    unsigned k, i, c;
    uint8_t buf[1500];
    for (k = 0; k < sizeof ns / sizeof ns[0]; k++) {
        wg_ip_t* ip = stack();
        unsigned n = ns[k], iters = 200000;
        double r0 = rss_mb(), r1, c0;
        for (i = 0; i < n; i++)
            s[i] = wg_udp_bind(ip, 0, (uint16_t)(i + 1), on_udp, NULL);
        r1 = rss_mb();
        {
            static uint8_t pre[64][128];
            size_t pl[64];
            for (i = 0; i < 64; i++) {
                pl[i] = build(0, 0, 64, (uint16_t)(1 + (i * 2654435761u) % n));
                memcpy(pre[i], pkt, pl[i]);
            }
            c0 = tcpu();
            for (i = 0; i < iters; i++) {
                memcpy(buf, pre[i & 63], pl[i & 63]);
                wg_ip_input(ip, buf, pl[i & 63]);
            }
            printf("ports %5u: %7.0f B/socket (RSS +%.2f MB), udp rx with lookup %.1f ns\n", n, (r1 - r0) * 1048576.0 / n, r1 - r0, (tcpu() - c0) / iters);
        }
        wg_ip_free(ip);
    }
    {
        wg_ip_t* ip = stack();
        printf("bind/unbind 65535 ports x10, RSS:");
        for (c = 0; c < 10; c++) {
            for (i = 0; i < 65535; i++)
                s[i] = wg_udp_bind(ip, 0, (uint16_t)(i + 1), on_udp, NULL);
            for (i = 0; i < 65535; i++)
                wg_udp_close(s[i]);
            printf(" %.1f", rss_mb());
        }
        printf(" MB\n");
        wg_ip_free(ip);
    }
}

int main(int argc, char** argv)
{
    const char* m = argc > 1 ? argv[1] : "all";
    printf("rss start %.1f MB\n", rss_mb());
    if (!strcmp(m, "all") || !strcmp(m, "rx"))
        b_rx();
    if (!strcmp(m, "all") || !strcmp(m, "pool"))
        b_pool();
    if (!strcmp(m, "all") || !strcmp(m, "ports"))
        b_ports();
    printf("peak-ish rss %.1f MB (sink %zu %zu)\n", rss_mb(), sink & 1, outs & 1);
    return 0;
}
