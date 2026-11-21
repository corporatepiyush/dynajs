#define _GNU_SOURCE
#include "dyn-pool.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../lib/dyn-xalloc.h"

#ifndef POOL_CPU_CAP_MIN
#define POOL_CPU_CAP_MIN 8u
#endif

static _Atomic int fails;
#define CHECK(c, ...)                                     \
    do {                                                  \
        if (!(c)) {                                       \
            if (atomic_fetch_add(&fails, 1) < 20) {       \
                printf("FAIL: " __VA_ARGS__);             \
                printf("\n");                             \
            }                                             \
        }                                                 \
    } while (0)

typedef struct {
    uint64_t s;
} rng_t;

static uint64_t rnd(rng_t* r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static unsigned rn(rng_t* r, unsigned n)
{
    return (unsigned)(rnd(r) % n);
}

enum { K_NOP, K_SPIN, K_SLEEP, K_YIELD, K_BLOCK, K_HOLD, K_N };

struct fchan;

typedef struct {
    struct fchan* fc;
    int cls;
    int kind;
    int cancelled;
    int orphan;
    _Atomic int ran;
    _Atomic int dead;
    int done;
} job_t;

typedef struct fchan {
    dyn_pool_chan_t* ch;
    pthread_t owner;
    size_t pending;
    job_t** live;
    size_t nlive, caplive;
} fchan_t;

typedef struct {
    dyn_pool_t* p;
    unsigned nthreads;
    unsigned cpu_bound;
    uint64_t seed;
    unsigned ops;
    _Atomic int cpu_now, cpu_max, all_now, all_max;
    _Atomic long submitted, completed, cancels, frees;
} round_t;

static round_t* g_r;

static void gauge_up(_Atomic int* now, _Atomic int* mx)
{
    int v = atomic_fetch_add(now, 1) + 1, m = atomic_load(mx);
    while (v > m && !atomic_compare_exchange_weak(mx, &m, v))
        ;
}

static void w_job(void* arg)
{
    job_t* j = (job_t*)arg;
    CHECK(!atomic_load(&j->dead), "work runs after its channel was freed");
    CHECK(!j->cancelled, "work runs for a job dyn_pool_cancel reported cancelled");
    if (j->cls)
        gauge_up(&g_r->cpu_now, &g_r->cpu_max);
    gauge_up(&g_r->all_now, &g_r->all_max);
    CHECK(atomic_fetch_add(&j->ran, 1) == 0, "work runs twice for one job");
    switch (j->kind) {
    case K_SPIN: {
        volatile unsigned k;
        for (k = 0; k < 2000; k++)
            ;
        break;
    }
    case K_SLEEP:
        usleep(100 + (unsigned)((uintptr_t)j % 300u));
        break;
    case K_YIELD:
        sched_yield();
        break;
    case K_BLOCK:
        usleep(2500);
        break;
    case K_HOLD: {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        do
            clock_gettime(CLOCK_MONOTONIC, &b);
        while ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) < 400000L);
        break;
    }
    default:
        break;
    }
    atomic_fetch_sub(&g_r->all_now, 1);
    if (j->cls)
        atomic_fetch_sub(&g_r->cpu_now, 1);
}

static void d_job(void* arg)
{
    job_t* j = (job_t*)arg;
    CHECK(pthread_equal(pthread_self(), j->fc->owner), "done runs off the submitting thread");
    CHECK(!j->orphan, "done runs for a job whose channel was freed");
    CHECK(j->done == 0, "done runs twice for one job");
    CHECK(j->cancelled || atomic_load(&j->ran) == 1, "done before work for a job not cancelled");
    j->done++;
    j->fc->pending--;
    atomic_fetch_add(&g_r->completed, 1);
}

static void fc_track(fchan_t* fc, job_t* j)
{
    if (fc->nlive == fc->caplive) {
        fc->caplive = fc->caplive ? fc->caplive * 2 : 64;
        fc->live = xrealloc(fc->live, fc->caplive * sizeof(job_t*));
    }
    fc->live[fc->nlive++] = j;
}

static int fc_drain(fchan_t* fc, int ms)
{
    while (fc->pending && ms > 0) {
        struct pollfd pfd;
        int before = (int)fc->pending;
        pfd.fd = dyn_pool_wake_fd(fc->ch);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 5) > 0)
            dyn_pool_drain(fc->ch);
        else
            ms -= 5;
        if ((int)fc->pending == before && pfd.revents)
            dyn_pool_drain(fc->ch);
    }
    return fc->pending == 0;
}

