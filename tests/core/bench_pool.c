/* dyn-pool benchmark (rule 11): links only the dyn-pool.h API, or libuv with -DBENCH_UV, so the old pool, the
 * new one and libuv run the same harness. `tp io|cpu [len_us] [threads]`: jobs/s, submit-to-done p50/p99 and CPU
 * per job beyond the work for job lengths 0/1/10/1000 us, 1/2/4/8/10 threads, 1 and 4 loop threads (BENCH_MUL
 * scales the job count). `mem`: peak RSS per pool thread, bytes per queued job over N=16..1M with queue_cap=N, 20
 * repeated 64Ki fill/drain cycles (plateau) and bytes per idle channel. `sparse threads gap_us count`: CPU per
 * job when lone jobs arrive gap_us apart (the cost of spinning before parking); SPARSE_IDLE=n adds n idle channels
 * (scan cost). BENCH_NOTIFY=fd (default: poll the wake fd), kq (macOS: dyn_pool_chan_new_notify triggering an
 * EVFILT_USER on the loop's kqueue, the path dyna-aio uses) or spin (no syscall: the loop busy-drains; the floor).
 * cc -O2 -std=gnu17 -Isrc/core tests/core/bench_pool.c src/core/dyn-pool.c -lpthread */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <poll.h>
#ifdef __APPLE__
#include <sys/event.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "../lib/dyn-xalloc.h"

