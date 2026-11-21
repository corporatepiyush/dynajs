/* dyna:wireguard benchmark (rule 11), S13b1 layer: links the wg-*.c files, OpenSSL and src/core/dyn-pool.c.
 * 1 handshake steps, main-thread us/op (static X25519 key cached in wg_static_t);
 * 2 data AEAD per packet at 64/512/1420 B: EVP context per call (S13a) vs per thread (now) vs per keypair (key set once);
 * 3 pool handoff: K packets per job (K=1..64), main-thread CPU per packet (submit + done) against inline sealing, and
 *   throughput with 1/2/4/8 workers;
 * 4 memory: peers N=16..65536 (peer + pubtab + 3 indices + v4/32 and v6/128 allowed IPs + 3 keypairs): bytes/peer,
 *   RSS, lookups ns/op; 20 build/teardown cycles at 65536 (plateau); packets N=16..1M through the pool: RSS growth.
 * cc -O2 -std=gnu17 -Isrc -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WIREGUARD tests/wireguard/bench_wg.c
 *    src/modules/wireguard/wg-{crypto,noise,transport,timers,table}.c src/core/dyn-pool.c $(pkg-config --cflags --libs openssl) */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <openssl/evp.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "core/dyn-pool.h"
#include "modules/wireguard/wg-internal.h"

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
static double peak_mb(void)
{
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return u.ru_maxrss / 1048576.0;
#else
    return u.ru_maxrss / 1024.0;
#endif
}

static wg_peer_t PI, PR;
static wg_static_t SI, SR;
static wg_peer_t* bp(void* u, const uint8_t p[32])
{
    (void)u;
    (void)p;
    return &PR;
}
static wg_peer_t* bi(void* u, uint32_t i)
{
    (void)u;
    (void)i;
    return &PI;
}

static void bench_handshake(void)
{
    uint8_t a[32], b[32], pub[32], init[148], resp[92], s1[32], r1[32];
    double t0, ci = 0, cn = 0, cr = 0, cc = 0, pi = 0;
    int i, n = 3000;
    uint64_t ns = 1700000000000000000ULL, ms = 1000;
    wg_keygen(a, pub);
    wg_keygen(b, pub);
    wg_static_init(&SI, a);
    wg_static_init(&SR, b);
    for (i = 0; i < n; i++) {
        t0 = tcpu();
        wg_peer_init(&PI, &SI, SR.pub, NULL);
        pi += tcpu() - t0;
        wg_peer_init(&PR, &SR, SI.pub, NULL);
        ns += 1000000000;
        ms += 1000;
        t0 = tcpu();
        wg_create_initiation(&SI, &PI, 1, ns, ms, init);
        ci += tcpu() - t0;
        t0 = tcpu();
        if (!wg_consume_initiation(&SR, init, 148, ms, bp, NULL))
            exit(1);
        cn += tcpu() - t0;
        t0 = tcpu();
        wg_create_response(&SR, &PR, 2, ms, resp);
        cr += tcpu() - t0;
        t0 = tcpu();
        if (!wg_consume_response(&SI, resp, 92, bi, NULL))
            exit(1);
        cc += tcpu() - t0;
        wg_derive_keys(&PI, s1, r1);
        wg_derive_keys(&PR, s1, r1);
    }
    printf("handshake us/op: peer_init %.1f create_init %.1f consume_init %.1f create_resp %.1f consume_resp %.1f\n", pi / n / 1e3, ci / n / 1e3, cn / n / 1e3, cr / n / 1e3, cc / n / 1e3);
}

static int seal_percall(const uint8_t key[32], uint64_t ctr, uint8_t* buf, size_t n)
{
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    uint8_t iv[12] = { 0 };
    int ol, ok;
    wg_put_le64(iv + 4, ctr);
    ok = EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), NULL, key, iv) == 1 && EVP_EncryptUpdate(c, buf, &ol, buf, (int)n) == 1 && EVP_EncryptFinal_ex(c, buf + n, &ol) == 1 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, buf + n) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}

static EVP_CIPHER_CTX* kp_ctx;
static int seal_perkp(uint64_t ctr, uint8_t* buf, size_t n)
{
    uint8_t iv[12] = { 0 };
    int ol;
    wg_put_le64(iv + 4, ctr);
    return EVP_EncryptInit_ex(kp_ctx, NULL, NULL, NULL, iv) == 1 && EVP_EncryptUpdate(kp_ctx, buf, &ol, buf, (int)n) == 1 && EVP_EncryptFinal_ex(kp_ctx, buf + n, &ol) == 1 && EVP_CIPHER_CTX_ctrl(kp_ctx, EVP_CTRL_AEAD_GET_TAG, 16, buf + n) == 1 ? 0 : -1;
}