static void fc_free(fchan_t* fc, int drain_first)
{
    size_t i;
    if (drain_first)
        CHECK(fc_drain(fc, 10000), "channel drains within 10 s (pending %zu)", fc->pending);
    dyn_pool_chan_free(fc->ch);
    for (i = 0; i < fc->nlive; i++) {
        job_t* j = fc->live[i];
        if (!j->done) {
            j->orphan = 1;
            CHECK(atomic_load(&j->ran) <= 1, "orphan ran more than once");
        } else {
            CHECK(j->cancelled ? atomic_load(&j->ran) == 0 : atomic_load(&j->ran) == 1, "finished job ran %d times (cancelled %d)", atomic_load(&j->ran), j->cancelled);
        }
        atomic_store(&j->dead, 1);
    }
    for (i = 0; i < fc->nlive; i++)
        free(fc->live[i]);
    free(fc->live);
    memset(fc, 0, sizeof(*fc));
    atomic_fetch_add(&g_r->frees, 1);
}

typedef struct {
    round_t* r;
    uint64_t seed;
} prod_t;

#define NCH 4

static void* producer(void* arg)
{
    prod_t* pa = (prod_t*)arg;
    round_t* r = pa->r;
    rng_t g = { pa->seed | 1u };
    fchan_t fc[NCH];
    unsigned op, i;
    memset(fc, 0, sizeof(fc));
    for (op = 0; op < r->ops; op++) {
        fchan_t* c = &fc[rn(&g, NCH)];
        unsigned a = rn(&g, 100);
        if (!c->ch) {
            c->ch = dyn_pool_chan_new(r->p);
            c->owner = pthread_self();
            CHECK(c->ch != NULL, "chan_new");
            if (!c->ch)
                continue;
        }
        if (a < 55) {
            job_t* j = xcalloc(1, sizeof(*j));
            int rc;
            j->fc = c;
            j->cls = (int)rn(&g, 2);
            j->kind = (int)rn(&g, K_N);
            if ((j->kind == K_SLEEP && rn(&g, 4)) || (j->kind == K_BLOCK && (j->cls || rn(&g, 16))) || (j->kind == K_HOLD && !j->cls))
                j->kind = K_NOP;
            fc_track(c, j);
            rc = j->cls ? dyn_pool_submit_cpu(c->ch, w_job, d_job, j) : dyn_pool_submit(c->ch, w_job, d_job, j);
            CHECK(rc == 0, "submit refused (rc %d errno %d): jobs past capacity must wait, not fail", rc, errno);
            if (rc == 0) {
                c->pending++;
                atomic_fetch_add(&r->submitted, 1);
            }
        } else if (a < 70) {
            if (c->nlive) {
                job_t* j = c->live[rn(&g, (unsigned)c->nlive)];
                if (!j->done && !j->cancelled && dyn_pool_cancel(c->ch, w_job, j) == 1) {
                    j->cancelled = 1;
                    atomic_fetch_add(&r->cancels, 1);
                }
            }
        } else if (a < 88) {
            dyn_pool_drain(c->ch);
        } else if (a < 92) {
            CHECK(dyn_pool_inflight(c->ch) >= c->pending, "inflight %zu below model pending %zu", dyn_pool_inflight(c->ch), c->pending);
        } else if (a < 96) {
            (void)dyn_pool_cpu_room(c->ch);
            fc_drain(c, (int)rn(&g, 20));
        } else {
            fc_free(c, a < 99);
        }
    }
    for (i = 0; i < NCH; i++)
        if (fc[i].ch)
            fc_free(&fc[i], 1);
    return NULL;
}

static unsigned env_u(const char* k, unsigned d)
{
    const char* v = getenv(k);
    return v && *v ? (unsigned)strtoul(v, NULL, 0) : d;
}

