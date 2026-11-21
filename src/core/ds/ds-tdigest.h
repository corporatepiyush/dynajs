#ifndef DS_TDIGEST_H
#define DS_TDIGEST_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double m, w;
} dyn_tdigest_cent;

typedef struct {
    double delta;
    dyn_tdigest_cent* c;
    dyn_tdigest_cent* view;
    dyn_tdigest_cent* vbuf;
    uint32_t n, nbuf, cap;
    uint32_t view_n, view_ok, vbuf_n;
    double total, min, max, sum;
    double view_total;
} dyn_tdigest_t;

#define DYN_TDIGEST_MIN_DELTA 10.0
#define DYN_TDIGEST_MAX_DELTA 10000.0

int dyn_tdigest_init(dyn_tdigest_t* t, double delta);

void dyn_tdigest_free(dyn_tdigest_t* t);

void dyn_tdigest_compress(dyn_tdigest_t* t);

void dyn_tdigest_add(dyn_tdigest_t* t, double x, double w);

double dyn_tdigest_quantile(dyn_tdigest_t* t, double q);

double dyn_tdigest_cdf(dyn_tdigest_t* t, double x);

const dyn_tdigest_cent* dyn_tdigest_view(dyn_tdigest_t* t, uint32_t* n, double* total);

#ifdef __cplusplus
}
#endif

#endif
