#ifndef DYN_PAR_H
#define DYN_PAR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_par dyn_par_t;

typedef void (*dyn_par_fn)(void* arg, unsigned worker, unsigned nworkers,
    dyn_par_t* par);

typedef struct {
    uint64_t runs;
    uint64_t serial_runs;
    uint64_t spawned;
    uint64_t spawn_failures;
    unsigned live;
    unsigned live_peak;
    unsigned budget;
} dyn_par_stats_t;

unsigned dyn_par_plan(size_t units, size_t min_units_per_worker, unsigned cap);

unsigned dyn_par_run(unsigned nworkers, dyn_par_fn fn, void* arg);

void dyn_par_barrier(dyn_par_t* par);

void dyn_par_stats(dyn_par_stats_t* out);

void dyn_par_reset_peak(void);

void dyn_par_set_spawn_limit(int limit);

#ifdef __cplusplus
}
#endif

#endif