static void bench_aead(void)
{
    static uint8_t buf[2048];
    uint8_t key[32];
    int sizes[3] = { 64, 512, 1420 }, z, i, m = 200000, rep;
    wg_random(key, 32);
    kp_ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(kp_ctx, EVP_chacha20_poly1305(), NULL, key, NULL);
    for (z = 0; z < 3; z++) {
        double best[3] = { 1e30, 1e30, 1e30 };
        for (rep = 0; rep < 3; rep++) {
            double t0 = tcpu(), d;
            for (i = 0; i < m; i++)
                seal_percall(key, (uint64_t)i, buf, (size_t)sizes[z]);
            d = (tcpu() - t0) / m;
            if (d < best[0])
                best[0] = d;
            t0 = tcpu();
            for (i = 0; i < m; i++)
                wg_data_seal(key, 1, (uint64_t)i, buf, (size_t)sizes[z], sizeof buf, 1440);
            d = (tcpu() - t0) / m;
            if (d < best[1])
                best[1] = d;
            t0 = tcpu();
            for (i = 0; i < m; i++)
                seal_perkp((uint64_t)i, buf, (size_t)sizes[z]);
            d = (tcpu() - t0) / m;
            if (d < best[2])
                best[2] = d;
        }
        printf("seal %4d B ns/pkt (best of 3): ctx-per-call %.0f  ctx-per-thread(product, incl. header+pad) %.0f  ctx-per-keypair %.0f  -> %.0f MB/s\n", sizes[z], best[0], best[1], best[2], sizes[z] / best[1] * 1e3);
    }
    EVP_CIPHER_CTX_free(kp_ctx);
}

typedef struct {
    wg_batch_t b;
    uint8_t mem[WG_BATCH_MAX][1480];
} jb_t;
static int jdone;
static void jdone_fn(void* a)
{
    ((wg_batch_t*)a)->done = 1;
    jdone++;
}

static void wait_all(dyn_pool_chan_t* ch)
{
    while (dyn_pool_inflight(ch)) {
        struct pollfd pf = { dyn_pool_wake_fd(ch), POLLIN, 0 };
        poll(&pf, 1, 50);
        dyn_pool_drain(ch);
    }
}

static void prep(jb_t* j, int k, const uint8_t key[32], uint64_t* ctr)
{
    int i;
    memcpy(j->b.key, key, 32);
    j->b.enc = 1;
    j->b.mtu = 1440;
    j->b.n = (uint16_t)k;
    for (i = 0; i < k; i++) {
        j->b.pkt[i].buf = j->mem[i];
        j->b.pkt[i].cap = sizeof j->mem[i];
        j->b.pkt[i].len = 1420;
        j->b.pkt[i].ctr = (*ctr)++;
    }
}

static void bench_pool(void)
{
    static jb_t jb[64];
    uint8_t key[32];
    int ks[5] = { 1, 8, 16, 32, 64 }, th[4] = { 1, 2, 4, 8 }, z, w, i;
    uint64_t ctr = 0;
    double inl;
    wg_random(key, 32);
    {
        double t0 = tcpu();
        for (i = 0; i < 20000; i++)
            wg_data_seal(key, 1, ctr++, jb[0].mem[0], 1420, 1480, 1440);
        inl = (tcpu() - t0) / 20000;
    }
    printf("inline seal 1420 B: %.0f ns/pkt main-thread CPU\n", inl);
    for (w = 0; w < 4; w++) {
        dyn_pool_t* p = dyn_pool_new((unsigned)th[w], 256);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        for (z = 0; z < 5; z++) {
            int total = 64000, jobs = total / ks[z], inflight = 64, sent = 0;
            double c0, w0;
            for (i = 0; i < 64; i++)
                prep(&jb[i], ks[z], key, &ctr);
            dyn_pool_submit_cpu(ch, wg_batch_work, jdone_fn, &jb[0].b);
            wait_all(ch);
            c0 = tcpu();
            w0 = twall();
            while (sent < jobs) {
                while (sent < jobs && (int)dyn_pool_inflight(ch) < inflight) {
                    jb_t* j = &jb[sent % 64];
                    for (i = 0; i < ks[z]; i++)
                        j->b.pkt[i].ctr = ctr++;
                    if (dyn_pool_submit_cpu(ch, wg_batch_work, jdone_fn, &j->b) != 0)
                        break;
                    sent++;
                }
                {
                    struct pollfd pf = { dyn_pool_wake_fd(ch), POLLIN, 0 };
                    poll(&pf, 1, 50);
                }
                dyn_pool_drain(ch);
            }
            wait_all(ch);
            {
                double cpu = (tcpu() - c0) / total, wall = (twall() - w0) / total;
                printf("pool %d workers, %2d pkts/job: main-thread CPU %.0f ns/pkt (inline %.0f), wall %.0f ns/pkt = %.0f MB/s\n", th[w], ks[z], cpu, inl, wall, 1420 / wall * 1e3);
            }
        }
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }
}

typedef struct {
    wg_peer_t p;
    wg_keypair_t kp[3];
    wg_slots_t slots;
} bpeer_t;
static const uint8_t* bpub(const void* v)
{
    return ((const bpeer_t*)v)->p.pub;
}

