/* CPU work offloaded through the reactor never waits behind blocking pool jobs:
   the pool keeps one thread free of them, and on a one-thread pool the offload
   is refused (or, if already parked, run on the reactor thread) instead. */
#include "dyna-aio.h"
#include "dyn-pool.h"

#if defined(CONFIG_NATIVE_MODULES)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static pthread_mutex_t gate_io = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate_cpu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int io_done, cpu_done, tagged_ran, tagged_done;
static pthread_t tagged_thread;

static int io_started;
static void w_io(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    io_started++;
    pthread_mutex_unlock(&mu);
    pthread_mutex_lock(&gate_io);
    pthread_mutex_unlock(&gate_io);
}

static int io_started_now(void)
{
    int n;
    pthread_mutex_lock(&mu);
    n = io_started;
    pthread_mutex_unlock(&mu);
    return n;
}
static void w_cpu(void *arg) { (void)arg; pthread_mutex_lock(&gate_cpu); pthread_mutex_unlock(&gate_cpu); }
static void d_io(void *arg) { (void)arg; io_done++; }
static void d_cpu(void *arg) { (void)arg; cpu_done++; }
static void w_tagged(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    tagged_ran++;
    tagged_thread = pthread_self();
    pthread_mutex_unlock(&mu);
}
static void d_tagged(void *arg) { (void)arg; tagged_done++; }

static void pump(dyn_aio_t *a, dyn_aio_t *b, const int *counter, int want)
{
    int spins = 0;
    while (*counter < want && spins++ < 1000) {
        dyn_aio_run(a, 5);
        if (b)
            dyn_aio_run(b, 5);
    }
}

int main(void)
{
    dyn_aio_t *a, *b;
    int i, ok;

    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Two pool threads: blocking jobs can hold only one of them. */
    dyn_pool_set_default_threads(2);
    if (dyn_pool_default_threads() == 2) {
        a = dyn_aio_new(256, 0);
        b = dyn_aio_new(256, 0);
        CHECK(a && b, "two reactors");
        if (!a || !b)
            return 1;
        CHECK(dyn_aio_pool_warm(a) == 0 && dyn_aio_pool_warm(b) == 0, "both reach the shared pool");
        pthread_mutex_lock(&gate_io);
        CHECK(dyn_aio_offload(a, w_io, d_io, NULL) == 0 && dyn_aio_offload(b, w_io, d_io, NULL) == 0
                  && dyn_aio_offload(b, w_io, d_io, NULL) == 0,
              "three blocking jobs queued from two reactors");
        for (i = 0; i < 200 && io_started_now() < 1; i++)
            usleep(5000);
        usleep(150000);
        CHECK(io_started_now() == 1, "exactly one blocking job runs on a two-thread pool (%d)", io_started_now());
        CHECK(dyn_aio_cpu_room(a) == 1, "room is still reported");
        CHECK(dyn_aio_offload_cpu(a, w_tagged, d_tagged, NULL) == 0, "the cpu offload is taken");
        pump(a, b, &tagged_done, 1);
        CHECK(tagged_ran == 1 && tagged_done == 1,
              "and completes while every blocking job is still held (%d/%d)", tagged_ran, tagged_done);
        CHECK(!pthread_equal(tagged_thread, pthread_self()), "on a pool thread");
        CHECK(io_done == 0, "no blocking job has finished (%d)", io_done);
        pthread_mutex_unlock(&gate_io);
        pump(a, b, &io_done, 3);
        CHECK(io_done == 3, "the blocking jobs complete after release (%d)", io_done);
        dyn_aio_free(a);
        dyn_aio_free(b);
    } else {
        printf("test_aio_cpu_offload: SKIP the two-thread case (one usable core)\n");
    }

    /* One pool thread: nothing can be reserved, so cpu work is refused while a
       blocking job holds the thread, and a cpu job already parked when that
       happens is run on the reactor thread. */
    io_done = cpu_done = tagged_ran = tagged_done = 0;
    dyn_pool_set_default_threads(1);
    a = dyn_aio_new(256, 0);
    b = dyn_aio_new(256, 0);
    CHECK(a && b, "two reactors (one thread)");
    if (!a || !b)
        return 1;
    CHECK(dyn_aio_pool_warm(a) == 0 && dyn_aio_pool_warm(b) == 0, "both reach the one-thread pool");

    pthread_mutex_lock(&gate_io);
    CHECK(dyn_aio_offload(a, w_io, d_io, NULL) == 0, "a blocking job queued");
    CHECK(dyn_aio_cpu_room(a) == 0, "no room is reported");
    CHECK(dyn_aio_offload_cpu(a, w_tagged, d_tagged, NULL) != 0, "the cpu offload is refused");
    CHECK(dyn_aio_offload_cpu(b, w_tagged, d_tagged, NULL) != 0,
          "and refused on another reactor sharing the pool");
    pthread_mutex_unlock(&gate_io);
    pump(a, b, &io_done, 1);
    CHECK(io_done == 1 && tagged_ran == 0, "the refused cpu work never ran (%d)", tagged_ran);

    pthread_mutex_lock(&gate_cpu);
    for (ok = 0, i = 0; i < 24 && dyn_aio_offload_cpu(a, w_cpu, d_cpu, NULL) == 0; i++)
        ok++;
    CHECK(ok == 24, "cpu jobs beyond the channel budget are parked, not refused (%d of 24)", ok);
    CHECK(dyn_aio_offload_cpu(a, w_tagged, d_tagged, NULL) == 0, "the tagged cpu job parks behind them");
    pthread_mutex_lock(&gate_io);
    CHECK(dyn_aio_offload(b, w_io, d_io, NULL) == 0, "the other reactor queues a blocking job");
    pthread_mutex_unlock(&gate_cpu);
    pump(a, b, &tagged_done, 1);
    CHECK(tagged_done == 1 && tagged_ran == 1, "the parked job ran (%d/%d)", tagged_ran, tagged_done);
    CHECK(pthread_equal(tagged_thread, pthread_self()),
          "on the reactor thread, because the pool's only thread held a blocking job");
    pthread_mutex_unlock(&gate_io);
    pump(a, b, &io_done, 2);
    pump(a, b, &cpu_done, 24);
    CHECK(io_done == 2, "the blocking job completed after release (%d)", io_done);
    CHECK(cpu_done == 24, "all 24 cpu jobs completed (%d)", cpu_done);

    dyn_aio_free(a);
    dyn_aio_free(b);
    if (fails == 0)
        printf("test_aio_cpu_offload: all tests passed\n");
    else
        printf("test_aio_cpu_offload: %d FAILED\n", fails);
    return fails != 0;
}

#else
int main(void) { return 0; }
#endif
