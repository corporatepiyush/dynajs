#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "dyn-pool.h"
#include "dyn-common.h"
#include "dyn-cdefs.h"
#include "dyn-thread-name.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(DYN_POOL_FORCE_GENERIC) || (!defined(__linux__) && !defined(__APPLE__))
#define POOL_GENERIC 1
#endif

#if defined(__linux__)
#include <linux/futex.h>
#include <sched.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 140400
#include <os/os_sync_wait_on_address.h>
#define POOL_OS_SYNC 1
#else
extern int __ulock_wait(uint32_t op, void* addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t op, void* addr, uint64_t wake_value);
#endif
#endif

#ifndef POOL_MAX_THREADS
#define POOL_MAX_THREADS 256u
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#define POOL_LINE 128
#else
#define POOL_LINE 64
#endif

#ifndef POOL_SPIN_MAX
#define POOL_SPIN_MAX 2048u
#endif
#ifndef POOL_SPIN_STEP
#define POOL_SPIN_STEP 64u
#endif
#ifndef POOL_SPIN_DECAY
#define POOL_SPIN_DECAY 1
#endif

#ifndef POOL_BURST
#define POOL_BURST 8u
#endif

#ifndef POOL_ELASTIC_MAX
#define POOL_ELASTIC_MAX 64u
#endif
#ifndef POOL_CPU_CAP_MIN
#define POOL_CPU_CAP_MIN 8u
#endif
#ifndef POOL_SPAWN_COOLDOWN_NS
#define POOL_SPAWN_COOLDOWN_NS 2000000LL
#endif
#ifndef POOL_ELASTIC_KEEP_NS
#define POOL_ELASTIC_KEEP_NS 10000000000LL
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define POOL_SAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define POOL_SAN 1
#endif
#endif
#ifndef POOL_STACK_BYTES
#ifdef POOL_SAN
#define POOL_STACK_BYTES (8u << 20)
#else
#define POOL_STACK_BYTES (1u << 20)
#endif
#endif
#define POOL_OV_KEEP 256u
#ifndef POOL_BLOCK_NS
#define POOL_BLOCK_NS 1000000LL
#ifndef POOL_MON_LINGER_NS
#define POOL_MON_LINGER_NS (16 * POOL_BLOCK_NS)
#endif
#endif

#define POOL_PAGE_SHIFT 9u
#define POOL_PAGE_N (1u << POOL_PAGE_SHIFT)
#define POOL_PAGES 256u

#define POOL_IO 0
#define POOL_CPU 1
#define POOL_DRAIN_SLICE_NS 4000000LL

#ifdef DYN_POOL_RACE_WIDEN
#include <sched.h>
#define POOL_WIDEN() sched_yield()
#else
#define POOL_WIDEN() ((void)0)
#endif

#ifdef POOL_GENERIC
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
} pool_wq_t;
#define POOL_WQ(x) (&(x))
#else
typedef struct pool_wq pool_wq_t;
#define POOL_WQ(x) NULL
#endif

#define POOL_LIKELY(x) __builtin_expect(!!(x), 1)
#define POOL_UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct {
    _Atomic size_t seq;
    _Atomic(dyn_pool_fn) work;
    dyn_pool_fn done;
    void* arg;
} pool_slot_t;

typedef struct {
    dyn_pool_fn work;
    dyn_pool_fn done;
    void* arg;
} pool_ov_t;

typedef struct {
    _Atomic uint32_t seq;
    uint32_t cls;
    dyn_pool_fn done;
    void* arg;
} pool_cslot_t;

struct dyn_pool_chan {
    _Alignas(POOL_LINE) dyn_pool_t* pool;
    pool_slot_t* ring[2];
    pool_cslot_t* cring;
    size_t mask;
    size_t size;
    size_t cmask;
    size_t csize;
    dyn_pool_chan_t* next;
    dyn_pool_chan_t* free_next;
    int wake_r, wake_w;
    dyn_pool_notify_fn notify;
    void* notify_ctx;
    _Atomic uint64_t* actw;
    uint64_t actb;
    unsigned idx;

    _Alignas(POOL_LINE) size_t tail[2];
    size_t rin[2];
    size_t ov_c[2];
    uint32_t submitted;
    size_t inflight;
    size_t chead;
    size_t ov_n;
    size_t cd_n;

    pool_ov_t* ov[2];
    size_t ov_cap[2];
    size_t ov_h[2];
    pool_ov_t* cd;
    size_t cd_cap;

    _Alignas(POOL_LINE) _Atomic size_t head_io;
    _Alignas(POOL_LINE) _Atomic size_t head_cpu;

    _Alignas(POOL_LINE) _Atomic size_t ctail;
    _Atomic uint32_t wake;
    _Atomic uint32_t left;
    _Atomic int closing;
#ifdef POOL_GENERIC
    pool_wq_t left_wq;
#endif
};

_Static_assert(offsetof(struct dyn_pool_chan, submitted) + sizeof(uint32_t) <= offsetof(struct dyn_pool_chan, tail) + POOL_LINE, "chan submit fields share one line");
_Static_assert(sizeof(pool_cslot_t) <= 3 * sizeof(void*), "completion slot is three words");

typedef struct {
    _Alignas(POOL_LINE) dyn_pool_t* pool;
    _Atomic unsigned long long io_done;
    unsigned cur[2];
    unsigned burst[2];
    int pref;
    int kind;
    unsigned spin;
    _Atomic unsigned long long parks;
} pool_worker_t;

typedef struct {
    dyn_pool_fn work;
    dyn_pool_fn done;
    void* arg;
    dyn_pool_chan_t* ch;
    int cls;
    int acct;
    int cacct;
} pool_job_t;

typedef struct {
    _Atomic uint64_t bits[POOL_PAGE_N / 64u];
    dyn_pool_chan_t* chans[POOL_PAGE_N];
} pool_page_t;

struct dyn_pool {
    _Alignas(POOL_LINE) unsigned cap;
    unsigned nthreads;
    unsigned nworkers;
    unsigned io_limit;
    unsigned cpu_cap;
    size_t size;
    _Atomic(dyn_pool_chan_t*) chans;
    _Atomic int stopping;
    pthread_t* threads;
    pool_worker_t* workers;
    pthread_mutex_t reg_mu;
    dyn_pool_chan_t* freelist;
    _Atomic unsigned nidx;
    _Atomic unsigned nlive;
    _Atomic(pool_page_t*) pages[POOL_PAGES];

    unsigned gen;
    _Alignas(POOL_LINE) _Atomic unsigned io_running;
    _Alignas(POOL_LINE) _Atomic uint32_t park_word;
    _Atomic uint64_t idle;
    _Alignas(POOL_LINE) _Atomic uint32_t work_seq;
    _Alignas(POOL_LINE) _Atomic uint32_t spare_word;
    _Atomic int spare_idle;
    _Atomic int spare_mon;
    _Atomic unsigned long long spare_kicks;
    _Alignas(POOL_LINE) _Atomic uint32_t e_word;
    _Atomic uint64_t e_idle;
    _Atomic unsigned e_live;
    _Atomic unsigned e_peak;
    _Atomic unsigned long long e_spawned;
    _Atomic unsigned long long e_retired;
    _Atomic unsigned long long e_parks;
    _Atomic unsigned long long e_done;
    _Atomic long long e_last;
    _Alignas(POOL_LINE) _Atomic unsigned cpu_running;
    _Atomic unsigned long long overflowed;
    _Atomic unsigned long long ov_peak;
    _Atomic unsigned long long cancelled;
#ifdef POOL_GENERIC
    pool_wq_t park_wq;
    pool_wq_t spare_wq;
    pool_wq_t e_wq;
#endif
};

static _Atomic unsigned pool_default_override;

static _Atomic int pool_inline_only;

