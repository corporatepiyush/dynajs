#include "dyn-par.h"
#include "dyn-pool.h"
#include "dyn-thread-name.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define PAR_MAX_WORKERS 64u
#define PAR_STACK_BYTES ((size_t)1 << 20)

struct dyn_par {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned n;
    unsigned arrived;
    unsigned gen;
    int started;
    dyn_par_fn fn;
    void* arg;
};

typedef struct {
    dyn_par_t* par;
    unsigned index;
} par_slot_t;

static _Atomic uint64_t par_runs;
static _Atomic uint64_t par_serial_runs;
static _Atomic uint64_t par_spawned;
static _Atomic uint64_t par_spawn_failures;
static _Atomic unsigned par_live;
static _Atomic unsigned par_live_peak;
static _Atomic int par_spawn_limit = -2;
static _Atomic unsigned par_cpu_cache;
static _Thread_local int par_in_worker;

static unsigned par_cpus(void)
{
    unsigned m = atomic_load_explicit(&par_cpu_cache, memory_order_relaxed);
    if (m == 0) {
        m = dyn_pool_max_threads();
        atomic_store_explicit(&par_cpu_cache, m, memory_order_relaxed);
    }
    return m;
}

static unsigned par_threads(void)
{
    unsigned d = dyn_pool_thread_override();
    unsigned m = par_cpus();
    return d && d < m ? d : m;
}

static unsigned par_budget(void)
{
    unsigned m = par_cpus();
    return m > 1 ? m - 1 : 0;
}