#ifdef BENCH_UV
#include <uv.h>
#else
#include "dyn-pool.h"
#endif

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static double cpu_s(void)
{
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

static long ctxsw(void)
{
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_nvcsw + r.ru_nivcsw;
}

static long maxrss_bytes(void)
{
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
    return r.ru_maxrss;
#else
    return r.ru_maxrss * 1024L;
#endif
}

static unsigned g_len_us;
static int g_cpu;

typedef struct {
    uint64_t t_submit;
    uint32_t idx;
    void* lp;
} job_t;

typedef struct {
    int id;
    unsigned njobs;
    unsigned done;
    unsigned submitted;
    uint32_t* lat;
    job_t* jobs;
    unsigned* free_stack;
    unsigned nfree;
#ifdef BENCH_UV
    uv_loop_t loop;
    uv_work_t* reqs;
#else
    void* chan;
    int kq;
#endif
} loop_t;

static int g_notify;

static void spin_work(void* arg)
{
    (void)arg;
    if (g_len_us) {
        uint64_t end = now_ns() + (uint64_t)g_len_us * 1000u;
        while (now_ns() < end)
            ;
    }
}

static void on_done(job_t* j)
{
    loop_t* L = (loop_t*)j->lp;
    uint64_t d = now_ns() - j->t_submit;
    L->lat[L->done++] = d > 0xffffffffu ? 0xffffffffu : (uint32_t)d;
    L->free_stack[L->nfree++] = j->idx;
}

#ifdef BENCH_UV
static void bu_work(uv_work_t* r) { spin_work(r->data); }
static void bu_after(uv_work_t* r, int status)
{
    (void)status;
    on_done((job_t*)r->data);
}
#else
static void pool_done(void* arg) { on_done((job_t*)arg); }
#endif

static unsigned g_window;

#ifndef BENCH_UV
static void bench_notify(void* ctx)
{
    loop_t* L = (loop_t*)ctx;
#ifdef __APPLE__
    if (L->kq >= 0) {
        struct kevent kev;
        EV_SET(&kev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        kevent(L->kq, &kev, 1, NULL, 0, NULL);
    }
#else
    (void)L;
#endif
}
#endif

static void* loop_main(void* arg)
{
    loop_t* L = (loop_t*)arg;
#ifdef BENCH_UV
    while (L->done < L->njobs) {
        while (L->submitted < L->njobs && L->nfree > 0 && L->submitted - L->done < g_window) {
            unsigned i = L->free_stack[--L->nfree];
            job_t* j = &L->jobs[i];
            j->t_submit = now_ns();
            L->reqs[i].data = j;
            uv_queue_work(&L->loop, &L->reqs[i], bu_work, bu_after);
            L->submitted++;
        }
        uv_run(&L->loop, UV_RUN_ONCE);
    }
#else
    dyn_pool_chan_t* ch = (dyn_pool_chan_t*)L->chan;
    struct pollfd pf;
#ifdef __APPLE__
    struct kevent kev;
    struct timespec tmo = { 0, 100000000 };
#endif
    pf.fd = dyn_pool_wake_fd(ch);
    pf.events = POLLIN;
    while (L->done < L->njobs) {
        while (L->submitted < L->njobs && L->nfree > 0 && L->submitted - L->done < g_window) {
            unsigned i = L->free_stack[L->nfree - 1];
            job_t* j = &L->jobs[i];
            int rc;
            j->t_submit = now_ns();
            rc = g_cpu ? dyn_pool_submit_cpu(ch, spin_work, pool_done, j) : dyn_pool_submit(ch, spin_work, pool_done, j);
            if (rc != 0)
                break;
            L->nfree--;
            L->submitted++;
        }
#ifdef __APPLE__
        if (g_notify == 1)
            kevent(L->kq, NULL, 0, &kev, 1, &tmo);
        else
#endif
        if (g_notify == 0)
            poll(&pf, 1, 100);
        dyn_pool_drain(ch);
    }
#endif
    return NULL;
}

static int cmp_u32(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void run_throughput(unsigned threads, unsigned nloops, unsigned len_us, unsigned total)
{
    loop_t* L = calloc(nloops, sizeof(loop_t));
    pthread_t* th = calloc(nloops, sizeof(pthread_t));
    unsigned per = total / nloops, i, k, n = 0;
    uint32_t* all;
    uint64_t t0, t1;
    double c0, c1;
#ifndef BENCH_UV
    dyn_pool_t* p = dyn_pool_new(threads, 0);
    unsigned real = dyn_pool_threads(p);
    g_window = dyn_pool_capacity(p);
#else
    unsigned real = threads;
    g_window = threads * 4u < 8u ? 8u : threads * 4u;
#endif
    g_len_us = len_us;
    for (i = 0; i < nloops; i++) {
        L[i].id = (int)i;
        L[i].njobs = per;
        L[i].lat = calloc(per, sizeof(uint32_t));
        L[i].jobs = xcalloc(g_window, sizeof(job_t));
        L[i].free_stack = xcalloc(g_window, sizeof(unsigned));
        for (k = 0; k < g_window; k++) {
            L[i].jobs[k].idx = k;
            L[i].jobs[k].lp = &L[i];
            L[i].free_stack[k] = g_window - 1 - k;
        }
        L[i].nfree = g_window;
#ifdef BENCH_UV
        uv_loop_init(&L[i].loop);
        L[i].reqs = calloc(g_window, sizeof(uv_work_t));
#else
        L[i].kq = -1;
#ifdef __APPLE__
        if (g_notify == 1) {
            struct kevent kev;
            L[i].kq = kqueue();
            EV_SET(&kev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
            kevent(L[i].kq, &kev, 1, NULL, 0, NULL);
        }
#endif
        L[i].chan = g_notify == 0 ? dyn_pool_chan_new(p) : dyn_pool_chan_new_notify(p, bench_notify, &L[i]);
#endif
    }
    long x0 = ctxsw(), x1;
    c0 = cpu_s();
    t0 = now_ns();
    for (i = 0; i < nloops; i++)
        pthread_create(&th[i], NULL, loop_main, &L[i]);
    for (i = 0; i < nloops; i++)
        pthread_join(th[i], NULL);
    t1 = now_ns();
    c1 = cpu_s();
    x1 = ctxsw();
    all = malloc((size_t)per * nloops * sizeof(uint32_t));
    for (i = 0; i < nloops; i++) {
        memcpy(all + n, L[i].lat, per * sizeof(uint32_t));
        n += per;
    }
    qsort(all, n, sizeof(uint32_t), cmp_u32);
    {
        double secs = (t1 - t0) / 1e9;
        double cpu_per = (c1 - c0) / n * 1e6 - (double)len_us;
        printf("%s %s len=%uus threads=%u loops=%u jobs=%u: %.0f jobs/s p50 %.1f us p99 %.1f us cpu/job %.2f us (overhead beyond the work)\n",
#ifdef BENCH_UV
            "libuv",
#else
            "dyn",
#endif
            g_cpu ? "cpu" : "io", len_us, real, nloops, n, n / secs, all[n / 2] / 1000.0, all[(size_t)n * 99 / 100] / 1000.0, cpu_per);
        printf("  ctxsw/job %.3f\n", (double)(x1 - x0) / n);
#ifdef BENCH_STATS
        {
            dyn_pool_stats_t st;
            dyn_pool_stats(p, &st);
            printf("  stats parks/job %.3f spare_kicks %llu spare_parks %llu e_spawned %llu overflowed %llu\n", (double)st.parks / n, st.spare_kicks, st.spare_parks, st.blocking_spawned, st.overflowed);
        }
#endif
    }
    for (i = 0; i < nloops; i++) {
#ifdef BENCH_UV
        uv_loop_close(&L[i].loop);
        free(L[i].reqs);
#else
        dyn_pool_chan_free((dyn_pool_chan_t*)L[i].chan);
        if (L[i].kq >= 0)
            close(L[i].kq);
#endif
        free(L[i].lat);
        free(L[i].jobs);
        free(L[i].free_stack);
    }
#ifndef BENCH_UV
    dyn_pool_free(p);
#endif
    free(all);
    free(L);
    free(th);
}

#ifndef BENCH_UV
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open;
static void gate_work(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&gate_mu);
    while (!gate_open)
        pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}
static void noop_done(void* arg) { (void)arg; }

static void run_memory(void)
{
    unsigned sizes[] = { 16, 256, 4096, 65536, 1048576 };
    unsigned s, r;
    {
        long t0 = maxrss_bytes();
        dyn_pool_t* tp = dyn_pool_new(0, 0);
        dyn_pool_chan_t* tc = dyn_pool_chan_new(tp);
        unsigned left = 4000, k;
        g_len_us = 2;
        while (left > 0) {
            struct pollfd pf = { dyn_pool_wake_fd(tc), POLLIN, 0 };
            for (k = 0; k < 16 && left > 0 && dyn_pool_submit_cpu(tc, spin_work, noop_done, NULL) == 0; k++)
                left--;
            poll(&pf, 1, 50);
            dyn_pool_drain(tc);
        }
        while (dyn_pool_inflight(tc) > 0) {
            struct pollfd pf = { dyn_pool_wake_fd(tc), POLLIN, 0 };
            poll(&pf, 1, 50);
            dyn_pool_drain(tc);
        }
        printf("threads: default pool of %u threads after 4000 jobs: +%.1f KB peak RSS, %.1f KB per thread\n", dyn_pool_threads(tp), (maxrss_bytes() - t0) / 1024.0, (maxrss_bytes() - t0) / 1024.0 / dyn_pool_threads(tp));
        dyn_pool_chan_free(tc);
        dyn_pool_free(tp);
        g_len_us = 0;
    }
    printf("memory: queued jobs held behind a closed gate, pool queue_cap = N, 1 chan\n");
    for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        unsigned N = sizes[s], i, ok = 0;
        long r0 = maxrss_bytes();
        dyn_pool_t* p = dyn_pool_new(4, N);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        long r1;
        gate_open = 0;
        for (i = 0; i < N; i++)
            ok += dyn_pool_submit(ch, gate_work, noop_done, NULL) == 0;
        r1 = maxrss_bytes();
        pthread_mutex_lock(&gate_mu);
        gate_open = 1;
        pthread_cond_broadcast(&gate_cv);
        pthread_mutex_unlock(&gate_mu);
        while (dyn_pool_inflight(ch) > 0) {
            struct pollfd pf = { dyn_pool_wake_fd(ch), POLLIN, 0 };
            poll(&pf, 1, 50);
            dyn_pool_drain(ch);
        }
        {
            long r2 = maxrss_bytes();
            dyn_pool_chan_free(ch);
            dyn_pool_free(p);
            printf("  N=%u queued=%u: +%.1f MB queued (%.1f bytes/job), peak through drain +%.1f MB (%.1f bytes/job)\n", N, ok, (r1 - r0) / 1048576.0, ok ? (double)(r1 - r0) / ok : 0.0, (r2 - r0) / 1048576.0, ok ? (double)(r2 - r0) / ok : 0.0);
        }
    }
    printf("repeat: 20 x (pool cap 65536, 1 chan, 65536 queued, drain, free)\n  peak RSS after each 5:");
    for (r = 0; r < 20; r++) {
        unsigned N = 65536, i;
        dyn_pool_t* p = dyn_pool_new(4, N);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        gate_open = 1;
        for (i = 0; i < N; i++)
            dyn_pool_submit(ch, NULL, noop_done, NULL);
        while (dyn_pool_inflight(ch) > 0) {
            struct pollfd pf = { dyn_pool_wake_fd(ch), POLLIN, 0 };
            poll(&pf, 1, 50);
            dyn_pool_drain(ch);
        }
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
        if (r % 5 == 4)
            printf(" %.1f", maxrss_bytes() / 1048576.0);
    }
    printf(" MB\n");
    {
        long r0 = maxrss_bytes();
        dyn_pool_t* p = dyn_pool_new(8, 0);
        dyn_pool_chan_t* chs[256];
        for (r = 0; r < 256; r++)
            chs[r] = dyn_pool_chan_new(p);
        printf("idle chans: 256 chans on a default pool: +%.1f KB, %.0f bytes/chan\n", (maxrss_bytes() - r0) / 1024.0, (double)(maxrss_bytes() - r0) / 256);
        for (r = 0; r < 256; r++)
            dyn_pool_chan_free(chs[r]);
        dyn_pool_free(p);
    }
}
#endif

#ifndef BENCH_UV
static void run_sparse(unsigned threads, unsigned gap_us, unsigned count)
{
    dyn_pool_t* p = dyn_pool_new(threads, 0);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    unsigned i, done = 0;
    dyn_pool_chan_t* idle[256];
    unsigned nidle_ch = getenv("SPARSE_IDLE") ? (unsigned)atoi(getenv("SPARSE_IDLE")) : 0;
    if (nidle_ch > 256)
        nidle_ch = 256;
    for (i = 0; i < nidle_ch; i++)
        idle[i] = dyn_pool_chan_new(p);
    double c0, c1;
    uint64_t t0;
    usleep(50000);
    c0 = cpu_s();
    t0 = now_ns();
    for (i = 0; i < count; i++) {
        struct pollfd pf = { dyn_pool_wake_fd(ch), POLLIN, 0 };
        dyn_pool_submit(ch, NULL, noop_done, NULL);
        while (dyn_pool_inflight(ch) > 0) {
            poll(&pf, 1, 100);
            dyn_pool_drain(ch);
        }
        done++;
        usleep(gap_us);
    }
    c1 = cpu_s();
    printf("sparse threads=%u gap=%uus jobs=%u: cpu/job %.2f us (including the loop thread; wall %.0f ms)\n", dyn_pool_threads(p), gap_us, done, (c1 - c0) / done * 1e6, (now_ns() - t0) / 1e6);
    for (i = 0; i < nidle_ch; i++)
        dyn_pool_chan_free(idle[i]);
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
}
#endif

int main(int argc, char** argv)
{
    unsigned lens[] = { 0, 1, 10, 1000 };
    unsigned threads[] = { 1, 2, 4, 8, 10 };
    unsigned loops[] = { 1, 4 };
    unsigned a, b, c;
    const char* mode = argc > 1 ? argv[1] : "all";
    const char* nt = getenv("BENCH_NOTIFY");
    g_notify = nt && !strcmp(nt, "kq") ? 1 : nt && !strcmp(nt, "spin") ? 2 : 0;
#ifdef BENCH_UV
    (void)mode;
#else
    if (!strcmp(mode, "mem")) {
        run_memory();
        return 0;
    }
    if (!strcmp(mode, "sparse")) {
        run_sparse(argc > 2 ? (unsigned)atoi(argv[2]) : 8, argc > 3 ? (unsigned)atoi(argv[3]) : 1000, argc > 4 ? (unsigned)atoi(argv[4]) : 1000);
        return 0;
    }
#endif
    if (argc > 2)
        g_cpu = !strcmp(argv[2], "cpu");
    for (a = 0; a < 4; a++)
        for (b = 0; b < 5; b++)
            for (c = 0; c < 2; c++) {
                unsigned len = lens[a], total = len >= 1000 ? 2000 : len >= 10 ? 40000 : 200000;
                if (getenv("BENCH_MUL"))
                    total *= (unsigned)atoi(getenv("BENCH_MUL"));
                if (argc > 3 && argv[3][0] && (unsigned)atoi(argv[3]) != len)
                    continue;
                if (argc > 4 && argv[4][0] && (unsigned)atoi(argv[4]) != threads[b])
                    continue;
                run_throughput(threads[b], loops[c], len, total);
            }
    return 0;
}