static pthread_mutex_t pool_shared_mu = PTHREAD_MUTEX_INITIALIZER;
static dyn_pool_t* pool_shared;
static unsigned pool_shared_refs;
static _Atomic unsigned pool_gen;
static pthread_once_t pool_fork_once = PTHREAD_ONCE_INIT;
static volatile int pool_mark_sink;

static void pool_mark_taken(void* a)
{
    (void)a;
    pool_mark_sink = 1;
}

static void pool_mark_cancel(void* a)
{
    (void)a;
    pool_mark_sink = 2;
}

static void pool_fork_child(void)
{
    pthread_mutex_init(&pool_shared_mu, NULL);
    pool_shared = NULL;
    pool_shared_refs = 0;
    atomic_fetch_add_explicit(&pool_gen, 1, memory_order_relaxed);
}

static void pool_fork_init(void)
{
    pthread_atfork(NULL, NULL, pool_fork_child);
}

static void pool_elastic_kick(dyn_pool_t* p);
static int pool_cls_pending(dyn_pool_t* p, int cls);

#if defined(__linux__)
static int pool_read_longs(const char* path, long* a, long* b)
{
    char buf[64];
    char* end;
    size_t n;
    FILE* f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    errno = 0;
    *a = strtol(buf, &end, 10);
    if (end == buf || errno)
        return -1;
    if (b) {
        char* rest = end;
        *b = strtol(rest, &end, 10);
        if (end == rest || errno)
            return -1;
    }
    return 0;
}

static long pool_cgroup_cpus(void)
{
    long quota = 0, period = 0;
    if (pool_read_longs("/sys/fs/cgroup/cpu.max", &quota, &period) != 0
        && (pool_read_longs("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", &quota, NULL) != 0
            || pool_read_longs("/sys/fs/cgroup/cpu/cpu.cfs_period_us", &period, NULL) != 0))
        return 0;
    if (quota <= 0 || period <= 0)
        return 0;
    return (quota + period - 1) / period;
}

static long pool_usable_cpus(void)
{
    cpu_set_t set;
    long n, lim;
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        n = CPU_COUNT(&set);
    else
        n = get_nprocs();
    lim = pool_cgroup_cpus();
    if (lim > 0 && lim < n)
        n = lim;
    return n;
}
#endif

unsigned dyn_pool_max_threads(void)
{
    long n = 0;
#if defined(__linux__)
    n = pool_usable_cpus();
#elif defined(_SC_NPROCESSORS_ONLN)
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1)
        n = 1;
    if (n > (long)POOL_MAX_THREADS)
        n = (long)POOL_MAX_THREADS;
    return (unsigned)n;
}

void dyn_pool_set_default_threads(unsigned n)
{
    unsigned limit = dyn_pool_max_threads();
    if (n == 0) {
        atomic_store_explicit(&pool_inline_only, 1, memory_order_relaxed);
        atomic_store_explicit(&pool_default_override, 0, memory_order_relaxed);
        return;
    }
    if (n > limit)
        n = limit;
    atomic_store_explicit(&pool_inline_only, 0, memory_order_relaxed);
    atomic_store_explicit(&pool_default_override, n, memory_order_relaxed);
}

int dyn_pool_is_inline_only(void)
{
    return atomic_load_explicit(&pool_inline_only, memory_order_relaxed);
}

unsigned dyn_pool_thread_override(void)
{
    return atomic_load_explicit(&pool_default_override, memory_order_relaxed);
}

unsigned dyn_pool_default_threads(void)
{
    unsigned ov = atomic_load_explicit(&pool_default_override,
        memory_order_relaxed);
    if (ov)
        return ov;
    return dyn_pool_max_threads();
}

static inline void pool_pause(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

#ifdef POOL_GENERIC
static void pool_wq_init(pool_wq_t* q)
{
    pthread_mutex_init(&q->mu, NULL);
#if defined(__APPLE__)
    pthread_cond_init(&q->cv, NULL);
#else
    {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&q->cv, &ca);
        pthread_condattr_destroy(&ca);
    }
#endif
}

static void pool_wq_destroy(pool_wq_t* q)
{
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
}
#endif

