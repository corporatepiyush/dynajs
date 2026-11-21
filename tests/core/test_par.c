/* dyn-par: fork-join with a mutex+cond barrier, a process-wide helper budget and
 * spawn-failure fallback. Built plain (ASan+UBSan) and under TSan by `tests/ctest/run.sh test-par`.
 * Checks: plan bounds (inline-only, cap, units, --io-threads override), every worker index runs
 * exactly once, barrier generations over many rounds (a worker past the barrier must see every
 * write of the round before it), spawn-limit injection 0/1/2 gives 1/2/3 workers and the same
 * partitioned answer, nested runs are serial, and 4 concurrent callers never hold more helpers
 * than the budget (live_peak) and leave live at 0. */
#include "dyn-par.h"
#include "dyn-pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define ROUNDS 2000
#define MAXW 64

typedef struct {
    _Atomic unsigned seen[MAXW];
    _Atomic unsigned nseen;
    unsigned nw_mismatch;
    _Atomic unsigned errors;
    unsigned* grid;
    unsigned nw;
} gen_ctx;

static void gen_fn(void* arg, unsigned w, unsigned n, dyn_par_t* par)
{
    gen_ctx* g = (gen_ctx*)arg;
    unsigned r, k;
    atomic_fetch_add(&g->seen[w], 1);
    atomic_fetch_add(&g->nseen, 1);
    if (w == 0)
        g->nw = n;
    for (r = 0; r < ROUNDS; r++) {
        __atomic_store_n(&g->grid[(size_t)r * MAXW + w], r + 1, __ATOMIC_RELAXED);
        dyn_par_barrier(par);
        for (k = 0; k < n; k++)
            if (__atomic_load_n(&g->grid[(size_t)r * MAXW + k], __ATOMIC_RELAXED) != r + 1)
                atomic_fetch_add(&g->errors, 1);
        dyn_par_barrier(par);
    }
}

static void run_generations(unsigned want, int limit, unsigned expect)
{
    gen_ctx g;
    unsigned got, i;
    memset(&g, 0, sizeof g);
    g.grid = calloc((size_t)ROUNDS * MAXW, sizeof(unsigned));
    dyn_par_set_spawn_limit(limit);
    got = dyn_par_run(want, gen_fn, &g);
    dyn_par_set_spawn_limit(-1);
    CHECK(got == expect, "want %u limit %d: got %u workers, expected %u", want, limit, got, expect);
    CHECK(g.nw == got, "worker 0 saw nworkers %u, run returned %u", g.nw, got);
    CHECK(atomic_load(&g.nseen) == got, "fn ran %u times for %u workers", atomic_load(&g.nseen), got);
    for (i = 0; i < got; i++)
        CHECK(atomic_load(&g.seen[i]) == 1, "worker %u ran %u times", i, atomic_load(&g.seen[i]));
    CHECK(atomic_load(&g.errors) == 0, "barrier let %u reads see an unfinished round (n=%u)", atomic_load(&g.errors), got);
    free(g.grid);
}

typedef struct {
    uint64_t part[MAXW];
    uint64_t total;
    uint64_t n;
} sum_ctx;

static void sum_fn(void* arg, unsigned w, unsigned n, dyn_par_t* par)
{
    sum_ctx* s = (sum_ctx*)arg;
    uint64_t lo = s->n * w / n, hi = s->n * (w + 1) / n, i, acc = 0;
    for (i = lo; i < hi; i++)
        acc += (i * 2654435761u) ^ (i >> 3);
    s->part[w] = acc;
    dyn_par_barrier(par);
    if (w == 0) {
        unsigned k;
        s->total = 0;
        for (k = 0; k < n; k++)
            s->total += s->part[k];
    }
}

static uint64_t sum_with(unsigned want, int limit, unsigned* used)
{
    sum_ctx s;
    memset(&s, 0, sizeof s);
    s.n = 1000003;
    dyn_par_set_spawn_limit(limit);
    *used = dyn_par_run(want, sum_fn, &s);
    dyn_par_set_spawn_limit(-1);
    return s.total;
}

static _Atomic unsigned nested_plan, nested_used;

static void nested_inner(void* arg, unsigned w, unsigned n, dyn_par_t* par)
{
    (void)arg;
    (void)w;
    (void)par;
    atomic_fetch_add(&nested_used, n);
}

static void nested_outer(void* arg, unsigned w, unsigned n, dyn_par_t* par)
{
    (void)arg;
    (void)w;
    (void)n;
    (void)par;
    atomic_fetch_add(&nested_plan, dyn_par_plan(1u << 30, 1, 0));
    dyn_par_run(4, nested_inner, NULL);
}

static _Atomic unsigned budget_bad;

