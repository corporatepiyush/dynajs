/* dyna:wireguard device benchmark (rule 11), S13b2: one wg-device on the real aio loop and pool (main thread runs only
 * the loop), simulated peers in 8 client threads over loopback UDP (each client: its own static key, Noise
 * initiation, response, then one data packet from its allowed IP).
 * hs N: peers N=16..65536: handshakes/s, device main-thread CPU us per handshake (all X25519 on the pool), RSS and
 *   bytes/peer after add_peer and after handshakes (keypairs), add/remove cycles at 16384 peers (plateau);
 * pkts: 16..262144 x 1420 B data packets from one client: device main-thread CPU ns/pkt, wall ns/pkt, RSS growth.
 * The load threshold is lifted (clients do not answer cookies). Client-side crypto is not device cost.
 * cc -O2 -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WIREGUARD -Isrc -Isrc/core
 *    tests/wireguard/bench_wg_device.c src/modules/wireguard/wg-*.c src/dyna-aio.c src/dyna-evloop.c src/dyna-io.c
 *    src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c $(pkg-config --cflags --libs openssl) -lpthread */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "dyna-aio.h"
#include "modules/wireguard/wg-internal.h"
#include "../lib/dyn-xalloc.h"

#define NT 8
#define WIN 48

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
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &c) == KERN_SUCCESS)
        return i.resident_size / 1048576.0;
    return 0;
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

typedef struct {
    wg_static_t s;
    wg_peer_t np;
    uint8_t send[32], recv[32];
    int ok;
} client_t;

static client_t* cl;
static int N;
static const uint8_t* dev_pub;
static struct sockaddr_in dev_addr;
static atomic_int setup_done, clients_done, rx_count, failures;
static wg_dev_t* dev;

static atomic_int ready_n;
static void on_ready(void* ud, wg_dpeer_t* p, int rc)
{
    (void)ud;
    (void)p;
    if (!rc)
        atomic_fetch_add(&ready_n, 1);
}

static void on_rx(void* ud, wg_dpeer_t* p, const uint8_t* ip, size_t n, int verified)
{
    (void)ud;
    (void)verified;
    (void)p;
    (void)ip;
    (void)n;
    atomic_fetch_add(&rx_count, 1);
}

static wg_peer_t* by_index(void* ud, uint32_t i)
{
    (void)ud;
    return i < (uint32_t)N ? &cl[i].np : NULL;
}

static void* client_setup(void* arg)
{
    int t = (int)(intptr_t)arg, i;
    for (i = t; i < N; i += NT) {
        uint8_t priv[32], pub[32];
        wg_keygen(priv, pub);
        wg_static_init(&cl[i].s, priv);
        wg_peer_init(&cl[i].np, &cl[i].s, dev_pub, NULL);
    }
    atomic_fetch_add(&setup_done, 1);
    return NULL;
}

static void* client_run(void* arg)
{
    int t = (int)(intptr_t)arg, fd = socket(AF_INET, SOCK_DGRAM, 0), i, base, sz = 4 << 20;
    struct timeval tv = { 0, 200000 };
    uint8_t buf[2048];
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    for (base = t; base < N; base += NT * WIN) {
        int want = 0, got = 0, tries = 0;
        for (i = base; i < N && i < base + NT * WIN; i += NT) {
            uint8_t m[WG_INIT_LEN];
            wg_create_initiation(&cl[i].s, &cl[i].np, (uint32_t)i, 1800000000000000000ULL + (uint64_t)i, 1, m);
            sendto(fd, m, sizeof m, 0, (struct sockaddr*)&dev_addr, sizeof dev_addr);
            want++;
        }
        while (got < want && tries < 20) {
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            wg_peer_t* p;
            uint32_t ri;
            if (n < 0) {
                tries++;
                continue;
            }
            ri = n == WG_RESP_LEN ? wg_le32(buf + 8) : (uint32_t)N;
            p = ri < (uint32_t)N ? wg_consume_response(&cl[ri].s, buf, (size_t)n, by_index, NULL) : NULL;
            if (!p)
                continue;
            {
                client_t* c = (client_t*)(void*)((uint8_t*)p - offsetof(client_t, np));
                uint32_t idx = (uint32_t)(c - cl), remote = wg_le32(buf + 4);
                uint8_t pkt[16 + 64 + 32] = { 0 };
                int r;
                wg_derive_keys(p, c->send, c->recv);
                c->ok = 1;
                pkt[16] = 0x45;
                pkt[19] = 64;
                pkt[28] = 10;
                pkt[29] = (uint8_t)(idx >> 16);
                pkt[30] = (uint8_t)(idx >> 8);
                pkt[31] = (uint8_t)idx;
                pkt[32] = 10;
                pkt[33] = 255;
                pkt[34] = 255;
                pkt[35] = 254;
                r = wg_data_seal(c->send, remote, 0, pkt, 64, sizeof pkt, 1420);
                sendto(fd, pkt, (size_t)r, 0, (struct sockaddr*)&dev_addr, sizeof dev_addr);
                got++;
            }
        }
        atomic_fetch_add(&failures, want - got);
    }
    close(fd);
    atomic_fetch_add(&clients_done, 1);
    return NULL;
}