static void pool_wait(_Atomic uint32_t* a, uint32_t v, pool_wq_t* q)
{
#if defined(POOL_GENERIC)
    pthread_mutex_lock(&q->mu);
    if (atomic_load_explicit(a, memory_order_seq_cst) == v)
        pthread_cond_wait(&q->cv, &q->mu);
    pthread_mutex_unlock(&q->mu);
#elif defined(__linux__)
    (void)q;
    syscall(SYS_futex, (uint32_t*)a, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
#elif defined(POOL_OS_SYNC)
    (void)q;
    os_sync_wait_on_address((void*)a, v, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_NONE);
#else
    (void)q;
    __ulock_wait(0x01000001u, (void*)a, v, 0);
#endif
}

static void pool_wake(_Atomic uint32_t* a, int all, pool_wq_t* q)
{
#if defined(POOL_GENERIC)
    (void)a;
    pthread_mutex_lock(&q->mu);
    if (all)
        pthread_cond_broadcast(&q->cv);
    else
        pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
#elif defined(__linux__)
    (void)q;
    syscall(SYS_futex, (uint32_t*)a, FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, NULL, NULL, 0);
#elif defined(POOL_OS_SYNC)
    (void)q;
    if (all)
        os_sync_wake_by_address_all((void*)a, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
    else
        os_sync_wake_by_address_any((void*)a, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
#else
    (void)q;
    __ulock_wake(0x01000001u | (all ? 0x100u : 0u), (void*)a, 0);
#endif
}

static void pool_wait_ns(_Atomic uint32_t* a, uint32_t v, pool_wq_t* q, int64_t ns)
{
#if defined(POOL_GENERIC)
    struct timespec ts;
    pthread_mutex_lock(&q->mu);
    if (atomic_load_explicit(a, memory_order_seq_cst) == v) {
#if defined(__APPLE__)
        ts.tv_sec = (time_t)(ns / 1000000000LL);
        ts.tv_nsec = (long)(ns % 1000000000LL);
        pthread_cond_timedwait_relative_np(&q->cv, &q->mu, &ts);
#else
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(ns / 1000000000LL);
        ts.tv_nsec += (long)(ns % 1000000000LL);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&q->cv, &q->mu, &ts);
#endif
    }
    pthread_mutex_unlock(&q->mu);
#elif defined(__linux__)
    struct timespec ts;
    (void)q;
    ts.tv_sec = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    syscall(SYS_futex, (uint32_t*)a, FUTEX_WAIT_PRIVATE, v, &ts, NULL, 0);
#elif defined(POOL_OS_SYNC)
    (void)q;
    os_sync_wait_on_address_with_timeout((void*)a, v, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_NONE,
        OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)ns);
#else
    (void)q;
    __ulock_wait(0x01000001u, (void*)a, v, ns / 1000 >= 0xffffffffLL ? 0xffffffffu : (uint32_t)(ns / 1000 + 1));
#endif
}

static int64_t pool_mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int pool_spawn(pthread_t* t, void* (*fn)(void*), void* arg, int detached)
{
    pthread_attr_t at;
    pthread_t tmp;
    sigset_t all, old;
    int rc;
    if (pthread_attr_init(&at) != 0)
        return -1;
    pthread_attr_setstacksize(&at, POOL_STACK_BYTES);
    if (detached)
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    rc = pthread_create(t ? t : &tmp, &at, fn, arg);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    return rc == 0 ? 0 : -1;
}

static void chan_write_wake(dyn_pool_chan_t* ch)
{
#if defined(__linux__)
    {
        uint64_t one = 1;
        ssize_t r = write(ch->wake_w, &one, sizeof(one));
        (void)r;
    }
#else
    {
        unsigned char b = 1;
        ssize_t r = write(ch->wake_w, &b, 1);
        (void)r;
    }
#endif
}

static void chan_signal(dyn_pool_chan_t* ch)
{
    if (atomic_exchange_explicit(&ch->wake, 1, memory_order_acq_rel) != 0)
        return;
    POOL_WIDEN();
    if (ch->notify)
        ch->notify(ch->notify_ctx);
    else
        chan_write_wake(ch);
}

static inline _Atomic size_t* chan_head(dyn_pool_chan_t* c, int cls)
{
    return cls == POOL_IO ? &c->head_io : &c->head_cpu;
}

static inline int chan_peek(dyn_pool_chan_t* c, int cls)
{
    _Atomic size_t* hp = chan_head(c, cls);
    for (;;) {
        size_t h = atomic_load_explicit(hp, memory_order_relaxed);
        size_t lap = h & ~c->mask;
        size_t v = atomic_load_explicit(&c->ring[cls][h & c->mask].seq, memory_order_relaxed);
        if (v == lap + 1u)
            return 1;
        if (v == lap)
            return 0;
        pool_pause();
    }
}

static int chan_pop(dyn_pool_chan_t* c, int cls, pool_job_t* j)
{
    _Atomic size_t* hp = chan_head(c, cls);
    size_t h = atomic_load_explicit(hp, memory_order_relaxed);
    for (;;) {
        pool_slot_t* s = &c->ring[cls][h & c->mask];
        size_t lap = h & ~c->mask;
        size_t v = atomic_load_explicit(&s->seq, memory_order_acquire);
        if (v != lap + 1u) {
            if (v == lap)
                return 0;
            h = atomic_load_explicit(hp, memory_order_relaxed);
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(hp, &h, h + 1u,
                memory_order_relaxed, memory_order_relaxed)) {
            j->work = atomic_exchange_explicit(&s->work, pool_mark_taken, memory_order_acquire);
            j->done = s->done;
            j->arg = s->arg;
            atomic_store_explicit(&s->seq, lap + c->size, memory_order_release);
            return 1;
        }
    }
}

static int pool_cls_pending(dyn_pool_t* p, int cls)
{
    unsigned n = atomic_load_explicit(&p->nidx, memory_order_acquire), pos;
    for (pos = 0; pos < n; pos += 64u) {
        pool_page_t* pg = atomic_load_explicit(&p->pages[pos >> POOL_PAGE_SHIFT], memory_order_acquire);
        uint64_t word = atomic_load_explicit(&pg->bits[(pos & (POOL_PAGE_N - 1u)) >> 6], memory_order_relaxed);
        while (word) {
            unsigned idx = pos + (unsigned)__builtin_ctzll(word);
            word &= word - 1u;
            if (idx < n && chan_peek(pg->chans[idx & (POOL_PAGE_N - 1u)], cls))
                return 1;
        }
    }
    return 0;
}

static int pool_spare_cpu_ok(dyn_pool_t* p)
{
    return atomic_load_explicit(&p->io_running, memory_order_relaxed) >= p->io_limit
        || atomic_load_explicit(&p->cpu_running, memory_order_relaxed) >= p->cpu_cap;
}

static void pool_spare_kick(dyn_pool_t* p)
{
    if (atomic_load_explicit(&p->spare_idle, memory_order_relaxed)
        && atomic_exchange_explicit(&p->spare_idle, 0, memory_order_seq_cst)) {
        atomic_fetch_add_explicit(&p->spare_kicks, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&p->spare_word, 1, memory_order_release);
        pool_wake(&p->spare_word, 0, POOL_WQ(p->spare_wq));
    }
}

static void pool_io_enter(dyn_pool_t* p)
{
    unsigned r = atomic_fetch_add_explicit(&p->io_running, 1u, memory_order_relaxed) + 1u;
    if (r == p->io_limit) {
        atomic_thread_fence(memory_order_seq_cst);
        if (atomic_load_explicit(&p->spare_idle, memory_order_relaxed)
            && (pool_cls_pending(p, POOL_CPU)
                || (!atomic_load_explicit(&p->spare_mon, memory_order_relaxed) && pool_cls_pending(p, POOL_IO))))
            pool_spare_kick(p);
    }
}

static inline dyn_pool_chan_t* pool_chan_at(dyn_pool_t* p, unsigned idx)
{
    pool_page_t* pg = atomic_load_explicit(&p->pages[idx >> POOL_PAGE_SHIFT], memory_order_acquire);
    return pg->chans[idx & (POOL_PAGE_N - 1u)];
}

static int chan_try(dyn_pool_t* p, dyn_pool_chan_t* c, int cls, pool_job_t* j, int* reserved, int acct)
{
    if (!chan_peek(c, cls)) {
        if (chan_peek(c, cls ^ 1))
            return 0;
        atomic_fetch_and_explicit(c->actw, ~c->actb, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        if (!chan_peek(c, POOL_IO) && !chan_peek(c, POOL_CPU))
            return 0;
        atomic_fetch_or_explicit(c->actw, c->actb, memory_order_relaxed);
        if (!chan_peek(c, cls))
            return 0;
    }
    if (acct && !*reserved && (cls == POOL_IO || p->cpu_cap < p->nthreads)) {
        if (cls == POOL_IO) {
            pool_io_enter(p);
        } else if (atomic_fetch_add_explicit(&p->cpu_running, 1u, memory_order_relaxed) >= p->cpu_cap) {
            atomic_fetch_sub_explicit(&p->cpu_running, 1u, memory_order_relaxed);
            return -1;
        }
        *reserved = 1;
    }
    if (!chan_pop(c, cls, j))
        return 0;
    j->ch = c;
    j->cls = cls;
    j->acct = cls == POOL_IO && acct;
    j->cacct = cls == POOL_CPU && *reserved;
    return 1;
}

static int pool_take_cls(pool_worker_t* w, int cls, pool_job_t* j)
{
    dyn_pool_t* p = w->pool;
    unsigned n = atomic_load_explicit(&p->nidx, memory_order_acquire);
    unsigned pos = w->cur[cls], left = n;
    int reserved = 0;
    int acct = w->kind == 0;
    if (!n)
        return 0;
    if (!(w->burst[cls] && w->burst[cls] < POOL_BURST && pos < n
            && p->nthreads < atomic_load_explicit(&p->nlive, memory_order_relaxed)
            && atomic_load_explicit(&pool_chan_at(p, pos)->wake, memory_order_relaxed)))
        pos++;
    if (pos >= n)
        pos = 0;
    while (left) {
        pool_page_t* pg = atomic_load_explicit(&p->pages[pos >> POOL_PAGE_SHIFT], memory_order_acquire);
        unsigned b = pos & 63u, span = 64u - b;
        uint64_t word;
        if (span > left)
            span = left;
        if (span > n - pos)
            span = n - pos;
        word = atomic_load_explicit(&pg->bits[(pos & (POOL_PAGE_N - 1u)) >> 6], memory_order_relaxed) >> b;
        if (span < 64u)
            word &= ((uint64_t)1 << span) - 1u;
        while (word) {
            unsigned idx = pos + (unsigned)__builtin_ctzll(word);
            word &= word - 1u;
            int got = chan_try(p, pg->chans[idx & (POOL_PAGE_N - 1u)], cls, j, &reserved, acct);
            if (got < 0) {
                pool_spare_kick(p);
                return 0;
            }
            if (got) {
                w->burst[cls] = idx == w->cur[cls] && w->burst[cls] ? w->burst[cls] + 1u : 1u;
                w->cur[cls] = idx;
                return 1;
            }
        }
        pos += span;
        left -= span;
        if (pos >= n)
            pos = 0;
    }
    if (reserved)
        atomic_fetch_sub_explicit(cls == POOL_IO ? &p->io_running : &p->cpu_running, 1u, memory_order_relaxed);
    return 0;
}

#define POOL_IDLE_ONE ((uint64_t)1 << 32)

static void pool_idle_leave(dyn_pool_t* p)
{
    uint64_t s = atomic_load_explicit(&p->idle, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&p->idle, &s,
        s - POOL_IDLE_ONE - ((uint32_t)s != 0 ? 1u : 0u), memory_order_seq_cst, memory_order_relaxed))
        ;
    atomic_thread_fence(memory_order_seq_cst);
}

static int pool_wake_claim(dyn_pool_t* p)
{
    uint64_t s = atomic_load_explicit(&p->idle, memory_order_relaxed);
    for (;;) {
        if ((s >> 32) <= (uint32_t)s)
            return 0;
        if (atomic_compare_exchange_weak_explicit(&p->idle, &s, s + 1u, memory_order_seq_cst, memory_order_relaxed))
            return 1;
    }
}

static int pool_take(pool_worker_t* w, pool_job_t* j)
{
    int first = w->pref;
    if (pool_take_cls(w, first, j) || pool_take_cls(w, first ^ 1, j)) {
        w->pref = j->cls ^ 1;
        return 1;
    }
    return 0;
}

static void chan_complete(dyn_pool_chan_t* c, dyn_pool_fn done, void* arg, int cls)
{
    size_t t = atomic_fetch_add_explicit(&c->ctail, 1, memory_order_relaxed);
    pool_cslot_t* s = &c->cring[t & c->cmask];
    size_t lap = t & ~c->cmask;
    while (POOL_UNLIKELY(atomic_load_explicit(&s->seq, memory_order_acquire) != (uint32_t)lap))
        pool_pause();
    s->done = done;
    s->arg = arg;
    s->cls = (uint32_t)cls;
    atomic_store_explicit(&s->seq, (uint32_t)(lap + 1u), memory_order_release);
    chan_signal(c);
    atomic_fetch_add_explicit(&c->left, 1, memory_order_seq_cst);
    if (POOL_UNLIKELY(atomic_load_explicit(&c->closing, memory_order_seq_cst)))
        pool_wake(&c->left, 1, POOL_WQ(c->left_wq));
}

static void pool_run(pool_worker_t* w, pool_job_t* j)
{
    dyn_pool_t* p = w->pool;
    if (j->work && j->work != pool_mark_cancel)
        j->work(j->arg);
    if (j->cacct)
        atomic_fetch_sub_explicit(&p->cpu_running, 1u, memory_order_relaxed);
    if (j->acct) {
        atomic_store_explicit(&w->io_done, atomic_load_explicit(&w->io_done, memory_order_relaxed) + 1u, memory_order_relaxed);
        atomic_fetch_sub_explicit(&p->io_running, 1, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
    } else if (j->cls == POOL_IO && w->kind == 2) {
        atomic_fetch_add_explicit(&p->e_done, 1, memory_order_relaxed);
    }
    chan_complete(j->ch, j->done, j->arg, j->cls);
}

static void* pool_worker(void* arg)
{
    pool_worker_t* w = (pool_worker_t*)arg;
    dyn_pool_t* p = w->pool;
    int woke = 0;
    dyn_thread_name("dyn-pool");

    for (;;) {
        pool_job_t j;
        uint32_t e;
        uint32_t seen = atomic_load_explicit(&p->work_seq, memory_order_relaxed);
        if (pool_take(w, &j)) {
            if (woke && w->spin < POOL_SPIN_MAX)
                w->spin += POOL_SPIN_STEP;
            woke = 0;
            pool_run(w, &j);
            continue;
        }
        woke = 0;
        if (w->spin) {
            unsigned k;
            int got = 0;
            for (k = 0; k < w->spin && !got; k++) {
                pool_pause();
                if (atomic_load_explicit(&p->work_seq, memory_order_relaxed) != seen) {
                    seen = atomic_load_explicit(&p->work_seq, memory_order_relaxed);
                    got = pool_take(w, &j);
                }
            }
            if (got) {
                w->spin = w->spin * 2u > POOL_SPIN_MAX ? POOL_SPIN_MAX : w->spin * 2u;
                pool_run(w, &j);
                continue;
            }
            w->spin -= w->spin >> POOL_SPIN_DECAY;
        }
        POOL_WIDEN();
        e = atomic_load_explicit(&p->park_word, memory_order_acquire);
        atomic_fetch_add_explicit(&p->idle, POOL_IDLE_ONE, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        if (pool_take(w, &j)) {
            pool_idle_leave(p);
            pool_run(w, &j);
            continue;
        }
        if (atomic_load_explicit(&p->stopping, memory_order_seq_cst)) {
            pool_idle_leave(p);
            return NULL;
        }
        atomic_fetch_add_explicit(&w->parks, 1, memory_order_relaxed);
        pool_wait(&p->park_word, e, POOL_WQ(p->park_wq));
        pool_idle_leave(p);
        woke = 1;
    }
}

static unsigned long long pool_io_progress(dyn_pool_t* p)
{
    unsigned long long s = atomic_load_explicit(&p->e_done, memory_order_relaxed);
    unsigned i;
    for (i = 0; i < p->nthreads; i++)
        s += atomic_load_explicit(&p->workers[i].io_done, memory_order_relaxed);
    return s;
}

static int pool_io_stuck(dyn_pool_t* p)
{
    return atomic_load_explicit(&p->io_running, memory_order_relaxed) >= p->io_limit && pool_cls_pending(p, POOL_IO);
}

static int64_t pool_spare_watch(dyn_pool_t* p, int64_t* since, unsigned long long* snap, int64_t* seen)
{
    unsigned long long now_p = pool_io_progress(p);
    int64_t now = pool_mono_ns();
    *seen = now;
    if (!*since || now_p != *snap) {
        *since = now;
        *snap = now_p;
    } else if (now - *since >= POOL_BLOCK_NS) {
        pool_elastic_kick(p);
        *since = now;
    }
    return POOL_BLOCK_NS - (now - *since) > 0 ? POOL_BLOCK_NS - (now - *since) : POOL_BLOCK_NS;
}

static void* pool_spare_main(void* arg)
{
    pool_worker_t* w = (pool_worker_t*)arg;
    dyn_pool_t* p = w->pool;
    unsigned long long snap = 0;
    int64_t since = 0, seen = 0;
    int woke = 0;
    dyn_thread_name("dyn-pool");

    for (;;) {
        pool_job_t j;
        uint32_t e;
        uint32_t ws = atomic_load_explicit(&p->work_seq, memory_order_relaxed);
        int got = pool_spare_cpu_ok(p) && pool_take_cls(w, POOL_CPU, &j);
        if (got) {
            if (woke && w->spin < POOL_SPIN_MAX)
                w->spin += POOL_SPIN_STEP;
        } else if (w->spin) {
            unsigned k;
            for (k = 0; k < w->spin && !got; k++) {
                pool_pause();
                if (atomic_load_explicit(&p->work_seq, memory_order_relaxed) != ws) {
                    ws = atomic_load_explicit(&p->work_seq, memory_order_relaxed);
                    got = pool_spare_cpu_ok(p) && pool_take_cls(w, POOL_CPU, &j);
                }
            }
            if (got)
                w->spin = w->spin * 2u > POOL_SPIN_MAX ? POOL_SPIN_MAX : w->spin * 2u;
            else
                w->spin -= w->spin >> POOL_SPIN_DECAY;
        }
        woke = 0;
        if (got) {
            if (pool_io_stuck(p))
                pool_spare_watch(p, &since, &snap, &seen);
            else
                since = 0;
            pool_run(w, &j);
            continue;
        }
        e = atomic_load_explicit(&p->spare_word, memory_order_acquire);
        atomic_store_explicit(&p->spare_idle, 1, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        if (pool_spare_cpu_ok(p) && pool_take_cls(w, POOL_CPU, &j)) {
            atomic_store_explicit(&p->spare_idle, 0, memory_order_relaxed);
            if (pool_io_stuck(p))
                pool_spare_watch(p, &since, &snap, &seen);
            else
                since = 0;
            pool_run(w, &j);
            continue;
        }
        if (atomic_load_explicit(&p->stopping, memory_order_seq_cst)) {
            atomic_store_explicit(&p->spare_idle, 0, memory_order_relaxed);
            return NULL;
        }
        atomic_fetch_add_explicit(&w->parks, 1, memory_order_relaxed);
        if (pool_io_stuck(p)) {
            int64_t left = pool_spare_watch(p, &since, &snap, &seen);
            atomic_store_explicit(&p->spare_mon, 1, memory_order_relaxed);
            pool_wait_ns(&p->spare_word, e, POOL_WQ(p->spare_wq), left);
        } else if (seen && pool_mono_ns() - seen < POOL_MON_LINGER_NS) {
            since = 0;
            atomic_store_explicit(&p->spare_mon, 1, memory_order_relaxed);
            pool_wait_ns(&p->spare_word, e, POOL_WQ(p->spare_wq), POOL_BLOCK_NS);
        } else {
            since = 0;
            seen = 0;
            if (atomic_load_explicit(&p->spare_mon, memory_order_relaxed)) {
                atomic_store_explicit(&p->spare_mon, 0, memory_order_seq_cst);
                atomic_thread_fence(memory_order_seq_cst);
                if (pool_io_stuck(p)) {
                    atomic_store_explicit(&p->spare_idle, 0, memory_order_relaxed);
                    continue;
                }
            }
            pool_wait(&p->spare_word, e, POOL_WQ(p->spare_wq));
            seen = pool_mono_ns();
        }
        woke = 1;
        atomic_store_explicit(&p->spare_idle, 0, memory_order_relaxed);
    }
}

static void pool_e_leave(dyn_pool_t* p)
{
    uint64_t s = atomic_load_explicit(&p->e_idle, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&p->e_idle, &s,
        s - POOL_IDLE_ONE - ((uint32_t)s != 0 ? 1u : 0u), memory_order_seq_cst, memory_order_relaxed))
        ;
    atomic_thread_fence(memory_order_seq_cst);
}

static int pool_e_claim(dyn_pool_t* p)
{
    uint64_t s = atomic_load_explicit(&p->e_idle, memory_order_relaxed);
    for (;;) {
        if ((s >> 32) <= (uint32_t)s)
            return 0;
        if (atomic_compare_exchange_weak_explicit(&p->e_idle, &s, s + 1u, memory_order_seq_cst, memory_order_relaxed))
            return 1;
    }
}

static void* pool_elastic_main(void* arg)
{
    pool_worker_t* w = (pool_worker_t*)arg;
    dyn_pool_t* p = w->pool;
    int64_t since = pool_mono_ns();
    dyn_thread_name("dyn-pool-blk");

    for (;;) {
        pool_job_t j;
        uint32_t e;
        int64_t left;
        if (pool_take_cls(w, POOL_IO, &j)) {
            pool_run(w, &j);
            since = pool_mono_ns();
            continue;
        }
        e = atomic_load_explicit(&p->e_word, memory_order_acquire);
        atomic_fetch_add_explicit(&p->e_idle, POOL_IDLE_ONE, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        if (pool_take_cls(w, POOL_IO, &j)) {
            pool_e_leave(p);
            pool_run(w, &j);
            since = pool_mono_ns();
            continue;
        }
        if (atomic_load_explicit(&p->stopping, memory_order_seq_cst)) {
            pool_e_leave(p);
            break;
        }
        left = POOL_ELASTIC_KEEP_NS - (pool_mono_ns() - since);
        if (left <= 0) {
            pool_e_leave(p);
            if (pool_take_cls(w, POOL_IO, &j)) {
                pool_run(w, &j);
                since = pool_mono_ns();
                continue;
            }
            break;
        }
        atomic_fetch_add_explicit(&w->parks, 1, memory_order_relaxed);
        pool_wait_ns(&p->e_word, e, POOL_WQ(p->e_wq), left);
        pool_e_leave(p);
    }
    atomic_fetch_add_explicit(&p->e_parks, atomic_load_explicit(&w->parks, memory_order_relaxed), memory_order_relaxed);
    atomic_fetch_add_explicit(&p->e_retired, 1, memory_order_relaxed);
    free(w);
    atomic_fetch_sub_explicit(&p->e_live, 1, memory_order_release);
    return NULL;
}

static void pool_elastic_kick(dyn_pool_t* p)
{
    pool_worker_t* w;
    unsigned n, pk;
    if ((atomic_load_explicit(&p->e_idle, memory_order_relaxed) >> 32) != 0 && pool_e_claim(p)) {
        atomic_fetch_add_explicit(&p->e_word, 1, memory_order_release);
        pool_wake(&p->e_word, 0, POOL_WQ(p->e_wq));
        return;
    }
    if (!pool_cls_pending(p, POOL_IO))
        return;
    {
        int64_t now = pool_mono_ns();
        if (now - atomic_load_explicit(&p->e_last, memory_order_relaxed) < POOL_SPAWN_COOLDOWN_NS)
            return;
        atomic_store_explicit(&p->e_last, now, memory_order_relaxed);
    }
    n = atomic_load_explicit(&p->e_live, memory_order_relaxed);
    do {
        if (n >= POOL_ELASTIC_MAX || atomic_load_explicit(&p->stopping, memory_order_relaxed))
            return;
    } while (!atomic_compare_exchange_weak_explicit(&p->e_live, &n, n + 1u, memory_order_acq_rel, memory_order_relaxed));
    w = (pool_worker_t*)calloc(1, sizeof(*w));
    if (w) {
        w->pool = p;
        w->kind = 2;
    }
    if (!w || pool_spawn(NULL, pool_elastic_main, w, 1) != 0) {
        free(w);
        atomic_fetch_sub_explicit(&p->e_live, 1, memory_order_release);
        return;
    }
    atomic_fetch_add_explicit(&p->e_spawned, 1, memory_order_relaxed);
    pk = atomic_load_explicit(&p->e_peak, memory_order_relaxed);
    while (n + 1u > pk && !atomic_compare_exchange_weak_explicit(&p->e_peak, &pk, n + 1u, memory_order_relaxed, memory_order_relaxed))
        ;
}

static void pool_stop_join(dyn_pool_t* p, unsigned n)
{
    unsigned i;
    atomic_store_explicit(&p->stopping, 1, memory_order_seq_cst);
    atomic_fetch_add_explicit(&p->park_word, 1, memory_order_seq_cst);
    pool_wake(&p->park_word, 1, POOL_WQ(p->park_wq));
    atomic_fetch_add_explicit(&p->spare_word, 1, memory_order_seq_cst);
    pool_wake(&p->spare_word, 1, POOL_WQ(p->spare_wq));
    for (i = 0; i < n; i++)
        pthread_join(p->threads[i], NULL);
    for (;;) {
        struct timespec ts = { 0, 200000L };
        atomic_fetch_add_explicit(&p->e_word, 1, memory_order_seq_cst);
        pool_wake(&p->e_word, 1, POOL_WQ(p->e_wq));
        if (atomic_load_explicit(&p->e_live, memory_order_acquire) == 0)
            break;
        nanosleep(&ts, NULL);
    }
}

static size_t pool_ring_size(unsigned cap)
{
    size_t s = 8;
    while (s < cap)
        s <<= 1;
    return s;
}

dyn_pool_t* dyn_pool_new(unsigned nthreads, unsigned queue_cap)
{
    dyn_pool_t* p;
    void* mem = NULL;
    unsigned i;
    unsigned limit = dyn_pool_max_threads();

    pthread_once(&pool_fork_once, pool_fork_init);
    if (atomic_load_explicit(&pool_inline_only, memory_order_relaxed))
        return NULL;
    if (nthreads == 0)
        nthreads = dyn_pool_default_threads();
    if (nthreads > limit)
        nthreads = limit;
    if (queue_cap == 0)
        queue_cap = nthreads * 4u;
    if (queue_cap < 8u)
        queue_cap = 8u;

    if (posix_memalign(&mem, POOL_LINE, sizeof(*p)) != 0)
        return NULL;
    p = (dyn_pool_t*)mem;
    memset(p, 0, sizeof(*p));
    p->cap = queue_cap;
    p->size = pool_ring_size(queue_cap);
    p->nthreads = nthreads;
    p->nworkers = nthreads + 1u;
    p->io_limit = nthreads;
    p->cpu_cap = nthreads >= POOL_CPU_CAP_MIN ? nthreads - 1u : nthreads;
    p->gen = atomic_load_explicit(&pool_gen, memory_order_relaxed);
    p->threads = (pthread_t*)calloc(p->nworkers, sizeof(pthread_t));
    p->workers = (pool_worker_t*)calloc(p->nworkers, sizeof(pool_worker_t));
    if (!p->threads || !p->workers)
        goto fail;
    if (pthread_mutex_init(&p->reg_mu, NULL) != 0)
        goto fail;
#ifdef POOL_GENERIC
    pool_wq_init(&p->park_wq);
    pool_wq_init(&p->spare_wq);
    pool_wq_init(&p->e_wq);
#endif
    for (i = 0; i < p->nworkers; i++) {
        p->workers[i].pool = p;
        p->workers[i].pref = (int)(i & 1u);
        p->workers[i].kind = i >= nthreads ? 1 : 0;
        if (pool_spawn(&p->threads[i], i >= nthreads ? pool_spare_main : pool_worker, &p->workers[i], 0) != 0) {
            pool_stop_join(p, i);
            pthread_mutex_destroy(&p->reg_mu);
#ifdef POOL_GENERIC
            pool_wq_destroy(&p->park_wq);
            pool_wq_destroy(&p->spare_wq);
            pool_wq_destroy(&p->e_wq);
#endif
            goto fail;
        }
    }
    return p;

fail:
    free(p->threads);
    free(p->workers);
    free(p);
    return NULL;
}

static void chan_close_fds(dyn_pool_chan_t* ch)
{
    if (ch->wake_r >= 0)
        close(ch->wake_r);
    if (ch->wake_w >= 0 && ch->wake_w != ch->wake_r)
        close(ch->wake_w);
    ch->wake_r = ch->wake_w = -1;
}

static void chan_ov_drop(dyn_pool_chan_t* ch)
{
    int cls;
    for (cls = 0; cls < 2; cls++) {
        free(ch->ov[cls]);
        ch->ov[cls] = NULL;
        ch->ov_cap[cls] = ch->ov_h[cls] = ch->ov_c[cls] = 0;
    }
    free(ch->cd);
    ch->cd = NULL;
    ch->cd_n = ch->cd_cap = 0;
    ch->ov_n = 0;
}

static void chan_destroy(dyn_pool_chan_t* ch)
{
    chan_close_fds(ch);
    chan_ov_drop(ch);
#ifdef POOL_GENERIC
    pool_wq_destroy(&ch->left_wq);
#endif
    free(ch->ring[0]);
    free(ch->ring[1]);
    free(ch->cring);
    free(ch);
}

void dyn_pool_free(dyn_pool_t* p)
{
    dyn_pool_chan_t* ch;
    unsigned i;
    if (!p)
        return;
    pool_stop_join(p, p->nworkers);
    ch = atomic_load_explicit(&p->chans, memory_order_acquire);
    while (ch) {
        dyn_pool_chan_t* next = ch->next;
        chan_destroy(ch);
        ch = next;
    }
    for (i = 0; i < POOL_PAGES; i++)
        free(atomic_load_explicit(&p->pages[i], memory_order_relaxed));
    pthread_mutex_destroy(&p->reg_mu);
#ifdef POOL_GENERIC
    pool_wq_destroy(&p->park_wq);
    pool_wq_destroy(&p->spare_wq);
    pool_wq_destroy(&p->e_wq);
#endif
    free(p->threads);
    free(p->workers);
    free(p);
}

dyn_pool_t* dyn_pool_shared_acquire(void)
{
    dyn_pool_t* p;
    pthread_mutex_lock(&pool_shared_mu);
    if (!pool_shared)
        pool_shared = dyn_pool_new(0, 0);
    p = pool_shared;
    if (p)
        pool_shared_refs++;
    pthread_mutex_unlock(&pool_shared_mu);
    return p;
}

void dyn_pool_shared_release(dyn_pool_t* p)
{
    dyn_pool_t* dead = NULL;
    if (!p)
        return;
    pthread_mutex_lock(&pool_shared_mu);
    if (p == pool_shared && pool_shared_refs > 0 && --pool_shared_refs == 0) {
        dead = pool_shared;
        pool_shared = NULL;
    }
    pthread_mutex_unlock(&pool_shared_mu);
    dyn_pool_free(dead);
}

unsigned dyn_pool_shared_refs(void)
{
    unsigned n;
    pthread_mutex_lock(&pool_shared_mu);
    n = pool_shared_refs;
    pthread_mutex_unlock(&pool_shared_mu);
    return n;
}

unsigned dyn_pool_threads(const dyn_pool_t* p) { return p ? p->nthreads : 0; }
unsigned dyn_pool_capacity(const dyn_pool_t* p) { return p ? p->cap : 0; }

void dyn_pool_stats(const dyn_pool_t* p, dyn_pool_stats_t* st)
{
    unsigned i;
    memset(st, 0, sizeof(*st));
    if (!p)
        return;
    for (i = 0; i < p->nworkers; i++)
        st->parks += atomic_load_explicit(&p->workers[i].parks, memory_order_relaxed);
    st->parks += atomic_load_explicit(&p->e_parks, memory_order_relaxed);
    st->threads = p->nworkers;
    st->blocking_live = atomic_load_explicit(&p->e_live, memory_order_relaxed);
    st->blocking_peak = atomic_load_explicit(&p->e_peak, memory_order_relaxed);
    st->blocking_spawned = atomic_load_explicit(&p->e_spawned, memory_order_relaxed);
    st->blocking_retired = atomic_load_explicit(&p->e_retired, memory_order_relaxed);
    st->overflowed = atomic_load_explicit(&p->overflowed, memory_order_relaxed);
    st->overflow_peak = atomic_load_explicit(&p->ov_peak, memory_order_relaxed);
    st->cancelled = atomic_load_explicit(&p->cancelled, memory_order_relaxed);
    st->spare_kicks = atomic_load_explicit(&p->spare_kicks, memory_order_relaxed);
    st->spare_parks = p->nworkers > p->nthreads ? atomic_load_explicit(&p->workers[p->nthreads].parks, memory_order_relaxed) : 0;
    st->io_running = atomic_load_explicit(&p->io_running, memory_order_relaxed);
    st->cpu_running = atomic_load_explicit(&p->cpu_running, memory_order_relaxed);
}

static int chan_open_fds(dyn_pool_chan_t* ch)
{
    if (ch->notify)
        return 0;
#if defined(__linux__)
    ch->wake_r = ch->wake_w = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return ch->wake_r < 0 ? -1 : 0;
#else
    int fds[2];
    if (dyn_pipe_cloexec(fds) != 0)
        return -1;
    ch->wake_r = fds[0];
    ch->wake_w = fds[1];
    dyn_fd_set_nonblock(ch->wake_r);
    dyn_fd_set_nonblock(ch->wake_w);
    return 0;
#endif
}

static dyn_pool_chan_t* chan_make(dyn_pool_t* p, dyn_pool_notify_fn fn, void* ctx)
{
    dyn_pool_chan_t* ch;
    void* mem = NULL;

    if (!p)
        return NULL;
    pthread_mutex_lock(&p->reg_mu);
    ch = p->freelist;
    if (ch)
        p->freelist = ch->free_next;
    pthread_mutex_unlock(&p->reg_mu);
    if (ch) {
        ch->notify = fn;
        ch->notify_ctx = ctx;
        if (chan_open_fds(ch) != 0) {
            pthread_mutex_lock(&p->reg_mu);
            ch->free_next = p->freelist;
            p->freelist = ch;
            pthread_mutex_unlock(&p->reg_mu);
            return NULL;
        }
        ch->inflight = 0;
        atomic_store_explicit(&ch->wake, 0, memory_order_relaxed);
        atomic_store_explicit(&ch->closing, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&p->nlive, 1, memory_order_relaxed);
        return ch;
    }
    if (posix_memalign(&mem, POOL_LINE, sizeof(*ch)) != 0)
        return NULL;
    ch = (dyn_pool_chan_t*)mem;
    memset(ch, 0, sizeof(*ch));
    ch->pool = p;
#ifdef POOL_GENERIC
    pool_wq_init(&ch->left_wq);
#endif
    ch->size = p->size;
    ch->mask = p->size - 1u;
    ch->csize = p->size * 2u;
    ch->cmask = ch->csize - 1u;
    ch->wake_r = ch->wake_w = -1;
    ch->notify = fn;
    ch->notify_ctx = ctx;
    ch->ring[0] = (pool_slot_t*)calloc(p->size, sizeof(pool_slot_t));
    ch->ring[1] = (pool_slot_t*)calloc(p->size, sizeof(pool_slot_t));
    ch->cring = (pool_cslot_t*)calloc(p->size * 2u, sizeof(pool_cslot_t));
    if (!ch->ring[0] || !ch->ring[1] || !ch->cring || chan_open_fds(ch) != 0) {
        chan_destroy(ch);
        return NULL;
    }
    pthread_mutex_lock(&p->reg_mu);
    {
        unsigned idx = atomic_load_explicit(&p->nidx, memory_order_relaxed);
        pool_page_t* pg = idx < POOL_PAGES * POOL_PAGE_N
            ? atomic_load_explicit(&p->pages[idx >> POOL_PAGE_SHIFT], memory_order_relaxed)
            : NULL;
        if (!pg && idx < POOL_PAGES * POOL_PAGE_N) {
            pg = (pool_page_t*)calloc(1, sizeof(*pg));
            if (pg)
                atomic_store_explicit(&p->pages[idx >> POOL_PAGE_SHIFT], pg, memory_order_release);
        }
        if (!pg) {
            pthread_mutex_unlock(&p->reg_mu);
            chan_destroy(ch);
            errno = ENOMEM;
            return NULL;
        }
        pg->chans[idx & (POOL_PAGE_N - 1u)] = ch;
        ch->idx = idx;
        ch->actw = &pg->bits[(idx & (POOL_PAGE_N - 1u)) >> 6];
        ch->actb = (uint64_t)1 << (idx & 63u);
        ch->next = atomic_load_explicit(&p->chans, memory_order_relaxed);
        atomic_store_explicit(&p->chans, ch, memory_order_release);
        atomic_store_explicit(&p->nidx, idx + 1u, memory_order_release);
        atomic_fetch_add_explicit(&p->nlive, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&p->reg_mu);
    return ch;
}

dyn_pool_chan_t* dyn_pool_chan_new(dyn_pool_t* p) { return chan_make(p, NULL, NULL); }

dyn_pool_chan_t* dyn_pool_chan_new_notify(dyn_pool_t* p, dyn_pool_notify_fn fn, void* ctx)
{
    if (!fn) {
        errno = EINVAL;
        return NULL;
    }
    return chan_make(p, fn, ctx);
}

void dyn_pool_chan_free(dyn_pool_chan_t* ch)
{
    dyn_pool_t* p;

    if (!ch)
        return;
    p = ch->pool;
    atomic_store_explicit(&ch->closing, 1, memory_order_seq_cst);
    for (;;) {
        uint32_t v = atomic_load_explicit(&ch->left, memory_order_seq_cst);
        if (v == ch->submitted)
            break;
        pool_wait(&ch->left, v, POOL_WQ(ch->left_wq));
    }
    while (ch->chead != atomic_load_explicit(&ch->ctail, memory_order_acquire)) {
        pool_cslot_t* s = &ch->cring[ch->chead & ch->cmask];
        size_t lap = ch->chead & ~ch->cmask;
        while (atomic_load_explicit(&s->seq, memory_order_acquire) != (uint32_t)(lap + 1u))
            pool_pause();
        atomic_store_explicit(&s->seq, (uint32_t)(lap + ch->csize), memory_order_release);
        ch->chead++;
    }
    ch->inflight = 0;
    ch->rin[0] = ch->rin[1] = 0;
    chan_ov_drop(ch);
    chan_close_fds(ch);
    atomic_fetch_sub_explicit(&p->nlive, 1, memory_order_relaxed);
    pthread_mutex_lock(&p->reg_mu);
    ch->free_next = p->freelist;
    p->freelist = ch;
    pthread_mutex_unlock(&p->reg_mu);
}

int dyn_pool_wake_fd(const dyn_pool_chan_t* ch) { return ch ? ch->wake_r : -1; }

size_t dyn_pool_inflight(const dyn_pool_chan_t* ch)
{
    return ch ? ch->inflight : 0;
}

static int chan_ring_put(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done, void* arg, int cls)
{
    size_t pos = ch->tail[cls], lap = pos & ~ch->mask;
    pool_slot_t* s = &ch->ring[cls][pos & ch->mask];
    if (POOL_UNLIKELY(ch->rin[cls] >= (size_t)ch->pool->cap
            || atomic_load_explicit(&s->seq, memory_order_acquire) != lap))
        return -1;
    atomic_store_explicit(&s->work, work, memory_order_relaxed);
    s->done = done;
    s->arg = arg;
    atomic_store_explicit(&s->seq, lap + 1u, memory_order_release);
    ch->tail[cls] = pos + 1u;
    ch->rin[cls]++;
    ch->submitted++;
    return 0;
}

static void chan_publish(dyn_pool_chan_t* ch, int cls)
{
    dyn_pool_t* p = ch->pool;
    atomic_fetch_add_explicit(&p->work_seq, 1, memory_order_relaxed);
    POOL_WIDEN();
    atomic_thread_fence(memory_order_seq_cst);
    if (!(atomic_load_explicit(ch->actw, memory_order_relaxed) & ch->actb)) {
        atomic_fetch_or_explicit(ch->actw, ch->actb, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
    }
    if (cls == POOL_IO && atomic_load_explicit(&p->io_running, memory_order_relaxed) >= p->io_limit) {
        if ((atomic_load_explicit(&p->e_idle, memory_order_relaxed) >> 32) != 0 && pool_e_claim(p)) {
            atomic_fetch_add_explicit(&p->e_word, 1, memory_order_release);
            pool_wake(&p->e_word, 0, POOL_WQ(p->e_wq));
        } else if (!atomic_load_explicit(&p->spare_mon, memory_order_relaxed)) {
            pool_spare_kick(p);
        }
        return;
    }
    if (cls == POOL_CPU && atomic_load_explicit(&p->cpu_running, memory_order_relaxed) >= p->cpu_cap) {
        pool_spare_kick(p);
        return;
    }
    if ((atomic_load_explicit(&p->idle, memory_order_relaxed) >> 32) != 0 && pool_wake_claim(p)) {
        atomic_fetch_add_explicit(&p->park_word, 1, memory_order_release);
        pool_wake(&p->park_word, 0, POOL_WQ(p->park_wq));
    }
    if (cls == POOL_CPU && pool_spare_cpu_ok(p))
        pool_spare_kick(p);
}

static int chan_ov_grow(pool_ov_t** a, size_t* cap, size_t h, size_t n)
{
    size_t nc = *cap ? *cap * 2u : 64u, i;
    pool_ov_t* na;
    if (nc < *cap || nc > SIZE_MAX / sizeof(pool_ov_t))
        return -1;
    na = (pool_ov_t*)malloc(nc * sizeof(pool_ov_t));
    if (!na)
        return -1;
    for (i = 0; i < n; i++)
        na[i] = (*a)[(h + i) & (*cap - 1u)];
    free(*a);
    *a = na;
    *cap = nc;
    return 0;
}

static int chan_ov_push(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done, void* arg, int cls)
{
    dyn_pool_t* p = ch->pool;
    pool_ov_t* e;
    unsigned long long pk;
    if (ch->ov_c[cls] == ch->ov_cap[cls]) {
        if (chan_ov_grow(&ch->ov[cls], &ch->ov_cap[cls], ch->ov_h[cls], ch->ov_c[cls]) != 0) {
            errno = ENOMEM;
            return -1;
        }
        ch->ov_h[cls] = 0;
    }
    e = &ch->ov[cls][(ch->ov_h[cls] + ch->ov_c[cls]) & (ch->ov_cap[cls] - 1u)];
    e->work = work;
    e->done = done;
    e->arg = arg;
    ch->ov_c[cls]++;
    ch->ov_n++;
    ch->inflight++;
    atomic_fetch_add_explicit(&p->overflowed, 1, memory_order_relaxed);
    pk = atomic_load_explicit(&p->ov_peak, memory_order_relaxed);
    while (ch->ov_n > pk && !atomic_compare_exchange_weak_explicit(&p->ov_peak, &pk, ch->ov_n, memory_order_relaxed, memory_order_relaxed))
        ;
    return 0;
}

static void chan_refill(dyn_pool_chan_t* ch, int cls)
{
    while (ch->ov_c[cls]) {
        pool_ov_t* e = &ch->ov[cls][ch->ov_h[cls] & (ch->ov_cap[cls] - 1u)];
        if (e->work != pool_mark_taken) {
            if (chan_ring_put(ch, e->work, e->done, e->arg, cls) != 0)
                break;
            chan_publish(ch, cls);
        }
        ch->ov_h[cls]++;
        ch->ov_c[cls]--;
        ch->ov_n--;
    }
    if (!ch->ov_c[cls] && ch->ov_cap[cls] > POOL_OV_KEEP) {
        free(ch->ov[cls]);
        ch->ov[cls] = NULL;
        ch->ov_cap[cls] = 0;
    }
    if (!ch->ov_c[cls])
        ch->ov_h[cls] = 0;
}

static int pool_submit(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done,
    void* arg, int cls)
{
    dyn_pool_t* p;
    if (POOL_UNLIKELY(!ch))
        return -1;
    p = ch->pool;
    if (POOL_UNLIKELY(atomic_load_explicit(&ch->closing, memory_order_relaxed)
            || atomic_load_explicit(&p->stopping, memory_order_relaxed))) {
        errno = EAGAIN;
        return -1;
    }
    if (POOL_UNLIKELY(p->gen != atomic_load_explicit(&pool_gen, memory_order_relaxed))) {
        errno = ECHILD;
        return -1;
    }
    if (POOL_LIKELY(!ch->ov_c[cls]) && chan_ring_put(ch, work, done, arg, cls) == 0) {
        ch->inflight++;
        chan_publish(ch, cls);
        return 0;
    }
    return chan_ov_push(ch, work, done, arg, cls);
}

int dyn_pool_submit(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done,
    void* arg)
{
    return pool_submit(ch, work, done, arg, POOL_IO);
}

int dyn_pool_submit_cpu(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done,
    void* arg)
{
    return pool_submit(ch, work, done, arg, POOL_CPU);
}

int dyn_pool_cpu_room(const dyn_pool_chan_t* ch)
{
    dyn_pool_chan_t* c = (dyn_pool_chan_t*)DYN_UNCONST(ch);
    return c && !atomic_load_explicit(&c->closing, memory_order_relaxed);
}

int dyn_pool_cancel(dyn_pool_chan_t* ch, dyn_pool_fn work, void* arg)
{
    int cls;
    if (!ch)
        return 0;
    for (cls = 0; cls < 2; cls++) {
        size_t h, t, i;
        for (i = 0; i < ch->ov_c[cls]; i++) {
            pool_ov_t* o = &ch->ov[cls][(ch->ov_h[cls] + i) & (ch->ov_cap[cls] - 1u)];
            if (o->work == pool_mark_taken || o->work != work || o->arg != arg)
                continue;
            if (ch->cd_n == ch->cd_cap && chan_ov_grow(&ch->cd, &ch->cd_cap, 0, ch->cd_n) != 0)
                return 0;
            ch->cd[ch->cd_n++] = *o;
            o->work = pool_mark_taken;
            atomic_fetch_add_explicit(&ch->pool->cancelled, 1, memory_order_relaxed);
            chan_signal(ch);
            return 1;
        }
        h = atomic_load_explicit(chan_head(ch, cls), memory_order_acquire);
        for (t = ch->tail[cls]; h != t; h++) {
            pool_slot_t* s = &ch->ring[cls][h & ch->mask];
            dyn_pool_fn want = work;
            if (s->arg != arg)
                continue;
            if (atomic_compare_exchange_strong_explicit(&s->work, &want, pool_mark_cancel,
                    memory_order_acq_rel, memory_order_relaxed)) {
                atomic_fetch_add_explicit(&ch->pool->cancelled, 1, memory_order_relaxed);
                return 1;
            }
        }
    }
    return 0;
}

static int64_t pool_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

int dyn_pool_drain(dyn_pool_chan_t* ch)
{
    size_t budget, mask, lap;
    int ran = 0;
    int64_t t0 = -1;

    if (!ch)
        return 0;
    if (atomic_load_explicit(&ch->wake, memory_order_relaxed)) {
        uint64_t v[2];
        if (ch->notify || read(ch->wake_r, v, sizeof(v)) > 0)
            atomic_exchange_explicit(&ch->wake, 0, memory_order_acq_rel);
    }
    if (ch->cd_n) {
        size_t i, n = ch->cd_n;
        for (i = 0; i < n; i++) {
            pool_ov_t o = ch->cd[i];
            ch->inflight--;
            if (o.done)
                o.done(o.arg);
            ran++;
        }
        if (ch->cd_n == n)
            ch->cd_n = 0;
        else {
            memmove(ch->cd, ch->cd + n, (ch->cd_n - n) * sizeof(*ch->cd));
            ch->cd_n -= n;
        }
    }
    mask = ch->cmask;
    budget = atomic_load_explicit(&ch->ctail, memory_order_acquire) - ch->chead;
    while (budget > 0) {
        pool_cslot_t* s = &ch->cring[ch->chead & mask];
        dyn_pool_fn done;
        void* arg;
        lap = ch->chead & ~mask;
        if (atomic_load_explicit(&s->seq, memory_order_acquire) != (uint32_t)(lap + 1u))
            break;
        done = s->done;
        arg = s->arg;
        ch->rin[s->cls]--;
        atomic_store_explicit(&s->seq, (uint32_t)(lap + ch->csize), memory_order_release);
        ch->chead++;
        ch->inflight--;
        budget--;
        if (t0 < 0 && budget > 0)
            t0 = pool_now_ns();
        if (done)
            done(arg);
        ran++;
        if (budget > 0 && t0 >= 0 && !(ran & 7) && pool_now_ns() - t0 >= POOL_DRAIN_SLICE_NS) {
            chan_signal(ch);
            break;
        }
    }
    if (ch->ov_n && !atomic_load_explicit(&ch->closing, memory_order_relaxed)) {
        chan_refill(ch, POOL_IO);
        chan_refill(ch, POOL_CPU);
    }
    return ran;
}