static void run_round(uint64_t seed, unsigned ops)
{
    static const unsigned threads[] = { 1, 2, 3, 4, 8, 9 };
    static const unsigned caps[] = { 2, 8, 64 };
    rng_t g = { seed | 1u };
    round_t r;
    pthread_t th[4];
    prod_t pa[4];
    unsigned np = 1 + rn(&g, 4), i;
    dyn_pool_stats_t st;
    memset(&r, 0, sizeof(r));
    r.nthreads = threads[rn(&g, 6)];
    r.seed = seed;
    r.ops = ops;
    r.p = dyn_pool_new(r.nthreads, caps[rn(&g, 3)]);
    CHECK(r.p != NULL, "pool_new");
    if (!r.p)
        return;
    r.nthreads = dyn_pool_threads(r.p);
    g_r = &r;
    for (i = 0; i < np; i++) {
        pa[i].r = &r;
        pa[i].seed = rnd(&g);
        pthread_create(&th[i], NULL, producer, &pa[i]);
    }
    for (i = 0; i < np; i++)
        pthread_join(th[i], NULL);
    dyn_pool_stats(r.p, &st);
    CHECK(atomic_load(&r.cpu_max) <= (int)(r.nthreads >= POOL_CPU_CAP_MIN ? r.nthreads : r.nthreads + 1u), "seed %llu: %d cpu jobs ran at once on %u threads (cap N-1 + spare, or N under the cap minimum)", (unsigned long long)seed, atomic_load(&r.cpu_max), r.nthreads);
    CHECK(st.io_running == 0 && st.cpu_running == 0, "seed %llu: idle pool holds io_running %u cpu_running %u (a reservation leaked)", (unsigned long long)seed, st.io_running, st.cpu_running);
    CHECK(atomic_load(&r.all_now) == 0 && atomic_load(&r.cpu_now) == 0, "gauges return to zero");
    dyn_pool_free(r.p);
    if (getenv("DYN_FUZZ_VERBOSE"))
        printf("  seed %llu: threads %u producers %u submitted %ld completed %ld cancels %ld frees %ld cpu_max %d all_max %d\n",
            (unsigned long long)seed, r.nthreads, np, atomic_load(&r.submitted), atomic_load(&r.completed),
            atomic_load(&r.cancels), atomic_load(&r.frees), atomic_load(&r.cpu_max), atomic_load(&r.all_max));
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_open;
static _Atomic int g_in, g_marks, g_dn;

static void w_gate(void* arg)
{
    (void)arg;
    atomic_fetch_add(&g_in, 1);
    pthread_mutex_lock(&g_mu);
    while (!g_open)
        pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

static void w_markj(void* arg)
{
    atomic_fetch_add((_Atomic int*)arg, 1);
    atomic_fetch_add(&g_marks, 1);
}

static void d_cnt(void* arg)
{
    (void)arg;
    atomic_fetch_add(&g_dn, 1);
}

static void gate_set(int open)
{
    pthread_mutex_lock(&g_mu);
    g_open = open;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static dyn_pool_chan_t* g_wch;

static int wait_for(_Atomic int* v, int want, int ms)
{
    while (atomic_load(v) < want && ms > 0) {
        if (g_wch)
            dyn_pool_drain(g_wch);
        usleep(1000);
        ms--;
    }
    return atomic_load(v);
}

static int drain_cnt(dyn_pool_chan_t* ch, int want, int ms)
{
    while (atomic_load(&g_dn) < want && ms > 0) {
        struct pollfd pfd;
        pfd.fd = dyn_pool_wake_fd(ch);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 5) > 0)
            dyn_pool_drain(ch);
        else
            ms -= 5;
    }
    return atomic_load(&g_dn);
}

static void gated_round(uint64_t seed)
{
    static const unsigned threads[] = { 1, 2, 3, 4, 8, 9 };
    rng_t g = { seed | 1u };
    unsigned n = threads[rn(&g, 6)], cap = 2 + rn(&g, 15), k = 1 + rn(&g, 3 * cap), i, held, lanes, total = 0;
    dyn_pool_t* p = dyn_pool_new(n, cap);
    dyn_pool_chan_t* ch = p ? dyn_pool_chan_new(p) : NULL;
    _Atomic int* hit = calloc(k + 2, sizeof(_Atomic int));
    unsigned char* cx = calloc(k + 2, 1);
    _Atomic int io_hit = 0, cpu_hit = 0;
    int c, ok;
    CHECK(p && ch && hit && cx, "gated: pool + chan");
    if (!p || !ch || !hit || !cx)
        goto out;
    n = dyn_pool_threads(p);
    atomic_store(&g_in, 0);
    atomic_store(&g_dn, 0);
    atomic_store(&g_marks, 0);
    gate_set(0);
    g_wch = ch;
    cap = dyn_pool_capacity(p);
    lanes = n < cap ? n : cap;
    held = lanes + 1;
    for (i = 0; i < held; i++)
        dyn_pool_submit_cpu(ch, w_gate, d_cnt, NULL);
    total += held;
    c = wait_for(&g_in, (int)lanes, 3000);
    CHECK(c == (int)lanes, "seed %llu: %u gated cpu jobs fill all %u lanes (%u threads, cap %u) (%d)", (unsigned long long)seed, held, lanes, n, cap, c);
    for (i = 0; i < 20; i++) {
        dyn_pool_drain(ch);
        usleep(1000);
    }
    CHECK(atomic_load(&g_in) == (int)lanes, "seed %llu: no cpu job past %u lanes starts (%d)", (unsigned long long)seed, lanes, atomic_load(&g_in));
    for (i = 0; i < k; i++) {
        CHECK(dyn_pool_submit_cpu(ch, w_markj, d_cnt, (void*)&hit[i]) == 0, "gated: submit past capacity waits, never fails");
        total++;
    }
    for (i = 0; i < k; i++)
        if (rn(&g, 2)) {
            ok = dyn_pool_cancel(ch, w_markj, (void*)&hit[i]);
            CHECK(ok == 1, "seed %llu: cancel of queued cpu job %u/%u (cap %u, %s) must succeed (%d)", (unsigned long long)seed, i, k, cap, i < cap ? "ring" : "overflow", ok);
            cx[i] = ok == 1;
        }
    if (n >= POOL_CPU_CAP_MIN) {
        dyn_pool_submit(ch, w_markj, d_cnt, (void*)&io_hit);
        total++;
        c = wait_for(&io_hit, 1, 2000);
        CHECK(c == 1, "seed %llu: an io job runs while cpu work holds every lane of a %u-thread pool", (unsigned long long)seed, n);
    }
    gate_set(1);
    c = drain_cnt(ch, (int)total, 10000);
    CHECK(c == (int)total, "seed %llu: every gated-phase job completes (%d/%u)", (unsigned long long)seed, c, total);
    for (i = 0; i < k; i++)
        CHECK(atomic_load(&hit[i]) == (cx[i] ? 0 : 1), "seed %llu: job %u ran %d times (cancelled %d)", (unsigned long long)seed, i, atomic_load(&hit[i]), cx[i]);
    atomic_store(&g_in, 0);
    atomic_store(&g_dn, 0);
    gate_set(0);
    held = n + cap + rn(&g, 2 * cap);
    for (i = 0; i < held; i++)
        dyn_pool_submit(ch, w_gate, d_cnt, NULL);
    total = held;
    c = wait_for(&g_in, (int)lanes, 3000);
    CHECK(c >= (int)lanes, "seed %llu: gated io fills %u lanes (%d)", (unsigned long long)seed, lanes, c);
    dyn_pool_submit_cpu(ch, w_markj, d_cnt, (void*)&cpu_hit);
    total++;
    c = wait_for(&cpu_hit, 1, 2000);
    CHECK(c == 1, "seed %llu: a cpu job runs while %u io jobs hold and overflow a cap-%u channel", (unsigned long long)seed, held, cap);
    gate_set(1);
    c = drain_cnt(ch, (int)total, 15000);
    CHECK(c == (int)total, "seed %llu: every io-gated job completes (%d/%u)", (unsigned long long)seed, c, total);
out:
    gate_set(1);
    g_wch = NULL;
    if (ch)
        dyn_pool_chan_free(ch);
    if (p)
        dyn_pool_free(p);
    free(hit);
    free(cx);
}

int main(void)
{
    const char* sv = getenv("DYN_FUZZ_SEED");
    uint64_t seed = sv && *sv ? strtoull(sv, NULL, 0) : (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 20);
    unsigned rounds = env_u("DYN_FUZZ_ROUNDS", 24), ops = env_u("DYN_FUZZ_OPS", 4000), i;
    rng_t g = { seed | 1u };
    printf("test_pool_fuzz: seed %llu rounds %u ops %u (replay: DYN_FUZZ_SEED=%llu)\n",
        (unsigned long long)seed, rounds, ops, (unsigned long long)seed);
    for (i = 0; i < rounds && !atomic_load(&fails); i++) {
        uint64_t rs = rnd(&g);
        run_round(rs, ops);
        if (!atomic_load(&fails) && (i & 1u) == 0)
            gated_round(rnd(&g));
        if (atomic_load(&fails))
            printf("  failing round seed %llu (DYN_FUZZ_SEED=%llu replays all rounds)\n", (unsigned long long)rs, (unsigned long long)seed);
    }
    if (!atomic_load(&fails))
        printf("test_pool_fuzz: all %u rounds passed\n", rounds);
    return atomic_load(&fails) != 0;
}