static void budget_fn(void* arg, unsigned w, unsigned n, dyn_par_t* par)
{
    dyn_par_stats_t st;
    unsigned r;
    (void)arg;
    (void)w;
    (void)n;
    for (r = 0; r < 200; r++) {
        dyn_par_stats(&st);
        if (st.live > st.budget)
            atomic_fetch_add(&budget_bad, 1);
        dyn_par_barrier(par);
    }
}

static void* budget_caller(void* arg)
{
    unsigned i;
    (void)arg;
    for (i = 0; i < 20; i++)
        dyn_par_run(64, budget_fn, NULL);
    return NULL;
}

int main(void)
{
    dyn_par_stats_t st;
    unsigned maxw = dyn_pool_max_threads(), used, i;
    uint64_t ref;

    setvbuf(stdout, NULL, _IOLBF, 0);

    CHECK(dyn_par_plan(0, 1, 0) == 1, "zero units must plan 1 worker");
    CHECK(dyn_par_plan(99, 100, 0) == 1, "below one worker's share must plan 1");
    CHECK(dyn_par_plan(1u << 30, 1, 0) == maxw, "unbounded work plans max threads (%u)", dyn_par_plan(1u << 30, 1, 0));
    CHECK(dyn_par_plan(1u << 30, 1, 3) <= 3, "cap 3 must bound the plan");
    CHECK(dyn_par_plan(400, 100, 0) == (maxw < 4 ? maxw : 4), "400 units / 100 per worker plans 4");
    dyn_pool_set_default_threads(2);
    CHECK(dyn_par_plan(1u << 30, 1, 0) <= 2, "--io-threads 2 must bound the plan");
    dyn_pool_set_default_threads(0);
    CHECK(dyn_par_plan(1u << 30, 1, 0) == 1, "inline-only must plan 1");
    {
        gen_ctx g;
        memset(&g, 0, sizeof g);
        g.grid = calloc((size_t)ROUNDS * MAXW, sizeof(unsigned));
        CHECK(dyn_par_run(8, gen_fn, &g) == 1, "inline-only must run serially");
        free(g.grid);
    }
    dyn_pool_set_default_threads(maxw);

    for (i = 1; i <= 8; i++)
        run_generations(i, -1, i <= maxw ? i : maxw);
    run_generations(8, 0, 1);
    run_generations(8, 1, maxw >= 2 ? 2 : 1);
    run_generations(8, 2, maxw >= 3 ? 3 : maxw);

    ref = sum_with(1, -1, &used);
    CHECK(used == 1, "one worker requested, %u used", used);
    for (i = 2; i <= 8; i++) {
        uint64_t v = sum_with(i, -1, &used);
        CHECK(v == ref, "partitioned sum with %u workers differs", used);
    }
    {
        int lim;
        for (lim = 0; lim <= 2; lim++) {
            uint64_t v = sum_with(8, lim, &used);
            CHECK(used == ((unsigned)lim + 1 < maxw ? (unsigned)lim + 1 : maxw), "spawn limit %d used %u workers", lim, used);
            CHECK(v == ref, "spawn limit %d changed the answer", lim);
        }
    }

    dyn_par_run(4, nested_outer, NULL);
    CHECK(atomic_load(&nested_plan) == (maxw < 4 ? maxw : 4), "every nested plan must be 1 (sum %u)", atomic_load(&nested_plan));
    CHECK(atomic_load(&nested_used) == (maxw < 4 ? maxw : 4), "every nested run must be serial (sum %u)", atomic_load(&nested_used));

    dyn_par_reset_peak();
    {
        pthread_t t[4];
        for (i = 0; i < 4; i++)
            pthread_create(&t[i], NULL, budget_caller, NULL);
        for (i = 0; i < 4; i++)
            pthread_join(t[i], NULL);
    }
    dyn_par_stats(&st);
    CHECK(atomic_load(&budget_bad) == 0, "live helpers exceeded the budget %u times", atomic_load(&budget_bad));
    CHECK(st.live_peak <= st.budget, "peak %u helpers over budget %u", st.live_peak, st.budget);
    CHECK(maxw < 2 || st.live_peak >= 1, "concurrent callers never got a helper");
    CHECK(st.live == 0, "%u helpers still counted live after every join", st.live);
    CHECK(st.spawn_failures >= 3, "spawn-limit injections must be counted (%llu)", (unsigned long long)st.spawn_failures);

    printf("test_par: maxw=%u spawned=%llu runs=%llu serial=%llu peak=%u budget=%u\n", maxw,
        (unsigned long long)st.spawned, (unsigned long long)st.runs, (unsigned long long)st.serial_runs,
        st.live_peak, st.budget);
    printf(fails ? "test_par: %d FAILED\n" : "test_par: ok\n", fails);
    return fails ? 1 : 0;
}