static double build(int n, int report, double base)
{
    bpeer_t* ps = calloc((size_t)n, sizeof *ps);
    wg_pubtab_t pt;
    wg_idx_t ix;
    wg_aip_t aip;
    double t0, r0 = rss_mb(), r1, tl[3];
    int i, j, q = 200000;
    uint32_t idxs[3];
    volatile uintptr_t sink = 0;
    memset(&aip, 0, sizeof aip);
    wg_pubtab_init(&pt, bpub);
    wg_idx_init(&ix, 16);
    for (i = 0; i < n; i++) {
        uint8_t a4[4] = { 10, (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i }, a6[16] = { 0xfd };
        wg_random(ps[i].p.pub, 32);
        wg_pubtab_put(&pt, &ps[i]);
        for (j = 0; j < 3; j++)
            wg_idx_alloc(&ix, &ps[i].kp[j], &idxs[j]);
        memcpy(a6 + 12, a4, 4);
        wg_aip_insert(&aip, 0, a4, 32, &ps[i]);
        wg_aip_insert(&aip, 1, a6, 128, &ps[i]);
        ps[i].kp[0].local_index = idxs[0];
    }
    r1 = rss_mb();
    t0 = tcpu();
    for (i = 0; i < q; i++)
        sink += (uintptr_t)wg_pubtab_get(&pt, ps[(unsigned)i * 2654435761u % (unsigned)n].p.pub);
    tl[0] = (tcpu() - t0) / q;
    t0 = tcpu();
    for (i = 0; i < q; i++)
        sink += (uintptr_t)wg_idx_get(&ix, ps[(unsigned)i * 2654435761u % (unsigned)n].kp[0].local_index);
    tl[1] = (tcpu() - t0) / q;
    t0 = tcpu();
    for (i = 0; i < q; i++) {
        unsigned k = (unsigned)i * 2654435761u % (unsigned)n;
        uint8_t a4[4] = { 10, (uint8_t)(k >> 16), (uint8_t)(k >> 8), (uint8_t)k };
        sink += (uintptr_t)wg_aip_lookup(&aip, 0, a4);
    }
    tl[2] = (tcpu() - t0) / q;
    if (report) {
        size_t tab = (size_t)(pt.mask + 1) * sizeof(void*) + (size_t)(ix.mask + 1) * sizeof(wg_idx_slot_t) + aip.nodes * sizeof(wg_aip_node_t);
        printf("peers %6d: struct %zu B/peer, tables %.0f B/peer (aip nodes %zu), RSS +%.2f MB = %.0f B/peer; lookup ns: pubkey %.0f index %.0f allowed-ip %.0f\n", n, sizeof(bpeer_t), (double)tab / n, aip.nodes, r1 - (base ? base : r0), (r1 - (base ? base : r0)) * 1048576.0 / n, tl[0], tl[1], tl[2]);
    }
    wg_aip_free(&aip);
    wg_idx_free(&ix);
    wg_pubtab_free(&pt);
    free(ps);
    (void)sink;
    return r1;
}

static void bench_memory(void)
{
    int n;
    double base = rss_mb(), r;
    for (n = 16; n <= 65536; n *= 4)
        build(n, 1, base);
    printf("teardown cycles at 65536 peers, RSS after each build:");
    for (n = 0; n < 20; n++) {
        r = build(65536, 0, base);
        if (n % 4 == 0 || n == 19)
            printf(" %d:%.1fMB", n, r);
    }
    printf("\n");
}

static void bench_packets(void)
{
    static jb_t jb[64];
    dyn_pool_t* p = dyn_pool_new(4, 256);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    uint8_t key[32];
    uint64_t ctr = 0;
    long n;
    int i;
    double r0;
    wg_random(key, 32);
    for (i = 0; i < 64; i++)
        prep(&jb[i], 64, key, &ctr);
    r0 = rss_mb();
    for (n = 16; n <= 1048576; n *= 4) {
        long jobs = (n + 63) / 64, sent = 0;
        double w0 = twall();
        while (sent < jobs) {
            while (sent < jobs && dyn_pool_inflight(ch) < 64) {
                jb_t* j = &jb[sent % 64];
                j->b.n = (uint16_t)(n - sent * 64 < 64 ? n - sent * 64 : 64);
                if (dyn_pool_submit_cpu(ch, wg_batch_work, jdone_fn, &j->b) != 0)
                    break;
                sent++;
            }
            {
                struct pollfd pf = { dyn_pool_wake_fd(ch), POLLIN, 0 };
                poll(&pf, 1, 50);
            }
            dyn_pool_drain(ch);
        }
        wait_all(ch);
        printf("packets %8ld x 1420 B, 4 workers: %.0f ns/pkt wall, RSS growth %+.2f MB\n", n, (twall() - w0) / n, rss_mb() - r0);
    }
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
}

int main(int argc, char** argv)
{
    const char* only = argc > 1 ? argv[1] : "all";
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(only, "all") || !strcmp(only, "hs"))
        bench_handshake();
    if (!strcmp(only, "all") || !strcmp(only, "aead"))
        bench_aead();
    if (!strcmp(only, "all") || !strcmp(only, "pool"))
        bench_pool();
    if (!strcmp(only, "all") || !strcmp(only, "mem"))
        bench_memory();
    if (!strcmp(only, "all") || !strcmp(only, "pkts"))
        bench_packets();
    printf("peak RSS %.1f MB\n", peak_mb());
    return 0;
}
