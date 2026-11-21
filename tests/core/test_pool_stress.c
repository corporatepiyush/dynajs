/* dyn-pool stress: the lock-free pool (per-channel per-class SPMC sequence rings, an MPSC completion
 * ring per channel, futex/os_sync parking that wakes one worker per submit, CAS io_running, a busy
 * count for close). Built plain (ASan+UBSan) and under TSan by `tests/ctest/run.sh test-pool-stress`.
 * Every wait is a poll() on the channel's wake fd with a deadline, so a lost worker wakeup or a lost
 * completion wakeup is a FAIL ("lost wakeup"), never a hang; SIGALRM is the last-resort watchdog.
 * Cases: many producers on one pool (each job's work runs once, its done runs once on the owner),
 * close while jobs are in flight (chan_free waits for queued work, no done after free, channels are
 * recycled while other channels stay busy), the 4 ms drain slice re-signals the fd, the per-channel
 * jobs past the queue cap wait in the pool overflow and all complete, cross-channel turns with 4 threads,
 * one submit wakes one parked worker (dyn_pool_stats parks), lone jobs submitted at random moments as
 * the worker parks (park race; built again with -DDYN_POOL_RACE_WIDEN, which yields inside the park and
 * submit windows so a missing recheck or wake loses the job within a few rounds), the blocking-class limit under load,
 * the spare worker (threads >= 2 get one extra worker that runs only cpu jobs, and only while every regular
 * thread holds a blocking job: a cpu job queued before the last blocking take, and one submitted while all are
 * held, each finish within 2 s), lone jobs on random channels among 1100 notify channels (channels with work are found through an
 * active bitmap of 64-bit words in 512-channel pages; a wrong clear/recheck strands a job for 3 s), and pool teardown with queued work. Built a fourth time with
 * -DDYN_POOL_FORCE_GENERIC to run the mutex/condvar parking used where no futex exists. POOL_STRESS_SCALE scales the job counts (TSan runs use 1); POOL_STRESS_ONLY=park,producers,close,slice,full,turns,single,iolimit,spare,many,teardown
 * picks cases. Late wake bytes are legal: a worker preempted between setting the wake flag and writing
 * the fd delivers its byte after the owner already drained the completion (seen 12 times in 50 widen runs
 * under a CPU hog at load 95); that costs one empty wakeup, and the next drain clears it because the flag
 * stays set until a read consumes a byte. The park race therefore asserts the invariant that matters: the
 * fd is never readable after two drains with nothing pending (a byte with the flag clear spins the loop;
 * the wake_clear_always mutant fails it 20 of 20 runs). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dyn-pool.h"

static int fails;
#define CHECK(c, ...)                                  \
    do {                                               \
        if (!(c)) {                                    \
            fails++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while (0)

static unsigned g_scale = 1;

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void spin_us(unsigned us)
{
    struct timespec t;
    uint64_t end;
    clock_gettime(CLOCK_MONOTONIC, &t);
    end = (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u + us;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u >= end)
            return;
    }
}

static int wait_fd(dyn_pool_chan_t* ch, int ms)
{
    struct pollfd pf;
    pf.fd = dyn_pool_wake_fd(ch);
    pf.events = POLLIN;
    pf.revents = 0;
    return poll(&pf, 1, ms);
}

static int drain_poll(dyn_pool_chan_t* ch, int want, int ms, const char* what)
{
    int got = 0;
    uint64_t t0 = now_ms();
    while (got < want) {
        int r = wait_fd(ch, 3000);
        if (r == 0 && dyn_pool_inflight(ch) > 0) {
            printf("FAIL lost wakeup (%s): no wake within 3 s, %zu in flight, %d/%d drained\n", what, dyn_pool_inflight(ch), got, want);
            fails++;
            return got;
        }
        got += dyn_pool_drain(ch);
        if (now_ms() - t0 > (uint64_t)ms) {
            printf("FAIL timeout (%s): %d/%d drained\n", what, got, want);
            fails++;
            return got;
        }
    }
    return got;
}

typedef struct {
    _Atomic int runs;
    int dones;
} rec_t;

typedef struct {
    dyn_pool_t* pool;
    unsigned njobs;
    rec_t* recs;
    pthread_t owner;
    int bad_thread;
    int lost;
    unsigned done;
} prod_t;

static _Thread_local prod_t* t_prod;

static void w_rec(void* arg)
{
    rec_t* r = (rec_t*)arg;
    atomic_fetch_add_explicit(&r->runs, 1, memory_order_relaxed);
    if (((uintptr_t)r >> 3) % 61u == 0)
        spin_us(20);
}

static void d_rec(void* arg)
{
    rec_t* r = (rec_t*)arg;
    r->dones++;
    if (!t_prod || !pthread_equal(t_prod->owner, pthread_self()))
        t_prod->bad_thread++;
    t_prod->done++;
}

static void* producer(void* arg)
{
    prod_t* P = (prod_t*)arg;
    dyn_pool_chan_t* ch = dyn_pool_chan_new(P->pool);
    unsigned next = 0;
    t_prod = P;
    P->owner = pthread_self();
    if (!ch) {
        P->lost = -1;
        return NULL;
    }
    while (P->done < P->njobs) {
        while (next < P->njobs) {
            int rc = (next % 3u == 0) ? dyn_pool_submit_cpu(ch, w_rec, d_rec, &P->recs[next])
                                      : dyn_pool_submit(ch, w_rec, d_rec, &P->recs[next]);
            if (rc != 0)
                break;
            next++;
        }
        if (wait_fd(ch, 3000) == 0 && dyn_pool_inflight(ch) > 0) {
            P->lost++;
            break;
        }
        dyn_pool_drain(ch);
    }
    dyn_pool_chan_free(ch);
    return NULL;
}

static void case_producers(void)
{
    enum { NP = 6 };
    dyn_pool_t* p = dyn_pool_new(4, 64);
    prod_t P[NP];
    pthread_t th[NP];
    unsigned i, k, n = 4000u * g_scale;
    int bad = 0, lost = 0, multi = 0, nodone = 0;
    memset(P, 0, sizeof P);
    CHECK(p != NULL, "pool_new (producers)");
    if (!p)
        return;
    for (i = 0; i < NP; i++) {
        P[i].pool = p;
        P[i].njobs = n;
        P[i].recs = (rec_t*)calloc(n, sizeof(rec_t));
        pthread_create(&th[i], NULL, producer, &P[i]);
    }
    for (i = 0; i < NP; i++)
        pthread_join(th[i], NULL);
    for (i = 0; i < NP; i++) {
        bad += P[i].bad_thread;
        lost += P[i].lost;
        for (k = 0; k < n; k++) {
            if (atomic_load(&P[i].recs[k].runs) != 1)
                multi++;
            if (P[i].recs[k].dones != 1)
                nodone++;
        }
        free(P[i].recs);
    }
    CHECK(lost == 0, "lost wakeup: %d producer(s) waited 3 s with work in flight", lost);
    CHECK(multi == 0, "%d jobs ran a number of times other than once", multi);
    CHECK(nodone == 0, "%d jobs saw done a number of times other than once", nodone);
    CHECK(bad == 0, "%d done callbacks ran off the owner thread", bad);
    dyn_pool_free(p);
    printf("  %d producers x %u mixed io/cpu jobs on 4 threads: ok\n", NP, n);
}

static _Atomic int g_work;
static _Atomic int g_after_free_done;
static _Atomic int g_close_stop;

static void w_slow(void* arg)
{
    spin_us((unsigned)(uintptr_t)arg);
    atomic_fetch_add_explicit(&g_work, 1, memory_order_relaxed);
}
static void d_count(void* arg)
{
    (void)arg;
    atomic_fetch_add_explicit(&g_after_free_done, 1, memory_order_relaxed);
}

static void w_noise(void* arg) { (void)arg; }
static void* noise_loop(void* arg)
{
    dyn_pool_t* p = (dyn_pool_t*)arg;
    while (!atomic_load(&g_close_stop)) {
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        int i, sub = 0;
        if (!ch)
            continue;
        for (i = 0; i < 12; i++)
            sub += dyn_pool_submit(ch, w_noise, NULL, NULL) == 0;
        if (sub)
            drain_poll(ch, sub, 10000, "noise");
        dyn_pool_chan_free(ch);
    }
    return NULL;
}

static void case_close_in_flight(void)
{
    dyn_pool_t* p = dyn_pool_new(3, 16);
    pthread_t noise;
    unsigned r, rounds = 150u * g_scale;
    int bad_wait = 0, bad_done = 0;
    CHECK(p != NULL, "pool_new (close)");
    if (!p)
        return;
    atomic_store(&g_close_stop, 0);
    pthread_create(&noise, NULL, noise_loop, p);
    for (r = 0; r < rounds; r++) {
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        int k = 1 + (int)(r % 16u), i, sub = 0, before;
        atomic_store(&g_work, 0);
        atomic_store(&g_after_free_done, 0);
        for (i = 0; i < k; i++)
            sub += ((i & 1) ? dyn_pool_submit_cpu(ch, w_slow, d_count, (void*)(uintptr_t)(r % 40u))
                            : dyn_pool_submit(ch, w_slow, d_count, (void*)(uintptr_t)(r % 40u)))
                == 0;
        if (r % 3u == 0) {
            wait_fd(ch, 50);
            dyn_pool_drain(ch);
        }
        before = atomic_load(&g_after_free_done);
        dyn_pool_chan_free(ch);
        if (atomic_load(&g_work) != sub)
            bad_wait++;
        usleep(r % 7u == 0 ? 2000 : 0);
        if (atomic_load(&g_after_free_done) != before)
            bad_done++;
    }
    atomic_store(&g_close_stop, 1);
    pthread_join(noise, NULL);
    CHECK(bad_wait == 0, "%d chan_free calls returned before their queued work ran", bad_wait);
    CHECK(bad_done == 0, "%d freed channels still ran a done callback", bad_done);
    dyn_pool_free(p);
    printf("  close during flight (%u rounds, recycled channels, a busy neighbour): ok\n", rounds);
}

static void d_sleep_ms(void* arg)
{
    (void)arg;
    usleep(1000);
}

static void case_drain_slice(void)
{
    dyn_pool_t* p = dyn_pool_new(2, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    int i, sub = 0, first, total;
    uint64_t t0;
    CHECK(p && ch, "pool + chan (slice)");
    if (!p || !ch)
        return;
    atomic_store(&g_work, 0);
    for (i = 0; i < 64; i++)
        sub += dyn_pool_submit(ch, w_slow, d_sleep_ms, (void*)(uintptr_t)0) == 0;
    t0 = now_ms();
    while (atomic_load(&g_work) < sub && now_ms() - t0 < 5000)
        usleep(1000);
    usleep(20000);
    first = dyn_pool_drain(ch);
    CHECK(first >= 1 && first < sub, "one drain must stop at the 4 ms slice (ran %d of %d)", first, sub);
    CHECK(wait_fd(ch, 0) == 1, "a drain cut by the slice must re-signal the wake fd");
    total = first + drain_poll(ch, sub - first, 10000, "slice");
    CHECK(total == sub, "slice drains finish all (%d of %d)", total, sub);
    CHECK(wait_fd(ch, 0) == 0, "the fd is quiet once everything is drained");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  drain slice re-signal: ok\n");
}

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate;
static void w_gate(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_gate_mu);
    while (!g_gate)
        pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    pthread_mutex_unlock(&g_gate_mu);
}
static void gate_set(int v)
{
    pthread_mutex_lock(&g_gate_mu);
    g_gate = v;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

static void case_queue_full(void)
{
    dyn_pool_t* p = dyn_pool_new(2, 8);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    unsigned cap = dyn_pool_capacity(p);
    int acc = 0, e = 0, round;
    CHECK(p && ch, "pool + chan (full)");
    if (!p || !ch)
        return;
    for (round = 0; round < 3; round++) {
        gate_set(0);
        acc = 0;
        while (acc < 1000) {
            if ((acc & 1 ? dyn_pool_submit_cpu(ch, w_gate, NULL, NULL) : dyn_pool_submit(ch, w_gate, NULL, NULL)) != 0) {
                e = errno;
                break;
            }
            acc++;
        }
        CHECK(acc == 1000, "round %d: a channel past cap=%u admits every job into overflow (admitted %d, errno %d)", round, cap, acc, e);
        gate_set(1);
        CHECK(drain_poll(ch, acc, 10000, "full") == acc, "round %d: every admitted job completes", round);
        CHECK(dyn_pool_inflight(ch) == 0, "round %d: inflight 0 after drain", round);
    }
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  queue full / admit again: ok\n");
}

static _Atomic int g_seq;
static int g_order[512];
static void w_order(void* arg)
{
    int at = atomic_fetch_add(&g_seq, 1);
    if (at < 512)
        g_order[at] = (int)(intptr_t)arg;
    spin_us(200);
}

static void case_turns(void)
{
    dyn_pool_t* p = dyn_pool_new(4, 256);
    dyn_pool_chan_t* a = dyn_pool_chan_new(p);
    dyn_pool_chan_t* b = dyn_pool_chan_new(p);
    unsigned nt = dyn_pool_threads(p);
    int i, pos = -1, base;
    CHECK(p && a && b, "pool + chans (turns)");
    if (!p || !a || !b)
        return;
    atomic_store(&g_seq, 0);
    gate_set(0);
    for (i = 0; i < (int)nt; i++)
        dyn_pool_submit_cpu(a, w_gate, NULL, NULL);
    for (i = 0; i < 200; i++)
        dyn_pool_submit(a, w_order, NULL, (void*)(intptr_t)1);
    usleep(20000);
    base = atomic_load(&g_seq);
    dyn_pool_submit(b, w_order, NULL, (void*)(intptr_t)2);
    gate_set(1);
    drain_poll(b, 1, 10000, "turns b");
    drain_poll(a, 200 + (int)nt, 20000, "turns a");
    for (i = 0; i < 201 && i < 512; i++)
        if (g_order[i] == 2) {
            pos = i;
            break;
        }
    CHECK(pos >= base && pos - base <= (int)nt * 2, "the late channel's job ran %d turns after it was queued (position %d); expected within %u", pos - base, pos, nt * 2);
    dyn_pool_chan_free(a);
    dyn_pool_chan_free(b);
    dyn_pool_free(p);
    printf("  channel turns with %u threads (late job %d turns after queueing): ok\n", nt, pos - base);
}

static void case_single_wake(void)
{
    unsigned nt = dyn_pool_max_threads();
    dyn_pool_t* p;
    dyn_pool_chan_t* ch;
    dyn_pool_stats_t s0, s1;
    int i, extra = 0;
    if (nt < 3) {
        printf("  single wake: SKIP (fewer than 3 usable cores)\n");
        return;
    }
    p = dyn_pool_new(nt, 16);
    ch = dyn_pool_chan_new(p);
    CHECK(p && ch, "pool + chan (single wake)");
    if (!p || !ch)
        return;
    for (i = 0; i < 5; i++) {
        usleep(80000);
        dyn_pool_stats(p, &s0);
        dyn_pool_submit(ch, w_noise, NULL, NULL);
        drain_poll(ch, 1, 5000, "single wake");
        usleep(30000);
        dyn_pool_stats(p, &s1);
        if (s1.parks - s0.parks > 2)
            extra++;
    }
    CHECK(extra <= 1, "one submit to %u parked workers woke more than one in %d of 5 tries", nt, extra);
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  one submit wakes one parked worker (%u threads): ok\n", nt);
}

static _Atomic int g_live, g_live_max;
static void w_live(void* arg)
{
    int v = atomic_fetch_add(&g_live, 1) + 1, m = atomic_load(&g_live_max);
    (void)arg;
    while (v > m && !atomic_compare_exchange_weak(&g_live_max, &m, v))
        ;
    spin_us(300);
    atomic_fetch_sub(&g_live, 1);
}

typedef struct {
    dyn_pool_t* p;
    int n;
    int got;
} lim_t;
static void* lim_loop(void* arg)
{
    lim_t* L = (lim_t*)arg;
    dyn_pool_chan_t* ch = dyn_pool_chan_new(L->p);
    int sub = 0;
    while (sub < L->n || L->got < sub) {
        while (sub < L->n && dyn_pool_submit(ch, w_live, NULL, NULL) == 0)
            sub++;
        if (wait_fd(ch, 3000) == 0 && dyn_pool_inflight(ch) > 0)
            break;
        L->got += dyn_pool_drain(ch);
    }
    dyn_pool_chan_free(ch);
    return NULL;
}

static void case_io_limit(void)
{
    dyn_pool_t* p = dyn_pool_new(4, 32);
    lim_t L[3];
    pthread_t th[3];
    unsigned nt = dyn_pool_threads(p);
    int i, n = 300 * (int)g_scale;
    atomic_store(&g_live, 0);
    atomic_store(&g_live_max, 0);
    for (i = 0; i < 3; i++) {
        L[i].p = p;
        L[i].n = n;
        L[i].got = 0;
        pthread_create(&th[i], NULL, lim_loop, &L[i]);
    }
    for (i = 0; i < 3; i++)
        pthread_join(th[i], NULL);
    for (i = 0; i < 3; i++)
        CHECK(L[i].got == n, "io-limit producer %d drained %d of %d", i, L[i].got, n);
    CHECK(atomic_load(&g_live_max) <= (int)nt + 64, "blocking jobs ran %d at once; the limit is %u threads + 64 elastic", atomic_load(&g_live_max), nt);
    CHECK(nt < 2 || atomic_load(&g_live_max) >= 2, "blocking jobs never overlapped (max %d)", atomic_load(&g_live_max));
    dyn_pool_free(p);
    printf("  blocking limit under 3 producers (max %d of %u threads): ok\n", atomic_load(&g_live_max), nt);
}

static int g_gate2;
static void w_gate2(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_gate_mu);
    while (!g_gate2)
        pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    pthread_mutex_unlock(&g_gate_mu);
}
static void gate2_set(int v)
{
    pthread_mutex_lock(&g_gate_mu);
    g_gate2 = v;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}
static _Atomic int g_x_done;
static void w_x(void* arg)
{
    (void)arg;
    atomic_fetch_add(&g_x_done, 1);
}
static int wait_x(int want)
{
    uint64_t t0 = now_ms();
    while (atomic_load(&g_x_done) < want && now_ms() - t0 < 2000)
        usleep(200);
    return atomic_load(&g_x_done) >= want;
}

static void case_spare(void)
{
    int round, stranded = 0, starved = 0, rounds = 20 * (int)g_scale;
    for (round = 0; round < rounds; round++) {
        dyn_pool_t* p = dyn_pool_new(2, 16);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        CHECK(p && ch, "pool + chan (spare)");
        if (!p || !ch)
            return;
        if (dyn_pool_threads(p) != 2) {
            dyn_pool_chan_free(ch);
            dyn_pool_free(p);
            printf("  spare worker: SKIP (fewer than 2 usable cores)\n");
            return;
        }
        gate_set(0);
        gate2_set(0);
        atomic_store(&g_x_done, 0);
        dyn_pool_submit_cpu(ch, w_gate, NULL, NULL);
        dyn_pool_submit_cpu(ch, w_gate, NULL, NULL);
        usleep(2000);
        dyn_pool_submit(ch, w_gate2, NULL, NULL);
        dyn_pool_submit(ch, w_gate2, NULL, NULL);
        dyn_pool_submit_cpu(ch, w_x, NULL, NULL);
        gate_set(1);
        if (!wait_x(1))
            stranded++;
        dyn_pool_submit_cpu(ch, w_x, NULL, NULL);
        if (!wait_x(2))
            starved++;
        gate2_set(1);
        drain_poll(ch, 6, 10000, "spare");
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }
    {
        dyn_pool_t* p = dyn_pool_new(2, 16);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        int k, acc = 0;
        atomic_store(&g_live, 0);
        atomic_store(&g_live_max, 0);
        for (k = 0; k < 16; k++)
            acc += dyn_pool_submit_cpu(ch, w_live, NULL, NULL) == 0;
        drain_poll(ch, acc, 10000, "spare cpu cap");
        CHECK(atomic_load(&g_live_max) <= 2, "cpu jobs ran %d at once on a 2-thread pool with no blocking job (the spare must stay out)", atomic_load(&g_live_max));
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }
    CHECK(stranded == 0, "a cpu job queued before both threads entered blocking jobs waited 2 s in %d of %d rounds (no spare kick at saturation)", stranded, rounds);
    CHECK(starved == 0, "a cpu job submitted while both threads held blocking jobs waited 2 s in %d of %d rounds (no spare kick on submit)", starved, rounds);
    printf("  spare worker runs cpu jobs while every thread holds a blocking job (%d rounds): ok\n", rounds);
}

static void note_notify(void* ctx)
{
    atomic_fetch_add((_Atomic int*)ctx, 1);
}

static void case_many_chans(void)
{
    enum { NCH = 1100 };
    dyn_pool_t* p = dyn_pool_new(2, 8);
    dyn_pool_chan_t** chs = (dyn_pool_chan_t**)calloc(NCH, sizeof(*chs));
    static _Atomic int notes;
    unsigned seed = 777u, round, rounds = 300u * g_scale;
    int i, stranded = 0, made = 0;
    CHECK(p && chs, "pool + channel table (many channels)");
    if (!p || !chs)
        return;
    errno = 0;
    CHECK(dyn_pool_chan_new_notify(p, NULL, NULL) == NULL && errno == EINVAL, "a notify channel without a callback is refused with EINVAL (errno %d)", errno);
    {
        static _Atomic int one;
        dyn_pool_chan_t* c1 = dyn_pool_chan_new_notify(p, note_notify, (void*)&one);
        int r, missed = 0;
        atomic_store(&one, 0);
        for (r = 0; r < 50 && c1 && !missed; r++) {
            int before = atomic_load(&one);
            uint64_t t0 = now_ms();
            dyn_pool_submit(c1, NULL, NULL, NULL);
            while (atomic_load(&one) == before && now_ms() - t0 < 2000)
                usleep(50);
            if (atomic_load(&one) == before)
                missed++;
            t0 = now_ms();
            while (dyn_pool_drain(c1) == 0 && now_ms() - t0 < 2000)
                usleep(50);
        }
        CHECK(c1 && missed == 0, "a notify channel stayed silent after drain %d of 50 (the drain must re-arm it)", r);
        dyn_pool_chan_free(c1);
    }
    atomic_store(&notes, 0);
    for (i = 0; i < NCH; i++)
        made += (chs[i] = dyn_pool_chan_new_notify(p, note_notify, (void*)&notes)) != NULL;
    CHECK(made == NCH, "created %d of %d notify channels", made, NCH);
    CHECK(made == 0 || dyn_pool_wake_fd(chs[0]) == -1, "a notify channel has no wake fd");
    for (round = 0; round < rounds && made == NCH; round++) {
        int pick[16], k, got = 0, want = 0;
        uint64_t t0;
        for (k = 0; k < 16; k++) {
            seed = seed * 1103515245u + 12345u;
            pick[k] = (int)((seed >> 8) % NCH);
            if ((k & 1 ? dyn_pool_submit_cpu(chs[pick[k]], NULL, NULL, NULL) : dyn_pool_submit(chs[pick[k]], NULL, NULL, NULL)) == 0)
                want++;
        }
        t0 = now_ms();
        while (got < want && now_ms() - t0 < 3000) {
            for (k = 0; k < 16; k++)
                got += dyn_pool_drain(chs[pick[k]]);
            if (got < want)
                usleep(50);
        }
        if (got < want) {
            stranded++;
            break;
        }
        if ((round & 7u) == 0)
            usleep((seed >> 20) % 400u);
    }
    CHECK(stranded == 0, "a job on one of %d channels waited 3 s at round %u (lost by the active-channel bitmap)", NCH, round);
    CHECK(atomic_load(&notes) > 0, "the notify callback never ran");
    for (i = 0; i < NCH; i++)
        dyn_pool_chan_free(chs[i]);
    free(chs);
    dyn_pool_free(p);
    printf("  %u rounds of 16 lone jobs over %d notify channels (bitmap words and pages): ok\n", round, NCH);
}

static void case_teardown(void)
{
    int r, bad = 0;
    for (r = 0; r < 40 * (int)g_scale; r++) {
        dyn_pool_t* p = dyn_pool_new(3, 16);
        dyn_pool_chan_t* a = dyn_pool_chan_new(p);
        dyn_pool_chan_t* b = dyn_pool_chan_new(p);
        int i, sub = 0;
        atomic_store(&g_work, 0);
        for (i = 0; i < 16; i++) {
            sub += dyn_pool_submit(a, w_slow, d_count, (void*)(uintptr_t)(r % 30)) == 0;
            sub += dyn_pool_submit_cpu(b, w_slow, d_count, (void*)(uintptr_t)(r % 30)) == 0;
        }
        if (r & 1)
            dyn_pool_chan_free(a);
        dyn_pool_free(p);
        if (atomic_load(&g_work) != sub)
            bad++;
    }
    CHECK(bad == 0, "%d pool teardowns skipped queued work", bad);
    printf("  teardown with queued work and live channels: ok\n");
}

static void case_park_race(unsigned nthreads)
{
    dyn_pool_t* p = dyn_pool_new(nthreads, 8);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    unsigned i, n = 4000u * g_scale, lost = 0, seed = 12345u;
    unsigned stray = 0, late = 0;
    CHECK(p && ch, "pool + chan (park race)");
    if (!p || !ch)
        return;
    for (i = 0; i < n && !lost; i++) {
        int got = 0;
        if (dyn_pool_submit(ch, NULL, NULL, NULL) != 0) {
            lost = 1000000u;
            break;
        }
        if (seed & 0x100u) {
            uint64_t t0 = now_ms();
            while (!got && now_ms() - t0 < 2000)
                got = dyn_pool_drain(ch);
            if (!got)
                lost++;
        }
        while (!got) {
            if (wait_fd(ch, 2000) == 0) {
                lost++;
                break;
            }
            got = dyn_pool_drain(ch);
        }
        if (i % 16u == 0) {
            usleep(300);
            dyn_pool_drain(ch);
            if (wait_fd(ch, 0) != 0) {
                late++;
                dyn_pool_drain(ch);
                if (wait_fd(ch, 0) != 0)
                    stray++;
            }
        }
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 4u == 0)
            spin_us((seed >> 20) % 30u);
    }
    CHECK(lost == 0, "lost wakeup: a lone job waited 2 s at round %u of %u (%u thread pool)", i, n, nthreads);
    CHECK(stray == 0, "the wake fd stayed readable after two drains with nothing pending in %u checks (a byte with the flag clear: the loop would spin)", stray);
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  park race: %u lone jobs on a %u-thread pool, each submitted as the worker parks (%u late wake bytes, each cleared by the next drain): ok\n", i, nthreads, late);
}

static int want(const char* name)
{
    const char* o = getenv("POOL_STRESS_ONLY");
    return !o || !*o || strstr(o, name) != NULL;
}

static void on_alarm(int sig)
{
    static const char m[] = "FAIL watchdog: test_pool_stress hung (lost wakeup or deadlock)\n";
    (void)sig;
    if (write(1, m, sizeof(m) - 1) < 0)
        _exit(3);
    _exit(2);
}

int main(void)
{
    const char* s = getenv("POOL_STRESS_SCALE");
    uint64_t t0 = now_ms();
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (s && atoi(s) > 0)
        g_scale = (unsigned)atoi(s);
    signal(SIGALRM, on_alarm);
    alarm(240);
    if (want("park")) {
        case_park_race(1);
        case_park_race(2);
    }
    if (want("producers"))
        case_producers();
    if (want("close"))
        case_close_in_flight();
    if (want("slice"))
        case_drain_slice();
    if (want("full"))
        case_queue_full();
    if (want("turns"))
        case_turns();
    if (want("single"))
        case_single_wake();
    if (want("iolimit"))
        case_io_limit();
    if (want("spare"))
        case_spare();
    if (want("many"))
        case_many_chans();
    if (want("teardown"))
        case_teardown();
    if (fails == 0)
        printf("test_pool_stress: ok (%llu ms, scale %u)\n", (unsigned long long)(now_ms() - t0), g_scale);
    else
        printf("test_pool_stress: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
