#ifndef METRICS_INTERNAL_H
#define METRICS_INTERNAL_H

#include "dyna-nat.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_METRICS)

#define MET_MAX 256
#define MET_NBUCKET 6
#define MET_NAME 56
#define MET_LABELS 96

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

enum { MET_FREE = 0,
    MET_COUNTER,
    MET_GAUGE,
    MET_HISTOGRAM };

typedef struct {
    _Atomic uint64_t v;
    _Atomic uint64_t sum_bits;
    _Atomic uint64_t bucket[MET_NBUCKET];
} met_hot_t;

_Static_assert(sizeof(met_hot_t) == 64,
    "met_hot_t must be exactly one cache line: two counters sharing "
    "a line is the false sharing this layout exists to prevent");

typedef struct {
    char name[MET_NAME];
    char labels[MET_LABELS];
    uint8_t kind;
    uint8_t n_bounds;
    double bounds[MET_NBUCKET];
} met_cold_t;

extern met_hot_t met_hot[256];

extern met_cold_t met_cold[256];

extern _Atomic(uint32_t) met_used;

extern pthread_mutex_t met_lock;

static inline uint64_t met_dtou(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static inline double met_utod(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

int met_name_ok(const char* s, size_t n);

int met_slot(const char* name, const char* labels, int kind,
    const double* bounds, uint8_t n_bounds);

int met_labels(JSContext* ctx, JSValueConst v, char* out, size_t cap);

JSValue dyn_met_observe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

void met_record_slot(int kind, int slot, double value);

#define MET_HANDLE_SLOT_BITS 9

_Static_assert(MET_MAX < (1 << MET_HANDLE_SLOT_BITS), "a metric slot index must fit the handle");

char* met_scrape_buf(size_t* out_len);

JSValue dyn_met_scrape(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
