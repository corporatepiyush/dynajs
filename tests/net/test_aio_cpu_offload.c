/* CPU work offloaded through the reactor never waits behind blocking pool jobs and never runs on the reactor
   thread: blocking jobs beyond the pool threads run on elastic threads, and every pool (one thread included) runs
   cpu work on its spare thread while all regular threads block. */
#include "dyna-aio.h"
#include "dyn-pool.h"

#if defined(CONFIG_NATIVE_MODULES)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...)            \
    do {                         \
        if (!(c)) {              \
            printf("FAIL: ");    \
            printf(__VA_ARGS__); \
            printf("\n");        \
            fails++;             \
        }                        \
    } while (0)

static pthread_mutex_t gate_io = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate_cpu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int io_done, cpu_done, tagged_ran, tagged_done;
static pthread_t tagged_thread;

static int io_started;
static void w_io(void* arg)
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
static void w_cpu(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&gate_cpu);
    pthread_mutex_unlock(&gate_cpu);
}
static void d_io(void* arg)
{
    (void)arg;
    io_done++;
}
static void d_cpu(void* arg)
{
    (void)arg;
    cpu_done++;
}
static void w_tagged(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    tagged_ran++;
    tagged_thread = pthread_self();
    pthread_mutex_unlock(&mu);
}
static void d_tagged(void* arg)
{
    (void)arg;
    tagged_done++;
}

static void pump(dyn_aio_t* a, dyn_aio_t* b, const int* counter, int want)
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

    /* Two pool threads: blocking jobs hold both, the spare thread takes the cpu job. */
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
        for (i = 0; i < 200 && io_started_now() < 3; i++)
            usleep(5000);
        CHECK(io_started_now() == 3, "all three blocking jobs run at once: two pool threads + one elastic (%d)", io_started_now());
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

    /* One pool thread: cpu work is still admitted while a blocking job holds the thread and runs on the spare. */
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
    CHECK(dyn_aio_cpu_room(a) == 1, "room is reported while the only thread blocks");
    CHECK(dyn_aio_offload_cpu(a, w_tagged, d_tagged, NULL) == 0, "the cpu offload is admitted");
    pump(a, b, &tagged_done, 1);
    CHECK(tagged_ran == 1 && !pthread_equal(tagged_thread, pthread_self()),
        "and runs on a pool thread while the blocking job is held (%d)", tagged_ran);
    pthread_mutex_unlock(&gate_io);
    pump(a, b, &io_done, 1);
    CHECK(io_done == 1, "the blocking job completes (%d)", io_done);
    tagged_ran = tagged_done = 0;

    pthread_mutex_lock(&gate_cpu);
    for (ok = 0, i = 0; i < 24 && dyn_aio_offload_cpu(a, w_cpu, d_cpu, NULL) == 0; i++)
        ok++;
    CHECK(ok == 24, "cpu jobs beyond the channel budget wait in the pool overflow, not refused (%d of 24)", ok);
    CHECK(dyn_aio_offload_cpu(a, w_tagged, d_tagged, NULL) == 0, "the tagged cpu job queues behind them");
    pthread_mutex_lock(&gate_io);
    CHECK(dyn_aio_offload(b, w_io, d_io, NULL) == 0, "the other reactor queues a blocking job");
    pthread_mutex_unlock(&gate_cpu);
    pump(a, b, &tagged_done, 1);
    CHECK(tagged_done == 1 && tagged_ran == 1, "the queued job ran (%d/%d)", tagged_ran, tagged_done);
    CHECK(!pthread_equal(tagged_thread, pthread_self()),
        "on a pool thread, never the reactor thread, though the only regular thread holds a blocking job");
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
