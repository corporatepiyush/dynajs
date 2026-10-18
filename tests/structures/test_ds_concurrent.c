/* Cross-thread structures of dyn-ds.c: the parking lot (address-keyed wait queue with a
 * validate-under-lock futex contract), the bounded MPMC ring and the Chase-Lev work-stealing
 * deque. Built under ThreadSanitizer and under AddressSanitizer by the test-ds-concurrent
 * target. Every check is on the RESULT -- exact counts, per-producer FIFO order, each task run
 * exactly once, a non-atomic counter guarded only by a park-based mutex -- so a broken memory
 * order shows as a wrong number even where the sanitizer is absent. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dyn-ds.c"

static int failures;
static long checks;
#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            if (++failures > 25)                        \
                exit(1);                                \
        }                                               \
    } while (0)

#define RING_PRODUCERS 4
#define RING_CONSUMERS 4
#define RING_PER_PRODUCER 250000

static dyn_ring_t* g_ring;
static atomic_long g_ring_consumed;
static atomic_ullong g_ring_sum;
static atomic_int g_ring_order_bad;

static void* ring_producer(void* arg)
{
    uint64_t id = (uint64_t)(uintptr_t)arg, i;
    for (i = 0; i < RING_PER_PRODUCER; i++) {
        uint64_t v = (id << 32) | i;
        while (dyn_ring_try_push(g_ring, &v) != 0)
            ;
    }
    return NULL;
}

static void* ring_consumer(void* arg)
{
    uint64_t last[RING_PRODUCERS];
    int i;
    (void)arg;
    for (i = 0; i < RING_PRODUCERS; i++)
        last[i] = UINT64_MAX;
    while (atomic_load(&g_ring_consumed) < (long)RING_PRODUCERS * RING_PER_PRODUCER) {
        uint64_t v;
        if (dyn_ring_try_pop(g_ring, &v) != 0)
            continue;
        {
            uint64_t p = v >> 32, s = v & 0xFFFFFFFFu;
            if (last[p] != UINT64_MAX && s <= last[p])
                atomic_fetch_add(&g_ring_order_bad, 1);
            last[p] = s;
        }
        atomic_fetch_add(&g_ring_sum, v & 0xFFFFFFFFu);
        atomic_fetch_add(&g_ring_consumed, 1);
    }
    return NULL;
}

static void test_ring(void)
{
    pthread_t p[RING_PRODUCERS], c[RING_CONSUMERS];
    uint64_t v = 7, out;
    int i;
    dyn_ring_t* r = dyn_ring_new(8, 3);
    CHECK(r && dyn_ring_capacity(r) == 4, "capacity rounds up to a power of two");
    for (i = 0; i < 4; i++)
        CHECK(dyn_ring_try_push(r, &v) == 0, "push into free slots");
    CHECK(dyn_ring_try_push(r, &v) == -1, "push into a full ring fails");
    for (i = 0; i < 4; i++)
        CHECK(dyn_ring_try_pop(r, &out) == 0 && out == 7, "pop");
    CHECK(dyn_ring_try_pop(r, &out) == -1, "pop from an empty ring fails");
    dyn_ring_free(r);
    g_ring = dyn_ring_new(8, 1024);
    for (i = 0; i < RING_CONSUMERS; i++)
        pthread_create(&c[i], NULL, ring_consumer, NULL);
    for (i = 0; i < RING_PRODUCERS; i++)
        pthread_create(&p[i], NULL, ring_producer, (void*)(uintptr_t)i);
    for (i = 0; i < RING_PRODUCERS; i++)
        pthread_join(p[i], NULL);
    for (i = 0; i < RING_CONSUMERS; i++)
        pthread_join(c[i], NULL);
    {
        unsigned long long want = (unsigned long long)RING_PRODUCERS * ((unsigned long long)RING_PER_PRODUCER * (RING_PER_PRODUCER - 1) / 2);
        CHECK(atomic_load(&g_ring_consumed) == (long)RING_PRODUCERS * RING_PER_PRODUCER, "every item consumed exactly once");
        CHECK(atomic_load(&g_ring_sum) == want, "consumed sum %llu want %llu", (unsigned long long)atomic_load(&g_ring_sum), want);
        CHECK(atomic_load(&g_ring_order_bad) == 0, "each consumer sees each producer in FIFO order (%d violations)", atomic_load(&g_ring_order_bad));
    }
    dyn_ring_free(g_ring);
    printf("ring ok\n");
}

#define WS_TASKS 200000
#define WS_THIEVES 4

static dyn_wsdeque_t* g_dq;
static atomic_uchar g_ran[WS_TASKS];
static atomic_int g_ws_done;
static atomic_long g_ws_stolen;

static void ws_run(uint64_t id)
{
    atomic_fetch_add(&g_ran[id], 1);
}

static void* ws_thief(void* arg)
{
    (void)arg;
    while (!atomic_load(&g_ws_done) || dyn_wsdeque_size_approx(g_dq)) {
        uint64_t x;
        int r = dyn_wsdeque_steal(g_dq, &x);
        if (r == 0) {
            ws_run(x);
            atomic_fetch_add(&g_ws_stolen, 1);
        }
    }
    return NULL;
}

static void test_wsdeque(void)
{
    pthread_t th[WS_THIEVES];
    uint64_t i, x;
    int t;
    long dup = 0, missing = 0;
    dyn_wsdeque_t* q = dyn_wsdeque_new(4);
    CHECK(dyn_wsdeque_pop(q, &x) == -1 && dyn_wsdeque_steal(q, &x) == -1, "an empty deque");
    for (i = 0; i < 100; i++)
        dyn_wsdeque_push(q, i);
    CHECK(dyn_wsdeque_pop(q, &x) == 0 && x == 99, "pop is LIFO for the owner");
    CHECK(dyn_wsdeque_steal(q, &x) == 0 && x == 0, "steal is FIFO for thieves");
    dyn_wsdeque_free(q);
    g_dq = dyn_wsdeque_new(16);
    for (t = 0; t < WS_THIEVES; t++)
        pthread_create(&th[t], NULL, ws_thief, NULL);
    for (i = 0; i < WS_TASKS; i++) {
        dyn_wsdeque_push(g_dq, i);
        if (i % 3 == 0 && dyn_wsdeque_pop(g_dq, &x) == 0)
            ws_run(x);
    }
    while (dyn_wsdeque_pop(g_dq, &x) == 0)
        ws_run(x);
    atomic_store(&g_ws_done, 1);
    for (t = 0; t < WS_THIEVES; t++)
        pthread_join(th[t], NULL);
    while (dyn_wsdeque_pop(g_dq, &x) == 0)
        ws_run(x);
    for (i = 0; i < WS_TASKS; i++) {
        unsigned char c = atomic_load(&g_ran[i]);
        if (c > 1)
            dup++;
        if (c == 0)
            missing++;
    }
    CHECK(dup == 0 && missing == 0, "every task ran exactly once (%ld duplicated, %ld missing)", dup, missing);
    printf("wsdeque: %ld of %d tasks stolen\n", atomic_load(&g_ws_stolen), WS_TASKS);
    CHECK(atomic_load(&g_ws_stolen) > 0, "thieves stole something");
    dyn_wsdeque_free(g_dq);
    printf("wsdeque ok\n");
}

#define WS_RACE_ROUNDS 300000
static dyn_wsdeque_t* g_last;
static atomic_int g_race_stop;
static atomic_uint g_taken[WS_RACE_ROUNDS];

static void* ws_last_thief(void* arg)
{
    (void)arg;
    while (!atomic_load_explicit(&g_race_stop, memory_order_relaxed)) {
        uint64_t x;
        if (dyn_wsdeque_steal(g_last, &x) == 0)
            atomic_fetch_add(&g_taken[x], 1);
    }
    return NULL;
}

static void test_ws_last_element(void)
{
    pthread_t th[3];
    uint64_t i, x;
    long dup = 0, lost = 0;
    int t;
    g_last = dyn_wsdeque_new(16);
    for (t = 0; t < 3; t++)
        pthread_create(&th[t], NULL, ws_last_thief, NULL);
    for (i = 0; i < WS_RACE_ROUNDS; i++) {
        dyn_wsdeque_push(g_last, i);
        if (dyn_wsdeque_pop(g_last, &x) == 0)
            atomic_fetch_add(&g_taken[x], 1);
    }
    atomic_store(&g_race_stop, 1);
    for (t = 0; t < 3; t++)
        pthread_join(th[t], NULL);
    while (dyn_wsdeque_pop(g_last, &x) == 0)
        atomic_fetch_add(&g_taken[x], 1);
    for (i = 0; i < WS_RACE_ROUNDS; i++) {
        unsigned c = atomic_load(&g_taken[i]);
        dup += c > 1;
        lost += c == 0;
    }
    CHECK(dup == 0 && lost == 0, "last-element contention: %ld taken twice, %ld lost over %d rounds", dup, lost, WS_RACE_ROUNDS);
    dyn_wsdeque_free(g_last);
    printf("wsdeque last-element contention ok\n");
}

static atomic_int g_word;
static int park_validate_eq(void* ud)
{
    return atomic_load(&g_word) == *(int*)ud;
}

typedef struct {
    atomic_int state;
} park_mutex;

static int park_validate_state(void* ud);

static void pm_lock(park_mutex* m)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&m->state, &expected, 1))
        return;
    for (;;) {
        int two = 2;
        expected = 0;
        if (atomic_exchange(&m->state, 2) == 0)
            return;
        (void)expected;
        dyn_park_wait(&m->state, park_validate_state, &two, -1);
    }
}

static int park_validate_state_impl(atomic_int* s, int want)
{
    return atomic_load(s) == want;
}

static park_mutex g_pm;
static long g_plain_counter;
static int park_validate_state(void* ud)
{
    return park_validate_state_impl(&g_pm.state, *(int*)ud);
}

static void pm_unlock(park_mutex* m)
{
    if (atomic_exchange(&m->state, 0) == 2)
        dyn_park_wake(&m->state, 1);
}

static void* pm_worker(void* arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 50000; i++) {
        pm_lock(&g_pm);
        g_plain_counter++;
        pm_unlock(&g_pm);
    }
    return NULL;
}

static atomic_int g_parked_rc[8];
static void* park_waiter(void* arg)
{
    int idx = (int)(uintptr_t)arg, want = 0;
    atomic_store(&g_parked_rc[idx], dyn_park_wait(&g_word, park_validate_eq, &want, -1));
    return NULL;
}

static void test_park(void)
{
    int want = 5, i;
    pthread_t th[8];
    atomic_store(&g_word, 0);
    CHECK(dyn_park_wait(&g_word, park_validate_eq, &want, -1) == -1, "a failed validation does not park");
    {
        struct timespec a, b;
        int zero = 0;
        int64_t ns;
        clock_gettime(CLOCK_MONOTONIC, &a);
        CHECK(dyn_park_wait(&g_word, park_validate_eq, &zero, 30000000) == 1, "a timeout returns 1");
        clock_gettime(CLOCK_MONOTONIC, &b);
        ns = (int64_t)(b.tv_sec - a.tv_sec) * 1000000000 + (b.tv_nsec - a.tv_nsec);
        CHECK(ns >= 30000000, "the wait lasted at least the timeout (%lld ns)", (long long)ns);
        CHECK(dyn_park_waiters(&g_word) == 0, "a timed-out waiter unlinks itself");
    }
    for (i = 0; i < 8; i++)
        pthread_create(&th[i], NULL, park_waiter, (void*)(uintptr_t)i);
    while (dyn_park_waiters(&g_word) < 8)
        ;
    {
        int other = 0;
        CHECK(dyn_park_wake(&other, 8) == 0, "waking another address wakes nobody");
    }
    CHECK(dyn_park_wake(&g_word, 3) == 3, "wake 3 wakes 3");
    CHECK(dyn_park_waiters(&g_word) == 5, "5 still parked");
    CHECK(dyn_park_wake(&g_word, 100) == 5, "wake 100 wakes the remaining 5");
    for (i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
        CHECK(atomic_load(&g_parked_rc[i]) == 0, "waiter %d returned woken", i);
    }
    {
        pthread_t w[4];
        atomic_init(&g_pm.state, 0);
        for (i = 0; i < 4; i++)
            pthread_create(&w[i], NULL, pm_worker, NULL);
        for (i = 0; i < 4; i++)
            pthread_join(w[i], NULL);
        CHECK(g_plain_counter == 200000, "a park-based mutex guards a plain counter (%ld)", g_plain_counter);
    }
    printf("park ok\n");
}

int main(void)
{
    test_ring();
    test_wsdeque();
    test_ws_last_element();
    test_park();
    printf("%s: %ld checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