static unsigned par_reserve(unsigned want)
{
    unsigned budget = par_budget();
    unsigned cur = atomic_load_explicit(&par_live, memory_order_relaxed);
    unsigned take, peak;
    for (;;) {
        unsigned left = cur < budget ? budget - cur : 0;
        take = want < left ? want : left;
        if (take == 0)
            return 0;
        if (atomic_compare_exchange_weak_explicit(&par_live, &cur, cur + take,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }
    peak = atomic_load_explicit(&par_live_peak, memory_order_relaxed);
    while (cur + take > peak
        && !atomic_compare_exchange_weak_explicit(&par_live_peak, &peak,
            cur + take, memory_order_relaxed, memory_order_relaxed)) {
    }
    return take;
}

static void par_release(unsigned n)
{
    if (n)
        atomic_fetch_sub_explicit(&par_live, n, memory_order_relaxed);
}

unsigned dyn_par_plan(size_t units, size_t min_units_per_worker, unsigned cap)
{
    size_t n;
    unsigned limit;
    if (par_in_worker || dyn_pool_is_inline_only())
        return 1;
    if (min_units_per_worker == 0)
        min_units_per_worker = 1;
    n = units / min_units_per_worker;
    limit = par_threads();
    if (cap && cap < limit)
        limit = cap;
    if (limit > PAR_MAX_WORKERS)
        limit = PAR_MAX_WORKERS;
    if (n > limit)
        n = limit;
    return n < 1 ? 1u : (unsigned)n;
}

void dyn_par_barrier(dyn_par_t* par)
{
    unsigned gen;
    if (!par || par->n <= 1)
        return;
    pthread_mutex_lock(&par->mu);
    gen = par->gen;
    if (++par->arrived == par->n) {
        par->arrived = 0;
        par->gen++;
        pthread_cond_broadcast(&par->cv);
    } else {
        while (gen == par->gen)
            pthread_cond_wait(&par->cv, &par->mu);
    }
    pthread_mutex_unlock(&par->mu);
}

static void* par_helper(void* p)
{
    par_slot_t* slot = (par_slot_t*)p;
    dyn_par_t* par = slot->par;
    dyn_thread_name("dyn-par");
    unsigned n;
    pthread_mutex_lock(&par->mu);
    while (!par->started)
        pthread_cond_wait(&par->cv, &par->mu);
    n = par->n;
    pthread_mutex_unlock(&par->mu);
    par_in_worker = 1;
    par->fn(par->arg, slot->index, n, par);
    return NULL;
}

static int par_spawn_limit_now(void)
{
    int lim = atomic_load_explicit(&par_spawn_limit, memory_order_relaxed);
    if (lim == -2) {
        const char* e = getenv("DYNAJS_PAR_SPAWN_LIMIT");
        int bad = 0, unread = -2;
        lim = -1;
        if (e && *e) {
            const char* q = e;
            unsigned v = 0;
            while (*q >= '0' && *q <= '9' && v <= PAR_MAX_WORKERS)
                v = v * 10 + (unsigned)(*q++ - '0');
            if (*q == 0 && q - e <= 3 && v <= PAR_MAX_WORKERS)
                lim = (int)v;
            else
                bad = 1;
        }
        if (atomic_compare_exchange_strong_explicit(&par_spawn_limit, &unread, lim,
                memory_order_relaxed, memory_order_relaxed)) {
            if (bad)
                fprintf(stderr, "dynajs: DYNAJS_PAR_SPAWN_LIMIT=%.32s ignored (expected 0..%u)\n", e, PAR_MAX_WORKERS);
        } else {
            lim = unread;
        }
    }
    return lim;
}

static int par_spawn_allowed(unsigned created)
{
    int lim = par_spawn_limit_now();
    return lim < 0 || created < (unsigned)lim;
}

unsigned dyn_par_run(unsigned nworkers, dyn_par_fn fn, void* arg)
{
    dyn_par_t par;
    pthread_t threads[PAR_MAX_WORKERS];
    par_slot_t slots[PAR_MAX_WORKERS];
    pthread_attr_t attr;
    sigset_t all, old;
    unsigned reserved, created = 0, i;
    int attr_ok;

    atomic_fetch_add_explicit(&par_runs, 1, memory_order_relaxed);
    if (nworkers > PAR_MAX_WORKERS)
        nworkers = PAR_MAX_WORKERS;
    if (nworkers <= 1 || par_in_worker || dyn_pool_is_inline_only()) {
        memset(&par, 0, sizeof par);
        par.n = 1;
        atomic_fetch_add_explicit(&par_serial_runs, 1, memory_order_relaxed);
        fn(arg, 0, 1, &par);
        return 1;
    }
    reserved = par_reserve(nworkers - 1);
    if (reserved == 0) {
        memset(&par, 0, sizeof par);
        par.n = 1;
        atomic_fetch_add_explicit(&par_serial_runs, 1, memory_order_relaxed);
        fn(arg, 0, 1, &par);
        return 1;
    }

    memset(&par, 0, sizeof par);
    pthread_mutex_init(&par.mu, NULL);
    pthread_cond_init(&par.cv, NULL);
    par.fn = fn;
    par.arg = arg;

    attr_ok = pthread_attr_init(&attr) == 0;
    if (attr_ok)
        pthread_attr_setstacksize(&attr, PAR_STACK_BYTES);
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    for (i = 0; i < reserved; i++) {
        slots[i].par = &par;
        slots[i].index = i + 1;
        if (!par_spawn_allowed(created)
            || pthread_create(&threads[i], attr_ok ? &attr : NULL, par_helper,
                   &slots[i])
                != 0) {
            atomic_fetch_add_explicit(&par_spawn_failures, 1,
                memory_order_relaxed);
            break;
        }
        created++;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (attr_ok)
        pthread_attr_destroy(&attr);
    par_release(reserved - created);
    atomic_fetch_add_explicit(&par_spawned, created, memory_order_relaxed);
    if (created == 0)
        atomic_fetch_add_explicit(&par_serial_runs, 1, memory_order_relaxed);

    pthread_mutex_lock(&par.mu);
    par.n = created + 1;
    par.started = 1;
    pthread_cond_broadcast(&par.cv);
    pthread_mutex_unlock(&par.mu);

    par_in_worker = 1;
    fn(arg, 0, created + 1, &par);
    par_in_worker = 0;

    for (i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    par_release(created);
    pthread_cond_destroy(&par.cv);
    pthread_mutex_destroy(&par.mu);
    return created + 1;
}

void dyn_par_stats(dyn_par_stats_t* out)
{
    out->runs = atomic_load_explicit(&par_runs, memory_order_relaxed);
    out->serial_runs = atomic_load_explicit(&par_serial_runs, memory_order_relaxed);
    out->spawned = atomic_load_explicit(&par_spawned, memory_order_relaxed);
    out->spawn_failures = atomic_load_explicit(&par_spawn_failures, memory_order_relaxed);
    out->live = atomic_load_explicit(&par_live, memory_order_relaxed);
    out->live_peak = atomic_load_explicit(&par_live_peak, memory_order_relaxed);
    out->budget = par_budget();
}

void dyn_par_reset_peak(void)
{
    atomic_store_explicit(&par_live_peak,
        atomic_load_explicit(&par_live, memory_order_relaxed),
        memory_order_relaxed);
}

void dyn_par_set_spawn_limit(int limit)
{
    atomic_store_explicit(&par_spawn_limit, limit < 0 ? -1 : limit, memory_order_relaxed);
}