static void bench_hs(dyn_aio_t* aio, int n)
{
    uint8_t priv[32], pub[32];
    pthread_t th[NT];
    double r0, r1, r2, c0, w0, cpu, wall;
    int i;
    N = n;
    cl = (client_t*)calloc((size_t)n, sizeof *cl);
    wg_keygen(priv, pub);
    r0 = rss_mb();
    dev = wg_dev_new(aio, priv, "127.0.0.1", 0, on_rx, NULL);
    wg_dev_set_load_threshold(dev, 1u << 30);
    dev_pub = wg_dev_pub(dev);
    dev_addr.sin_family = AF_INET;
    dev_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dev_addr.sin_port = htons((uint16_t)wg_dev_port(dev));
    atomic_store(&setup_done, 0);
    for (i = 0; i < NT; i++)
        pthread_create(&th[i], NULL, client_setup, (void*)(intptr_t)i);
    for (i = 0; i < NT; i++)
        pthread_join(th[i], NULL);
    atomic_store(&ready_n, 0);
    c0 = tcpu();
    for (i = 0; i < n; i++) {
        uint8_t a[4] = { 10, (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i };
        wg_dpeer_t* p = wg_dev_add_peer(dev, cl[i].s.pub, NULL, 0, on_ready, NULL);
        wg_dev_allow(dev, p, 0, a, 32);
    }
    cpu = (tcpu() - c0) / n;
    while (atomic_load(&ready_n) < n)
        dyn_aio_run_us(aio, 1000);
    r1 = rss_mb();
    atomic_store(&clients_done, 0);
    atomic_store(&rx_count, 0);
    atomic_store(&failures, 0);
    c0 = tcpu();
    w0 = twall();
    for (i = 0; i < NT; i++)
        pthread_create(&th[i], NULL, client_run, (void*)(intptr_t)i);
    while (atomic_load(&clients_done) < NT || (atomic_load(&rx_count) < n - atomic_load(&failures) && twall() - w0 < 30e9)) {
        dyn_aio_run_us(aio, 2000);
        wg_dev_flush(dev);
    }
    for (i = 0; i < 20; i++)
        dyn_aio_run_us(aio, 1000);
    wall = twall() - w0;
    {
        double main_cpu = (tcpu() - c0) / n;
        for (i = 0; i < NT; i++)
            pthread_join(th[i], NULL);
        r2 = rss_mb();
        printf("peers %6d: add_peer %.2f us (main; X25519 precompute on the pool), handshakes %.0f/s, device main-thread %.1f us/handshake+packet, delivered %d failed %d, RSS +%.2f MB after add (%.0f B/peer), +%.2f MB after handshakes (%.0f B/peer)\n",
            n, cpu / 1e3, n / (wall / 1e9), main_cpu / 1e3, atomic_load(&rx_count), atomic_load(&failures), r1 - r0, (r1 - r0) * 1048576 / n, r2 - r0, (r2 - r0) * 1048576 / n);
    }
    wg_dev_free(dev);
    for (i = 0; i < n; i++)
        wg_static_free(&cl[i].s);
    free(cl);
}

static void bench_cycles(dyn_aio_t* aio)
{
    uint8_t priv[32], pub[32];
    int c, i, n = 16384;
    uint8_t (*pubs)[32] = malloc((size_t)n * 32);
    wg_keygen(priv, pub);
    for (i = 0; i < n; i++)
        wg_random(pubs[i], 32);
    dev = wg_dev_new(aio, priv, "127.0.0.1", 0, on_rx, NULL);
    printf("add/remove 16384 peers, RSS after each cycle:");
    for (c = 0; c < 10; c++) {
        wg_dpeer_t** ps = xmalloc((size_t)n * sizeof *ps);
        for (i = 0; i < n; i++) {
            uint8_t a[4] = { 10, 0, (uint8_t)(i >> 8), (uint8_t)i };
            ps[i] = wg_dev_add_peer(dev, pubs[i], NULL, 0, NULL, NULL);
            wg_dev_allow(dev, ps[i], 0, a, 32);
        }
        for (i = 0; i < n; i++)
            wg_dev_remove_peer(dev, ps[i]);
        free(ps);
        printf(" %.1f", rss_mb());
    }
    printf(" MB\n");
    wg_dev_free(dev);
    free(pubs);
}

typedef struct {
    int fd, m, base;
    uint32_t remote;
    uint64_t* ctr;
    const uint8_t* key;
} sender_t;

static void* sender(void* arg)
{
    sender_t* s = (sender_t*)arg;
    static uint8_t pk[1500];
    int i;
    for (i = 0; i < s->m; i++) {
        memset(pk, 0, sizeof pk);
        pk[16] = 0x45;
        pk[18] = 1420 >> 8;
        pk[19] = 1420 & 255;
        pk[28] = 10;
        pk[31] = 1;
        wg_data_seal(s->key, s->remote, (*s->ctr)++, pk, 1420, sizeof pk, 1420);
        while (sendto(s->fd, pk, 1452, 0, (struct sockaddr*)&dev_addr, sizeof dev_addr) < 0)
            usleep(50);
        while (i - (atomic_load(&rx_count) - s->base) > 512)
            usleep(20);
    }
    return NULL;
}

static void bench_pkts(dyn_aio_t* aio)
{
    uint8_t priv[32], pub[32], cpriv[32], cpub[32], m[WG_INIT_LEN], buf[2048];
    int fd = socket(AF_INET, SOCK_DGRAM, 0), sz = 4 << 20, M;
    uint8_t ks[32], kr[32];
    uint32_t remote = 0;
    uint64_t ctr = 0;
    double r0;
    wg_dpeer_t* p;
    wg_keygen(priv, pub);
    wg_keygen(cpriv, cpub);
    dev = wg_dev_new(aio, priv, "127.0.0.1", 0, on_rx, NULL);
    N = 1;
    cl = (client_t*)calloc(1, sizeof *cl);
    wg_static_init(&cl[0].s, cpriv);
    wg_peer_init(&cl[0].np, &cl[0].s, wg_dev_pub(dev), NULL);
    p = wg_dev_add_peer(dev, cpub, NULL, 0, NULL, NULL);
    wg_dev_allow(dev, p, 0, (const uint8_t[4]) { 10, 0, 0, 1 }, 32);
    dev_addr.sin_family = AF_INET;
    dev_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dev_addr.sin_port = htons((uint16_t)wg_dev_port(dev));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    wg_create_initiation(&cl[0].s, &cl[0].np, 0, 1800000000000000000ULL, 1, m);
    sendto(fd, m, sizeof m, 0, (struct sockaddr*)&dev_addr, sizeof dev_addr);
    for (;;) {
        ssize_t n;
        dyn_aio_run_us(aio, 2000);
        n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n == WG_RESP_LEN && wg_consume_response(&cl[0].s, buf, (size_t)n, by_index, NULL)) {
            remote = wg_le32(buf + 4);
            wg_derive_keys(&cl[0].np, ks, kr);
            break;
        }
    }
    r0 = rss_mb();
    for (M = 16; M <= 262144; M *= 4) {
        pthread_t th;
        int base = atomic_load(&rx_count);
        sender_t s = { fd, M, base, remote, &ctr, ks };
        double c0 = tcpu(), w0 = twall();
        pthread_create(&th, NULL, sender, &s);
        while (atomic_load(&rx_count) - base < M && twall() - w0 < 10e9)
            dyn_aio_run_us(aio, 1000);
        pthread_join(th, NULL);
        printf("packets %7d x 1420 B in: delivered %d, device main-thread CPU %.0f ns/pkt, wall %.0f ns/pkt (%.0f MB/s), RSS growth %+.2f MB\n", M, atomic_load(&rx_count) - base, (tcpu() - c0) / M, (twall() - w0) / M, 1420.0 * M / ((twall() - w0) / 1e3), rss_mb() - r0);
    }
    (void)kr;
    close(fd);
    wg_dev_free(dev);
    wg_static_free(&cl[0].s);
    free(cl);
}

int main(int argc, char** argv)
{
    const char* only = argc > 1 ? argv[1] : "all";
    dyn_aio_t* aio = dyn_aio_new(256, 0);
    int n;
    struct rusage u;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(only, "all") || !strcmp(only, "hs"))
        for (n = 16; n <= 65536; n *= 4)
            bench_hs(aio, n);
    if (!strcmp(only, "all") || !strcmp(only, "cycles"))
        bench_cycles(aio);
    if (!strcmp(only, "all") || !strcmp(only, "pkts"))
        bench_pkts(aio);
    dyn_aio_free(aio);
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    printf("peak RSS %.1f MB\n", u.ru_maxrss / 1048576.0);
#else
    printf("peak RSS %.1f MB\n", u.ru_maxrss / 1024.0);
#endif
    return 0;
}
